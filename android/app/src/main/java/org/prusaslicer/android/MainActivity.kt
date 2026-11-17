package org.prusaslicer.android

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.View
import android.widget.ArrayAdapter
import android.widget.AutoCompleteTextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.FileProvider
import androidx.lifecycle.lifecycleScope
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.prusaslicer.android.databinding.ActivityMainBinding
import java.io.File

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    private var modelFile: File? = null
    private var outputFile: File? = null
    private var printers: List<Profiles.Printer> = emptyList()
    private var printProfiles: List<Profiles.PrintProfile> = emptyList()
    private var ready = false
    private var busy = false

    private val openModel = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) importModel(uri)
    }

    private val saveOutput = registerForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri != null) exportOutput(uri)
    }

    private val prefs by lazy { getSharedPreferences("selection", MODE_PRIVATE) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.openButton.setOnClickListener { openModel.launch(arrayOf("*/*")) }
        binding.sliceButton.setOnClickListener { slice() }
        binding.saveButton.setOnClickListener { outputFile?.let { saveOutput.launch(it.name) } }
        binding.shareButton.setOnClickListener { shareOutput() }

        binding.printerDropdown.setOnItemClickListener { _, _, position, _ ->
            onPrinterSelected(printers[position].name)
        }
        binding.printDropdown.setOnItemClickListener { _, _, position, _ ->
            onPrintSelected(printProfiles[position])
        }
        binding.materialDropdown.setOnItemClickListener { _, _, _, _ ->
            prefs.edit().putString("material", binding.materialDropdown.text.toString()).apply()
        }

        initialize()
        handleIncomingIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIncomingIntent(intent)
    }

    private fun handleIncomingIntent(intent: Intent?) {
        val uri: Uri? = when (intent?.action) {
            Intent.ACTION_VIEW -> intent.data
            Intent.ACTION_SEND ->
                if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
                else @Suppress("DEPRECATION") intent.getParcelableExtra(Intent.EXTRA_STREAM)
            else -> null
        }
        if (uri != null) importModel(uri)
    }

    private fun initialize() {
        setBusy(true, getString(R.string.status_installing))
        lifecycleScope.launch {
            val result = withContext(NativeSlicer.dispatcher) {
                runCatching {
                    val resources = ResourceInstaller.install(this@MainActivity)
                    NativeSlicer.init(resources, File(filesDir, "data"), File(cacheDir, "native"))
                    val json = File(cacheDir, "printers.json")
                    json.delete()
                    val run = NativeSlicer.run(
                        listOf("--query-printer-models", "--output", json.absolutePath),
                        File(cacheDir, "query.log"),
                    )
                    if (!json.exists()) error("Querying printers failed:\n${run.output}")
                    Profiles.parsePrinters(json.readText())
                }
            }
            setBusy(false)
            result.onSuccess { list ->
                printers = list.sortedWith(compareBy({ it.vendor }, { it.name }))
                binding.versionText.text = NativeSlicer.nativeVersion()
                binding.printerDropdown.setAdapter(
                    ArrayAdapter(this@MainActivity, android.R.layout.simple_list_item_1, printers.map { it.name })
                )
                ready = true
                val saved = prefs.getString("printer", null)
                val initial = printers.firstOrNull { it.name == saved } ?: printers.firstOrNull()
                if (initial != null) {
                    binding.printerDropdown.setText(initial.name, false)
                    onPrinterSelected(initial.name)
                }
                if (printers.isEmpty()) showLog(getString(R.string.no_printers))
            }.onFailure { showLog(it.message ?: it.toString()) }
            updateButtons()
        }
    }

    private fun onPrinterSelected(printer: String) {
        prefs.edit().putString("printer", printer).apply()
        setBusy(true, getString(R.string.status_loading_profiles))
        lifecycleScope.launch {
            val result = withContext(NativeSlicer.dispatcher) {
                runCatching {
                    val json = File(cacheDir, "profiles.json")
                    json.delete()
                    val run = NativeSlicer.run(
                        listOf("--query-print-tool-filament-profiles", "--printer-profile", printer, "--output", json.absolutePath),
                        File(cacheDir, "query.log"),
                    )
                    if (!json.exists()) error("Querying profiles failed:\n${run.output}")
                    Profiles.parsePrintProfiles(json.readText())
                }
            }
            setBusy(false)
            result.onSuccess { list ->
                printProfiles = list
                binding.printDropdown.setAdapter(
                    ArrayAdapter(this@MainActivity, android.R.layout.simple_list_item_1, list.map { it.name })
                )
                val saved = prefs.getString("print", null)
                val initial = list.firstOrNull { it.name == saved }
                    ?: list.firstOrNull { it.name.contains("0.20mm") }
                    ?: list.firstOrNull()
                binding.printDropdown.setText(initial?.name ?: "", false)
                initial?.let { onPrintSelected(it) }
            }.onFailure { showLog(it.message ?: it.toString()) }
            updateButtons()
        }
    }

    private fun onPrintSelected(print: Profiles.PrintProfile) {
        prefs.edit().putString("print", print.name).apply()
        binding.materialDropdown.setAdapter(
            ArrayAdapter(this, android.R.layout.simple_list_item_1, print.materials)
        )
        val saved = prefs.getString("material", null)
        val material = print.materials.firstOrNull { it == saved }
            ?: print.materials.firstOrNull { it.contains("PLA") }
            ?: print.materials.firstOrNull()
        binding.materialDropdown.setText(material ?: "", false)
        updateButtons()
    }

    private fun importModel(uri: Uri) {
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching {
                    val name = displayName(uri) ?: "model.stl"
                    val dir = File(cacheDir, "input").apply { deleteRecursively(); mkdirs() }
                    val target = File(dir, name.replace('/', '_'))
                    contentResolver.openInputStream(uri).use { input ->
                        requireNotNull(input) { "Cannot open $uri" }
                        target.outputStream().use { input.copyTo(it) }
                    }
                    target
                }
            }
            result.onSuccess {
                modelFile = it
                outputFile = null
                binding.modelText.text = it.name
            }.onFailure { showLog(it.message ?: it.toString()) }
            updateButtons()
        }
    }

    private fun displayName(uri: Uri): String? =
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        } ?: uri.lastPathSegment?.substringAfterLast('/')

    private fun slice() {
        val model = modelFile ?: return
        val args = mutableListOf("--export-gcode")
        val printer = binding.printerDropdown.text.toString()
        val print = binding.printDropdown.text.toString()
        val material = binding.materialDropdown.text.toString()
        if (printer.isNotEmpty()) args += listOf("--printer-profile", printer)
        if (print.isNotEmpty()) args += listOf("--print-profile", print)
        if (material.isNotEmpty()) args += listOf("--material-profile", material)
        val outDir = File(cacheDir, "output").apply { deleteRecursively(); mkdirs() }
        args += listOf("--output", outDir.absolutePath + "/")
        args += model.absolutePath

        setBusy(true, getString(R.string.status_slicing))
        outputFile = null
        lifecycleScope.launch {
            val started = System.currentTimeMillis()
            val run = withContext(NativeSlicer.dispatcher) {
                runCatching { NativeSlicer.run(args, File(cacheDir, "slice.log")) }
            }
            setBusy(false)
            run.onSuccess { result ->
                val produced = outDir.listFiles()?.filter { it.isFile }?.maxByOrNull { it.lastModified() }
                val seconds = (System.currentTimeMillis() - started) / 1000.0
                if (result.isSuccess && produced != null) {
                    outputFile = produced
                    binding.outputText.text = getString(R.string.slice_done, produced.name, produced.length() / 1024, seconds)
                    showLog(result.output)
                    Snackbar.make(binding.root, R.string.slice_ok, Snackbar.LENGTH_SHORT).show()
                } else {
                    binding.outputText.text = getString(R.string.slice_failed, result.exitCode)
                    showLog(result.output)
                }
            }.onFailure { showLog(it.message ?: it.toString()) }
            updateButtons()
        }
    }

    private fun exportOutput(uri: Uri) {
        val file = outputFile ?: return
        lifecycleScope.launch(Dispatchers.IO) {
            val result = runCatching {
                contentResolver.openOutputStream(uri).use { out ->
                    requireNotNull(out) { "Cannot write $uri" }
                    file.inputStream().use { it.copyTo(out) }
                }
            }
            withContext(Dispatchers.Main) {
                Snackbar.make(
                    binding.root,
                    if (result.isSuccess) getString(R.string.saved) else (result.exceptionOrNull()?.message ?: "Error"),
                    Snackbar.LENGTH_SHORT,
                ).show()
            }
        }
    }

    private fun shareOutput() {
        val file = outputFile ?: return
        val uri = FileProvider.getUriForFile(this, "$packageName.files", file)
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "application/octet-stream"
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivity(Intent.createChooser(send, getString(R.string.share)))
    }

    private fun showLog(text: String) {
        binding.logText.text = text.trim()
        binding.logCard.visibility = if (text.isBlank()) View.GONE else View.VISIBLE
    }

    private fun setBusy(isBusy: Boolean, status: String = "") {
        busy = isBusy
        binding.progress.visibility = if (isBusy) View.VISIBLE else View.GONE
        binding.statusText.text = status
        binding.statusText.visibility = if (isBusy) View.VISIBLE else View.GONE
        updateButtons()
    }

    private fun updateButtons() {
        binding.openButton.isEnabled = !busy
        binding.sliceButton.isEnabled = ready && !busy && modelFile != null && binding.printerDropdown.text.isNotEmpty()
        binding.saveButton.isEnabled = !busy && outputFile != null
        binding.shareButton.isEnabled = !busy && outputFile != null
    }
}
