package org.prusaslicer.android

import org.json.JSONArray
import org.json.JSONObject

/** Parsers for the JSON emitted by the CLI profile queries (ProfilesSharingUtils.cpp). */
object Profiles {

    data class Printer(val name: String, val vendor: String, val technology: String)

    data class PrintProfile(val name: String, val materials: List<String>)

    /** Output of `--query-printer-models`. */
    fun parsePrinters(json: String): List<Printer> {
        val result = mutableListOf<Printer>()
        val models = JSONObject(json).optJSONArray("printer_models") ?: return result
        for (i in 0 until models.length()) {
            val model = models.getJSONObject(i)
            val vendor = model.optString("vendor_name")
            val technology = model.optString("technology", "FFF")
            val variants = model.optJSONArray("variants")
            if (variants != null) {
                for (j in 0 until variants.length()) {
                    addProfiles(result, variants.getJSONObject(j), vendor, technology)
                }
            } else {
                addProfiles(result, model, vendor, technology)
            }
        }
        return result.distinctBy { it.name }
    }

    private fun addProfiles(out: MutableList<Printer>, node: JSONObject, vendor: String, technology: String) {
        for (key in listOf("printer_profiles", "user_printer_profiles")) {
            val profiles = node.optJSONArray(key) ?: continue
            for (k in 0 until profiles.length()) {
                val name = profiles.getJSONObject(k).optString("name")
                if (name.isNotEmpty()) out.add(Printer(name, vendor, technology))
            }
        }
    }

    /** Output of `--query-print-tool-filament-profiles --printer-profile <name>`. */
    fun parsePrintProfiles(json: String): List<PrintProfile> {
        if (json.isBlank()) return emptyList()
        val prints = JSONObject(json).optJSONArray("print_profiles") ?: return emptyList()
        return (0 until prints.length()).map { i ->
            val print = prints.getJSONObject(i)
            val materials = print.optJSONArray("filament_profiles")
                ?: print.optJSONArray("sla_material_profiles")
                ?: JSONArray()
            PrintProfile(
                name = print.getString("name"),
                materials = (0 until materials.length()).map { materials.getString(it) }.distinct(),
            )
        }
    }
}
