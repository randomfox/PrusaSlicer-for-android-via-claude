package org.prusaslicer.android

import android.annotation.SuppressLint
import android.content.Context
import android.opengl.GLSurfaceView
import android.text.InputType
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager

/**
 * OpenGL ES 3.2 surface the PrusaSlicer GUI renders into. Input is forwarded to native code on
 * the render thread; the native side decides when more frames are needed.
 */
@SuppressLint("ViewConstructor")
class PrusaSlicerSurfaceView(context: Context, private val onSurfaceReady: () -> Unit) : GLSurfaceView(context) {

    /** Physical pixels per PrusaSlicer UI pixel (density times the user's UI zoom). */
    @Volatile
    var uiScale: Float = resources.displayMetrics.density

    private var contextCreated = false
    private var surfaceWidth = 0
    private var surfaceHeight = 0

    init {
        setEGLContextClientVersion(3)
        setEGLConfigChooser(8, 8, 8, 8, 24, 8)
        preserveEGLContextOnPause = true
        setRenderer(object : Renderer {
            override fun onSurfaceCreated(gl: javax.microedition.khronos.opengles.GL10?, config: javax.microedition.khronos.egl.EGLConfig?) {
                if (contextCreated) {
                    // The EGL context was lost (all GPU resources are gone). PrusaSlicer cannot
                    // rebuild them yet, so restart the GUI.
                    post { (context as? GuiActivity)?.restartAfterContextLoss() }
                    return
                }
                contextCreated = true
                GuiNative.nativeSurfaceCreated()
                post(onSurfaceReady)
            }

            override fun onSurfaceChanged(gl: javax.microedition.khronos.opengles.GL10?, width: Int, height: Int) {
                surfaceWidth = width
                surfaceHeight = height
                GuiNative.nativeSurfaceChanged(width, height, uiScale)
            }

            override fun onDrawFrame(gl: javax.microedition.khronos.opengles.GL10?) {
                if (GuiNative.nativeDrawFrame()) {
                    requestRender()
                }
            }
        })
        renderMode = RENDERMODE_WHEN_DIRTY
        isFocusable = true
        isFocusableInTouchMode = true
    }

    fun applyUiScale(scale: Float) {
        uiScale = scale
        queueEvent {
            if (surfaceWidth > 0) GuiNative.nativeSurfaceChanged(surfaceWidth, surfaceHeight, scale)
        }
        requestRender()
    }

    private fun send(block: () -> Unit) {
        queueEvent(block)
        requestRender()
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE) {
            return onMouseEvent(event)
        }
        val action = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> GuiNative.TOUCH_DOWN
            MotionEvent.ACTION_MOVE -> GuiNative.TOUCH_MOVE
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> GuiNative.TOUCH_UP
            MotionEvent.ACTION_CANCEL -> GuiNative.TOUCH_CANCEL
            else -> return true
        }
        val count = minOf(event.pointerCount, 2)
        val xs = FloatArray(count) { event.getX(it) }
        val ys = FloatArray(count) { event.getY(it) }
        send { GuiNative.nativeTouch(action, xs, ys) }
        return true
    }

    private var mouseButtons = 0

    private fun onMouseEvent(event: MotionEvent): Boolean {
        val x = event.x
        val y = event.y
        val buttons = event.buttonState
        val changed = buttons xor mouseButtons
        mouseButtons = buttons
        send {
            GuiNative.nativeMouseMove(x, y)
            listOf(MotionEvent.BUTTON_PRIMARY to 0, MotionEvent.BUTTON_SECONDARY to 1, MotionEvent.BUTTON_TERTIARY to 2)
                .forEach { (mask, index) ->
                    if (changed and mask != 0) GuiNative.nativeMouseButton(index, buttons and mask != 0, x, y)
                }
        }
        return true
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_MOVE -> {
                val x = event.x; val y = event.y
                send { GuiNative.nativeMouseMove(x, y) }
                return true
            }
            MotionEvent.ACTION_SCROLL -> {
                val x = event.x; val y = event.y
                val dx = event.getAxisValue(MotionEvent.AXIS_HSCROLL)
                val dy = event.getAxisValue(MotionEvent.AXIS_VSCROLL)
                send { GuiNative.nativeMouseWheel(x, y, dx, dy) }
                return true
            }
        }
        return super.onGenericMotionEvent(event)
    }

    // ---- Keyboard ------------------------------------------------------------------------

    fun setSoftKeyboardVisible(visible: Boolean) {
        val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        if (visible) {
            requestFocus()
            imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT)
        } else {
            imm.hideSoftInputFromWindow(windowToken, 0)
        }
    }

    override fun onCheckIsTextEditor(): Boolean = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_FLAG_NO_EXTRACT_UI
        return object : BaseInputConnection(this, false) {
            override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
                val s = text.toString()
                send { GuiNative.nativeText(s) }
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                repeat(beforeLength) { sendKey(KeyEvent.KEYCODE_DEL) }
                repeat(afterLength) { sendKey(KeyEvent.KEYCODE_FORWARD_DEL) }
                return true
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                forwardKey(event)
                return true
            }
        }
    }

    private fun sendKey(code: Int) {
        send {
            GuiNative.nativeKey(code, true)
            GuiNative.nativeKey(code, false)
        }
    }

    private fun forwardKey(event: KeyEvent): Boolean {
        val code = event.keyCode
        val down = event.action == KeyEvent.ACTION_DOWN
        val char = event.unicodeChar
        send {
            GuiNative.nativeKey(code, down)
            if (down && char != 0 && !Character.isISOControl(char)) {
                GuiNative.nativeText(String(Character.toChars(char)))
            }
        }
        return true
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean =
        if (keyCode == KeyEvent.KEYCODE_BACK) super.onKeyDown(keyCode, event) else forwardKey(event)

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean =
        if (keyCode == KeyEvent.KEYCODE_BACK) super.onKeyUp(keyCode, event) else forwardKey(event)

    fun sendBackAsEscape() = sendKey(KeyEvent.KEYCODE_ESCAPE)

    fun openFiles(paths: List<String>) = send { GuiNative.nativeOpenFiles(paths.toTypedArray()) }
    fun answerQuestion(id: Int, index: Int) = send { GuiNative.nativeQuestionAnswered(id, index) }
    fun answerFiles(id: Int, paths: List<String>) = send { GuiNative.nativeFilesPicked(id, paths.toTypedArray()) }
}
