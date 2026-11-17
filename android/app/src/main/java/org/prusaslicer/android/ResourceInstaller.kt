package org.prusaslicer.android

import android.content.Context
import android.content.res.AssetManager
import java.io.File

/**
 * PrusaSlicer reads its resources (presets, shaders, ...) from the file system, so the
 * copy bundled in the APK assets is extracted into private storage once per app version.
 */
object ResourceInstaller {
    private const val ASSET_ROOT = "resources"

    fun install(context: Context): File {
        val target = File(context.filesDir, ASSET_ROOT)
        val stamp = File(target, ".installed-version")
        val version = context.packageManager
            .getPackageInfo(context.packageName, 0).longVersionCode.toString() +
            "-" + BuildConfigCompat.lastUpdateTime(context)

        if (stamp.exists() && stamp.readText() == version) {
            return target
        }

        target.deleteRecursively()
        copyAssetDir(context.assets, ASSET_ROOT, target)
        stamp.writeText(version)
        return target
    }

    private fun copyAssetDir(assets: AssetManager, path: String, dest: File) {
        val children = assets.list(path) ?: emptyArray()
        if (children.isEmpty()) {
            // A file (asset directories are never empty in the APK).
            dest.parentFile?.mkdirs()
            assets.open(path).use { input ->
                dest.outputStream().use { output -> input.copyTo(output) }
            }
            return
        }
        dest.mkdirs()
        for (child in children) {
            copyAssetDir(assets, "$path/$child", File(dest, child))
        }
    }
}

private object BuildConfigCompat {
    fun lastUpdateTime(context: Context): Long =
        context.packageManager.getPackageInfo(context.packageName, 0).lastUpdateTime
}
