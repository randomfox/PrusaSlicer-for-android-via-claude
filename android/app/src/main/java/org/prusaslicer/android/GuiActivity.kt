package org.prusaslicer.android

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.FileObserver
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.provider.OpenableColumns
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.FileProvider
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.prusaslicer.android.databinding.ActivityGuiBinding
import java.io.File

/** The full PrusaSlicer user interface (plater + G-code preview) rendered with OpenGL ES. */
class GuiActivity : AppCompatActivity() {

    private lateinit var binding: ActivityGuiBinding
    private var surface: PrusaSlicerSurfaceView? = null
    private val pendingOpen = mutableListOf<Uri>()
    private val pendingUrls = mutableListOf<String>()
    private var outputObserver: FileObserver? = null
    private val prefs by lazy { getSharedPreferences("gui", MODE_PRIVATE) }
    private val mainHandler = Handler(Looper.getMainLooper())

    // File picker requested by PrusaSlicer (IMobileHost::pick_files).
    private var pickRequestId = -1
    private val pickOne = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        deliverPicked(listOfNotNull(uri))
    }
    private val pickMany = registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        deliverPicked(uris)
    }

    // "Open" from the toolbar.
    private val openModels = registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) openUris(uris)
    }

    private var fileToSave: File? = null
    private val saveOutput = registerForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val file = fileToSave ?: return@registerForActivityResult
        if (uri != null) copyToUri(file, uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityGuiBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setSupportActionBar(binding.toolbar)

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                // Closes PrusaSlicer's dialogs/popups like the Escape key.
                surface?.sendBackAsEscape()
            }
        })

        collectIncoming(intent)
        checkPreviousCrash()
        startNative()
    }

    // ---- Crash reports -------------------------------------------------------------------

    private val sessionMarker by lazy { File(filesDir, "gui_session.marker") }
    private val crashFile by lazy { File(filesDir, "data/last_crash.txt") }

    override fun onStart() {
        super.onStart()
        sessionMarker.writeText(System.currentTimeMillis().toString())
    }

    override fun onStop() {
        sessionMarker.delete()
        super.onStop()
    }

    /** The previous session ended while the GUI was in the foreground: offer the log. */
    private fun checkPreviousCrash() {
        val crashed = sessionMarker.exists() || (crashFile.exists() && crashFile.length() > 0)
        if (!crashed) return
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.crash_title)
            .setMessage(R.string.crash_message)
            .setPositiveButton(R.string.share_log) { _, _ -> shareLog() }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun shareLog() {
        val report = File(cacheDir, "output/prusaslicer-android-report.txt")
        report.parentFile?.mkdirs()
        report.bufferedWriter().use { out ->
            out.write("PrusaSlicer for Android ${packageManager.getPackageInfo(packageName, 0).versionName}\n")
            out.write("Device: ${Build.MANUFACTURER} ${Build.MODEL}, Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})\n\n")
            if (crashFile.exists() && crashFile.length() > 0) {
                out.write("==== Native crash ====\n")
                out.write(crashFile.readText())
                out.write("\n")
            }
            File(filesDir, "data").walkTopDown()
                .filter { it.name == "log.txt" && it.parentFile?.name == "shared_runtime" }
                .forEach { log ->
                    out.write("==== ${log.name} (last 400 lines) ====\n")
                    log.readLines().takeLast(400).forEach { out.write(it); out.write("\n") }
                }
        }
        val uri = FileProvider.getUriForFile(this, "$packageName.files", report)
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivity(Intent.createChooser(send, getString(R.string.share_log)))
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        collectIncoming(intent)
        if (surface != null) flushPendingOpen()
    }

    // Android filters multicast packets unless an app holds this lock: needed to discover
    // printers (PrusaLink, OctoPrint) with Bonjour / mDNS.
    private val multicastLock by lazy {
        (applicationContext.getSystemService(WIFI_SERVICE) as android.net.wifi.WifiManager)
            .createMulticastLock("PrusaSlicer printer discovery").apply { setReferenceCounted(false) }
    }

    override fun onResume() {
        super.onResume()
        surface?.onResume()
        runCatching { multicastLock.acquire() }
    }

    override fun onPause() {
        runCatching { multicastLock.release() }
        surface?.onPause()
        super.onPause()
    }

    override fun onDestroy() {
        outputObserver?.stopWatching()
        super.onDestroy()
    }

    // ---- Start-up ------------------------------------------------------------------------

    private fun startNative() {
        binding.loading.visibility = View.VISIBLE
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching {
                    val resources = ResourceInstaller.install(this@GuiActivity)
                    android.util.Log.i("PrusaSlicerGui", "Resources ready, starting native code")
                    val rc = GuiNative.nativeCreate(
                        resources.absolutePath,
                        File(filesDir, "data").apply { mkdirs() }.absolutePath,
                        File(cacheDir, "native").apply { mkdirs() }.absolutePath,
                        host,
                    )
                    check(rc == 0) { "Native initialization failed ($rc)" }
                }
            }
            result.onSuccess { createSurface() }.onFailure {
                binding.loadingText.text = it.message ?: it.toString()
            }
        }
    }

    private fun createSurface() {
        val view = PrusaSlicerSurfaceView(this) { onSurfaceReady() }
        view.uiScale = resources.displayMetrics.density * uiZoom()
        binding.surfaceContainer.addView(view)
        surface = view
    }

    private fun onSurfaceReady() {
        binding.loading.visibility = View.GONE
        flushPendingOpen()
        surface?.queueEvent {
            val dir = GuiNative.nativeOutputDir()
            mainHandler.post { watchOutputDir(dir) }
        }
    }

    fun onContextLost() {
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.app_name)
            .setMessage(R.string.context_lost)
            .setCancelable(false)
            .setPositiveButton(R.string.restart) { _, _ -> restartAfterContextLoss() }
            .show()
    }

    private fun restartAfterContextLoss() {
        val intent = Intent(this, GuiActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        startActivity(intent)
        finishAffinity()
        Process.killProcess(Process.myPid())
    }

    // ---- Opening files -------------------------------------------------------------------

    private fun collectIncoming(intent: Intent?) {
        val data = intent?.data
        if (intent?.action == Intent.ACTION_VIEW && data?.scheme == "prusaslicer") {
            // Prusa Account login redirect or "Open in PrusaSlicer" link.
            val url = data.toString()
            if (surface != null) surface?.let { it.queueEvent { GuiNative.nativeOpenUrl(url) }; it.requestRender() }
            else pendingUrls += url
            return
        }
        when (intent?.action) {
            Intent.ACTION_VIEW -> intent.data?.let { pendingOpen += it }
            Intent.ACTION_SEND -> {
                val uri = if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
                else @Suppress("DEPRECATION") intent.getParcelableExtra(Intent.EXTRA_STREAM)
                uri?.let { pendingOpen += it }
            }
        }
    }

    private fun flushPendingOpen() {
        if (pendingUrls.isNotEmpty()) {
            val urls = pendingUrls.toList()
            pendingUrls.clear()
            surface?.queueEvent { urls.forEach { GuiNative.nativeOpenUrl(it) } }
        }
        if (pendingOpen.isEmpty()) return
        val uris = pendingOpen.toList()
        pendingOpen.clear()
        openUris(uris)
    }

    private fun openUris(uris: List<Uri>) {
        lifecycleScope.launch {
            val files = withContext(Dispatchers.IO) { copyToInbox(uris) }
            if (files.isNotEmpty()) surface?.openFiles(files.map { it.absolutePath })
        }
    }

    private fun copyToInbox(uris: List<Uri>): List<File> {
        val dir = File(cacheDir, "inbox/${System.currentTimeMillis()}").apply { mkdirs() }
        return uris.mapNotNull { uri ->
            runCatching {
                val name = displayName(uri) ?: "model.stl"
                val target = File(dir, name.replace('/', '_'))
                contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input)
                    target.outputStream().use { input.copyTo(it) }
                }
                target
            }.getOrNull()
        }
    }

    private fun displayName(uri: Uri): String? =
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        } ?: uri.lastPathSegment?.substringAfterLast('/')

    private fun deliverPicked(uris: List<Uri>) {
        val id = pickRequestId
        pickRequestId = -1
        if (id < 0) return
        lifecycleScope.launch {
            val files = withContext(Dispatchers.IO) { copyToInbox(uris) }
            surface?.answerFiles(id, files.map { it.absolutePath })
        }
    }

    // ---- Output files --------------------------------------------------------------------

    private fun watchOutputDir(path: String) {
        if (path.isEmpty()) return
        val dir = File(path).apply { mkdirs() }
        val mask = FileObserver.CLOSE_WRITE or FileObserver.MOVED_TO
        val observer = if (Build.VERSION.SDK_INT >= 29) {
            object : FileObserver(dir, mask) {
                override fun onEvent(event: Int, name: String?) = onOutputFile(dir, name)
            }
        } else {
            @Suppress("DEPRECATION")
            object : FileObserver(dir.absolutePath, mask) {
                override fun onEvent(event: Int, name: String?) = onOutputFile(dir, name)
            }
        }
        observer.startWatching()
        outputObserver = observer
    }

    private var lastOffered: Pair<String, Long>? = null

    private fun onOutputFile(dir: File, name: String?) {
        if (name == null || name.startsWith(".") || name.endsWith(".tmp")) return
        val file = File(dir, name)
        mainHandler.postDelayed({
            if (!file.exists()) return@postDelayed
            val key = file.absolutePath to file.lastModified()
            if (key == lastOffered) return@postDelayed
            lastOffered = key
            offerFile(file)
        }, 300)
    }

    private fun offerFile(file: File) {
        Snackbar.make(binding.root, getString(R.string.file_ready, file.name), Snackbar.LENGTH_INDEFINITE)
            .setAction(R.string.save_or_share) {
                MaterialAlertDialogBuilder(this)
                    .setTitle(file.name)
                    .setItems(arrayOf(getString(R.string.save), getString(R.string.share))) { _, which ->
                        if (which == 0) {
                            fileToSave = file
                            saveOutput.launch(file.name)
                        } else {
                            shareFile(file)
                        }
                    }
                    .show()
            }
            .show()
    }

    private fun shareFile(file: File) {
        val uri = FileProvider.getUriForFile(this, "$packageName.files", file)
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "application/octet-stream"
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivity(Intent.createChooser(send, getString(R.string.share)))
    }

    private fun copyToUri(file: File, uri: Uri) {
        lifecycleScope.launch(Dispatchers.IO) {
            val result = runCatching {
                contentResolver.openOutputStream(uri).use { out ->
                    requireNotNull(out)
                    file.inputStream().use { it.copyTo(out) }
                }
            }
            withContext(Dispatchers.Main) {
                Toast.makeText(
                    this@GuiActivity,
                    if (result.isSuccess) getString(R.string.saved) else (result.exceptionOrNull()?.message ?: "Error"),
                    Toast.LENGTH_SHORT,
                ).show()
            }
        }
    }

    // ---- Toolbar -------------------------------------------------------------------------

    private fun uiZoom(): Float {
        // PrusaSlicer's layout needs roughly 1000 x 480 UI pixels (landscape).
        val default = if (resources.configuration.smallestScreenWidthDp >= 600) 1.0f else 0.8f
        return prefs.getFloat("ui_zoom", default)
    }

    private fun setUiZoom(zoom: Float) {
        val clamped = zoom.coerceIn(0.5f, 2.0f)
        prefs.edit().putFloat("ui_zoom", clamped).apply()
        surface?.applyUiScale(resources.displayMetrics.density * clamped)
        Toast.makeText(this, getString(R.string.ui_zoom_value, (clamped * 100).toInt()), Toast.LENGTH_SHORT).show()
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.gui, menu)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean = when (item.itemId) {
        R.id.action_open -> { openModels.launch(arrayOf("*/*")); true }
        R.id.action_zoom_in -> { setUiZoom(uiZoom() + 0.1f); true }
        R.id.action_zoom_out -> { setUiZoom(uiZoom() - 0.1f); true }
        R.id.action_quick_slice -> { startActivity(Intent(this, MainActivity::class.java)); true }
        R.id.action_share_log -> { shareLog(); true }
        else -> super.onOptionsItemSelected(item)
    }

    // ---- Native host ---------------------------------------------------------------------

    private val host = object : GuiHost {
        override fun requestRender() {
            surface?.requestRender()
        }

        override fun showMessage(kind: Int, title: String, text: String) {
            mainHandler.post {
                MaterialAlertDialogBuilder(this@GuiActivity)
                    .setTitle(title.ifEmpty { getString(R.string.app_name) })
                    .setMessage(text)
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
            }
        }

        override fun askQuestion(id: Int, title: String, text: String, buttons: Array<String>) {
            mainHandler.post {
                val builder = MaterialAlertDialogBuilder(this@GuiActivity)
                    .setTitle(title.ifEmpty { getString(R.string.app_name) })
                    .setMessage(text)
                    .setOnCancelListener { surface?.answerQuestion(id, -1) }
                buttons.getOrNull(0)?.let { label -> builder.setPositiveButton(label) { _, _ -> surface?.answerQuestion(id, 0) } }
                buttons.getOrNull(1)?.let { label -> builder.setNegativeButton(label) { _, _ -> surface?.answerQuestion(id, 1) } }
                buttons.getOrNull(2)?.let { label -> builder.setNeutralButton(label) { _, _ -> surface?.answerQuestion(id, 2) } }
                builder.show()
            }
        }

        override fun pickFiles(id: Int, kind: Int, title: String, wildcards: String) {
            mainHandler.post {
                pickRequestId = id
                if (kind == 1) pickMany.launch(arrayOf("*/*")) else pickOne.launch(arrayOf("*/*"))
            }
        }

        override fun offerOutputFile(path: String) {
            mainHandler.post { offerFile(File(path)) }
        }

        override fun openUrl(url: String) {
            mainHandler.post {
                runCatching { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
            }
        }

        override fun setTextInputActive(active: Boolean) {
            mainHandler.post { surface?.setSoftKeyboardVisible(active) }
        }

        override fun openWebView(id: Int, title: String, url: String, handlerNames: Array<String>) {
            mainHandler.post {
                val dialog = WebViewDialog(
                    this@GuiActivity, id, title.ifEmpty { getString(R.string.app_name) }, url, handlerNames.toList(),
                    postNative = { block -> surface?.let { it.queueEvent(block); it.requestRender() } },
                    callNative = { block -> callOnGlThread(block) },
                )
                webViews[id] = dialog
                dialog.start()
            }
        }

        override fun webViewCommand(id: Int, command: Int, data: String, extra: String) {
            mainHandler.post { webViews[id]?.command(command, data, extra) }
        }
    }

    private val webViews = mutableMapOf<Int, WebViewDialog>()

    fun onWebViewClosed(id: Int) {
        webViews.remove(id)
    }

    /** Runs [block] on the GL thread (PrusaSlicer's main thread) and waits for its result. */
    private fun callOnGlThread(block: () -> Boolean): Boolean {
        val view = surface ?: return true
        val latch = java.util.concurrent.CountDownLatch(1)
        var result = true
        view.queueEvent {
            result = runCatching(block).getOrDefault(true)
            latch.countDown()
        }
        view.requestRender()
        latch.await(3, java.util.concurrent.TimeUnit.SECONDS)
        return result
    }
}
