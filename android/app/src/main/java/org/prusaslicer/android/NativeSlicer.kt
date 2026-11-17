package org.prusaslicer.android

import java.io.File

/** Thin wrapper around libprusaslicer.so (src/slic3r-app-android). */
object NativeSlicer {
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
