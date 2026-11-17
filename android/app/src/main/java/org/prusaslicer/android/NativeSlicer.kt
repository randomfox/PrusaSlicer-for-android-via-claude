package org.prusaslicer.android

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.asCoroutineDispatcher
import java.io.File
import java.util.concurrent.Executors

/**
 * Thin wrapper around libprusaslicer.so (src/slic3r-app-android).
 *
 * The native side keeps one PrusaSlicer session for the lifetime of the process: the preset
 * bundle is loaded by the first call that needs it (about 1 GB, several seconds) and reused
 * afterwards, so later profile queries and slicing do not pay for it again.
 */
object NativeSlicer {
    /** All native calls run on this single thread (PrusaSlicer's session is not thread safe). */
    val dispatcher: CoroutineDispatcher =
        Executors.newSingleThreadExecutor { r -> Thread(r, "prusaslicer-native").apply { isDaemon = true } }
            .asCoroutineDispatcher()

    init {
        System.loadLibrary("prusaslicer")
    }

    class Result(val exitCode: Int, val output: String) {
        val isSuccess get() = exitCode == 0
    }

    @Volatile
    private var initialized = false

    @JvmStatic
    private external fun nativeInit(resourcesDir: String, dataDir: String, cacheDir: String): Int

    @JvmStatic
    private external fun nativeRun(args: Array<String>, logFile: String): Int

    @JvmStatic
    external fun nativeVersion(): String

    @Synchronized
    fun init(resourcesDir: File, dataDir: File, cacheDir: File) {
        if (initialized) return
        dataDir.mkdirs()
        cacheDir.mkdirs()
        val rc = nativeInit(resourcesDir.absolutePath, dataDir.absolutePath, cacheDir.absolutePath)
        check(rc == 0) { "PrusaSlicer native initialization failed ($rc)" }
        initialized = true
    }

    /**
     * Runs the PrusaSlicer command line interface with [args] (without the program name).
     * Blocks until finished; call from a background thread.
     */
    @Synchronized
    fun run(args: List<String>, logFile: File): Result {
        check(initialized) { "NativeSlicer.init() was not called" }
        logFile.parentFile?.mkdirs()
        val rc = nativeRun(args.toTypedArray(), logFile.absolutePath)
        val output = if (logFile.exists()) logFile.readText() else ""
        return Result(rc, output)
    }
}
