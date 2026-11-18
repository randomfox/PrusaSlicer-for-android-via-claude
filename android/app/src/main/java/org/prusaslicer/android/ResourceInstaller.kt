package org.prusaslicer.android

import android.content.Context
import java.io.File
import java.io.RandomAccessFile
import java.util.zip.ZipInputStream

/**
 * PrusaSlicer reads its resources (presets, shaders, ...) from the file system, so the
 * archive bundled in the APK assets is extracted into private storage once per app version.
 *
 * Both app processes (the GUI and "Quick slice") may start at the same time: a file lock
 * serializes the extraction, and the archive is unpacked next to the target and renamed into
 * place, so a half extracted directory is never used.
 */
object ResourceInstaller {
    private const val ASSET_ARCHIVE = "resources.zip"
    private const val TARGET_DIR = "resources"

    fun install(context: Context): File {
        val target = File(context.filesDir, TARGET_DIR)
        val stamp = File(target, ".installed-version")
        val packageInfo = context.packageManager.getPackageInfo(context.packageName, 0)
        val version = "${packageInfo.longVersionCode}-${packageInfo.lastUpdateTime}"

        if (stamp.isFile && stamp.readText() == version) {
            return target
        }

        RandomAccessFile(File(context.filesDir, "resources.lock"), "rw").use { lockFile ->
            lockFile.channel.lock().use {
                // Another process may have finished the job while we waited.
                if (stamp.isFile && stamp.readText() == version) {
                    return target
                }
                val staging = File(context.filesDir, "$TARGET_DIR.new")
                staging.deleteRecursively()
                extract(context, staging)
                File(staging, ".installed-version").writeText(version)

                val old = File(context.filesDir, "$TARGET_DIR.old")
                old.deleteRecursively()
                if (target.exists() && !target.renameTo(old)) {
                    target.deleteRecursively()
                }
                check(staging.renameTo(target)) { "Cannot move the resources into place" }
                old.deleteRecursively()
            }
        }
        return target
    }

    private fun extract(context: Context, dest: File) {
        val root = dest.canonicalFile
        val buffer = ByteArray(256 * 1024)
        ZipInputStream(context.assets.open(ASSET_ARCHIVE).buffered(256 * 1024)).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                val file = File(root, entry.name).canonicalFile
                // Never write outside of the target directory.
                check(file.path.startsWith(root.path + File.separator)) { "Bad entry ${entry.name}" }
                if (entry.isDirectory) {
                    file.mkdirs()
                    continue
                }
                file.parentFile?.mkdirs()
                file.outputStream().use { out ->
                    while (true) {
                        val n = zip.read(buffer)
                        if (n < 0) break
                        out.write(buffer, 0, n)
                    }
                }
            }
        }
    }
}
