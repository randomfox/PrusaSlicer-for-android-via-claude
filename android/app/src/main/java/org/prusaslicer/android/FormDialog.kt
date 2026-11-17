package org.prusaslicer.android

import android.content.Context
import android.graphics.Typeface
import android.text.InputType
import android.util.TypedValue
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.json.JSONArray
import org.json.JSONObject

/**
 * A modal form requested by native code (IMobileHost::show_form): the replacement of the
 * desktop's modal dialogs. The native render thread waits until [answer] is called, so every
 * form must be answered exactly once: by a button, by dismissing it, or by [cancel].
 */
class FormDialog(
    private val context: Context,
    private val id: Int,
    json: String,
    private val onAnswer: (id: Int, answer: String) -> Unit,
) {
    private val spec = JSONObject(json)
    private val getters = LinkedHashMap<String, () -> String>()
    private var answered = false
    private var dialog: AlertDialog? = null

    fun show() {
        val buttons = spec.optJSONArray("buttons") ?: JSONArray()
        val builder = MaterialAlertDialogBuilder(context)
            .setTitle(spec.optString("title").ifEmpty { context.getString(R.string.app_name) })
            .setView(buildContent())
            .setOnCancelListener { answer(-1) }
        // AlertDialog has three button slots: positive (right), negative, neutral (left).
        buttons.optString(0, null)?.let { builder.setPositiveButton(it) { _, _ -> answer(0) } }
        buttons.optString(1, null)?.let { builder.setNegativeButton(it) { _, _ -> answer(1) } }
        buttons.optString(2, null)?.let { builder.setNeutralButton(it) { _, _ -> answer(2) } }
        if (buttons.length() == 0) builder.setPositiveButton(android.R.string.ok) { _, _ -> answer(0) }
        // At most three buttons: the native side never asks for more.
        dialog = builder.show()
    }

    /** Answers "dismissed" (e.g. the activity is going away). */
    fun cancel() {
        answer(-1)
        dialog?.dismiss()
    }

    val isOpen get() = !answered

    private fun answer(button: Int) {
        if (answered) return
        answered = true
        val values = JSONObject()
        getters.forEach { (key, get) -> values.put(key, get()) }
        onAnswer(id, JSONObject().put("button", button).put("values", values).toString())
    }

    private fun dp(value: Int) =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value.toFloat(), context.resources.displayMetrics).toInt()

    private fun buildContent(): View {
        val column = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(8), dp(24), 0)
        }
        spec.optString("text").takeIf { it.isNotEmpty() }?.let {
            column.addView(TextView(context).apply { text = it; setTextIsSelectable(true) })
        }
        spec.optString("error").takeIf { it.isNotEmpty() }?.let {
            column.addView(TextView(context).apply {
                text = it
                setTextColor(0xFFE57373.toInt())
                setPadding(0, dp(8), 0, 0)
            })
        }
        val fields = spec.optJSONArray("fields") ?: JSONArray()
        for (i in 0 until fields.length()) {
            column.addView(buildField(fields.getJSONObject(i)))
        }
        return ScrollView(context).apply { addView(column) }
    }

    private fun label(text: String, bold: Boolean = false) = TextView(context).apply {
        this.text = text
        if (bold) setTypeface(typeface, Typeface.BOLD)
        setPadding(0, dp(12), 0, dp(2))
    }

    private fun buildField(field: JSONObject): View {
        val key = field.optString("id")
        val labelText = field.optString("label")
        val value = field.optString("value")
        val wrap = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        when (field.optString("type")) {
            "check" -> {
                val box = CheckBox(context).apply { text = labelText; isChecked = value == "1" }
                getters[key] = { if (box.isChecked) "1" else "0" }
                wrap.addView(box)
            }
            "choice" -> {
                if (labelText.isNotEmpty()) wrap.addView(label(labelText))
                val options = field.optJSONArray("options") ?: JSONArray()
                val items = (0 until options.length()).map { options.getString(it) }
                val spinner = Spinner(context).apply {
                    adapter = ArrayAdapter(context, android.R.layout.simple_spinner_dropdown_item, items)
                    setSelection(items.indexOf(value).coerceAtLeast(0))
                }
                getters[key] = { items.getOrNull(spinner.selectedItemPosition) ?: value }
                wrap.addView(spinner, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            }
            "info" -> {
                if (labelText.isNotEmpty()) wrap.addView(label(labelText, bold = true))
                wrap.addView(TextView(context).apply {
                    text = value
                    setTextIsSelectable(true)
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                })
            }
            else -> { // text, number
                if (labelText.isNotEmpty()) wrap.addView(label(labelText))
                val edit = EditText(context).apply {
                    setText(value)
                    setSelection(value.length)
                    inputType = if (field.optString("type") == "number")
                        InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED
                    else InputType.TYPE_CLASS_TEXT
                    isSingleLine = true
                }
                getters[key] = { edit.text.toString() }
                wrap.addView(edit, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            }
        }
        return wrap
    }
}
