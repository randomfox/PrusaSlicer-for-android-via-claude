package org.prusaslicer.android

import android.annotation.SuppressLint
import android.app.Dialog
import android.content.Intent
import android.graphics.Bitmap
import android.net.Uri
import android.view.ViewGroup
import android.webkit.CookieManager
import android.webkit.JavascriptInterface
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.LinearLayout
import androidx.webkit.WebViewCompat
import androidx.webkit.WebViewFeature
import com.google.android.material.appbar.MaterialToolbar

/**
 * Full screen web view driven by PrusaSlicer's browser logic (Prusa Account login, Prusa
 * Connect, Printables). Mirrors the desktop WebViewDialog: page events go to native code,
 * which answers with commands ([command]).
 *
 * @param callNative runs a block on the GL (PrusaSlicer main) thread and waits for the result.
 */
@SuppressLint("SetJavaScriptEnabled")
class WebViewDialog(
    private val activity: GuiActivity,
    private val id: Int,
    title: String,
    private val initialUrl: String,
    private val handlerNames: List<String>,
    private val postNative: (() -> Unit) -> Unit,
    private val callNative: (() -> Boolean) -> Boolean,
) : Dialog(activity, android.R.style.Theme_DeviceDefault_NoActionBar) {

    private val webView = WebView(activity)
    private var closedByNative = false
    private val userScripts = mutableListOf<String>()

    init {
        val toolbar = MaterialToolbar(activity).apply {
            this.title = title
            setNavigationIcon(androidx.appcompat.R.drawable.abc_ic_ab_back_material)
            setNavigationOnClickListener { dismiss() }
        }
        val layout = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            addView(toolbar, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(webView, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        setContentView(layout)

        webView.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            allowFileAccess = true // PrusaSlicer's loading/error pages are local files
            @Suppress("DEPRECATION")
            allowFileAccessFromFileURLs = true
        }
        CookieManager.getInstance().setAcceptThirdPartyCookies(webView, true)

        // Each message handler is a JavaScript object with postMessage(); objects are passed
        // as JSON, like WebKit does on the desktop.
        handlerNames.forEachIndexed { index, name ->
            webView.addJavascriptInterface(Bridge(), "__prusaslicer_bridge$index")
            addDocumentStartScript(
                """
                (function() {
                    var bridge = window.__prusaslicer_bridge$index;
                    var handler = { postMessage: function(m) { bridge.postMessage(typeof m === 'string' ? m : JSON.stringify(m)); } };
                    window['$name'] = handler;
                    window.webkit = window.webkit || {};
                    window.webkit.messageHandlers = window.webkit.messageHandlers || {};
                    window.webkit.messageHandlers['$name'] = handler;
                })();
                """.trimIndent()
            )
        }

        webView.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
                if (!request.isForMainFrame) return false
                val url = request.url.toString()
                val current = view.url ?: ""
                val allow = callNative { GuiNative.nativeWebViewNavigation(id, url, current) }
                if (!allow) return true
                // Links PrusaSlicer does not handle itself (custom schemes): let Android open them.
                if (!url.startsWith("http") && !url.startsWith("file:") && !url.startsWith("about:")) {
                    return true
                }
                return false
            }

            override fun onPageStarted(view: WebView, url: String, favicon: Bitmap?) {
                if (!WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT)) {
                    userScripts.forEach { view.evaluateJavascript(it, null) }
                }
            }

            override fun onPageFinished(view: WebView, url: String) {
                postNative { GuiNative.nativeWebViewLoaded(id, url) }
            }

            override fun onReceivedError(view: WebView, request: WebResourceRequest, error: WebResourceError) {
                if (request.isForMainFrame) {
                    val description = "${error.errorCode} ${error.description}"
                    postNative { GuiNative.nativeWebViewError(id, description) }
                }
            }
        }

        setOnDismissListener {
            if (!closedByNative) postNative { GuiNative.nativeWebViewDismissed(id) }
            webView.destroy()
            activity.onWebViewClosed(id)
        }
    }

    fun start() {
        show()
        webView.loadUrl(initialUrl)
    }

    private fun addDocumentStartScript(script: String) {
        userScripts += script
        if (WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT)) {
            WebViewCompat.addDocumentStartJavaScript(webView, script, setOf("*"))
        }
    }

    /** Executes a command of PrusaSlicer's browser logic (IMobileHost::WebViewCommand). */
    fun command(command: Int, data: String, extra: String) {
        when (command) {
            GuiNative.WEB_LOAD_URL -> webView.loadUrl(data)
            GuiNative.WEB_LOAD_URL_WITH_TOKEN ->
                webView.loadUrl(data, if (extra.isEmpty()) emptyMap() else mapOf("Authorization" to "External $extra"))
            GuiNative.WEB_RUN_SCRIPT -> webView.evaluateJavascript(data, null)
            GuiNative.WEB_ADD_USER_SCRIPT -> {
                addDocumentStartScript(data)
                webView.evaluateJavascript(data, null)
            }
            GuiNative.WEB_DELETE_COOKIES -> deleteCookies(data)
            GuiNative.WEB_RELOAD -> webView.reload()
            GuiNative.WEB_OPEN_EXTERNAL ->
                runCatching { activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(data))) }
            GuiNative.WEB_CLOSE -> {
                closedByNative = true
                dismiss()
            }
        }
    }

    private fun deleteCookies(url: String) {
        val manager = CookieManager.getInstance()
        val cookies = manager.getCookie(url) ?: return
        cookies.split(";").map { it.substringBefore('=').trim() }.filter { it.isNotEmpty() }.forEach { name ->
            manager.setCookie(url, "$name=; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT")
        }
        manager.flush()
    }

    private inner class Bridge {
        @JavascriptInterface
        fun postMessage(message: String) {
            postNative { GuiNative.nativeWebViewScriptMessage(id, message) }
        }
    }
}
