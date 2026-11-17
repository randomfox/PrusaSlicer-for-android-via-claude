package org.prusaslicer.android

/**
 * Bindings of the PrusaSlicer GUI in libprusaslicer.so (src/slic3r-app-android/src/PrusaSlicerGuiJni.cpp).
 * Everything except [nativeCreate] must be called on the GL render thread.
 */
object GuiNative {
    init {
        System.loadLibrary("prusaslicer")
    }

    const val TOUCH_DOWN = 0
    const val TOUCH_MOVE = 1
    const val TOUCH_UP = 2
    const val TOUCH_CANCEL = 3

    @JvmStatic external fun nativeCreate(resourcesDir: String, dataDir: String, cacheDir: String, host: GuiHost): Int
    @JvmStatic external fun nativeSurfaceCreated(): Int
    @JvmStatic external fun nativeSurfaceChanged(width: Int, height: Int, scale: Float)
    @JvmStatic external fun nativeDrawFrame(): Boolean
    @JvmStatic external fun nativeTouch(action: Int, xs: FloatArray, ys: FloatArray)
    @JvmStatic external fun nativeMouseButton(button: Int, down: Boolean, x: Float, y: Float)
    @JvmStatic external fun nativeMouseMove(x: Float, y: Float)
    @JvmStatic external fun nativeMouseWheel(x: Float, y: Float, dx: Float, dy: Float)
    @JvmStatic external fun nativeText(text: String)
    @JvmStatic external fun nativeKey(keyCode: Int, down: Boolean)
    @JvmStatic external fun nativeOpenFiles(paths: Array<String>)
    @JvmStatic external fun nativeQuestionAnswered(id: Int, index: Int)
    @JvmStatic external fun nativeFilesPicked(id: Int, paths: Array<String>)
    @JvmStatic external fun nativeOutputDir(): String
    @JvmStatic external fun nativeOpenUrl(url: String)
    @JvmStatic external fun nativeWebViewNavigation(id: Int, url: String, currentUrl: String): Boolean
    @JvmStatic external fun nativeWebViewLoaded(id: Int, url: String)
    @JvmStatic external fun nativeWebViewScriptMessage(id: Int, message: String)
    @JvmStatic external fun nativeWebViewError(id: Int, description: String)
    @JvmStatic external fun nativeWebViewDismissed(id: Int)

    // IMobileHost::WebViewCommand
    const val WEB_LOAD_URL = 0
    const val WEB_LOAD_URL_WITH_TOKEN = 1
    const val WEB_RUN_SCRIPT = 2
    const val WEB_ADD_USER_SCRIPT = 3
    const val WEB_DELETE_COOKIES = 4
    const val WEB_RELOAD = 5
    const val WEB_OPEN_EXTERNAL = 6
    const val WEB_CLOSE = 7
}

/** Callbacks from native code (IMobileHost). Called on the GL render thread. */
interface GuiHost {
    fun requestRender()
    fun showMessage(kind: Int, title: String, text: String)
    fun askQuestion(id: Int, title: String, text: String, buttons: Array<String>)
    fun pickFiles(id: Int, kind: Int, title: String, wildcards: String)
    fun offerOutputFile(path: String)
    fun openUrl(url: String)
    fun setTextInputActive(active: Boolean)
    fun openWebView(id: Int, title: String, url: String, handlerNames: Array<String>)
    fun webViewCommand(id: Int, command: Int, data: String, extra: String)
}
