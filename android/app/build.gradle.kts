plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// PrusaSlicer resources that are bundled into the APK as assets and extracted on first start.
// Test data and the translation sources are left out to keep the APK small.
// resources/matdb is copied from the dependency build when PrusaSlicer is configured.
val bundledResources = listOf("presets", "lua", "lua_template", "shaders", "icons", "web", "matdb", "fonts", "shapes")
val generatedAssets = layout.buildDirectory.dir("generated/prusaslicer-assets")
val repoRoot = rootProject.projectDir.parentFile

val stagedResources = layout.buildDirectory.dir("generated/prusaslicer-resources")

val copyPrusaSlicerResources by tasks.registering(Sync::class) {
    description = "Collects the PrusaSlicer resources bundled with the app."
    into(stagedResources)
    bundledResources.forEach { dir ->
        from(File(repoRoot, "resources/$dir")) {
            into(dir)
            // ~60 MB of CJK fonts: the app uses the ones in /system/fonts instead.
            exclude("NotoSansCJK-*.ttc")
        }
    }
    // Translations: only the compiled catalogs (the .po sources are ~40 MB). PrusaSlicer finds
    // the languages by listing these directories, English is the fallback.
    from(File(repoRoot, "resources/localization")) {
        into("localization")
        include("**/*.mo")
    }
}

// One archive instead of ~1300 separate assets: extracting it is much faster than listing and
// copying the asset tree file by file (see ResourceInstaller).
val zipPrusaSlicerResources by tasks.registering(Zip::class) {
    description = "Packs the PrusaSlicer resources into a single APK asset."
    dependsOn(copyPrusaSlicerResources)
    from(stagedResources)
    archiveFileName.set("resources.zip")
    destinationDirectory.set(generatedAssets)
    isReproducibleFileOrder = true
    isPreserveFileTimestamps = false
}

android {
    namespace = "org.prusaslicer.android"
    compileSdk = 35

    defaultConfig {
        applicationId = "org.prusaslicer.android"
        minSdk = 28
        targetSdk = 35
        versionCode = 8
        versionName = "3.0.0-alpha14-android8"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            // No release signing configured: sign with the debug key so the APK installs.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    buildFeatures {
        viewBinding = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    sourceSets {
        getByName("main") {
            assets.srcDir(generatedAssets)
        }
    }

    packaging {
        jniLibs {
            // Keep the native library uncompressed and page aligned, it is large.
            useLegacyPackaging = false
        }
    }

    androidResources {
        // The resource archive is compressed already.
        noCompress += listOf("zip")
    }
}

tasks.named("preBuild") {
    dependsOn(zipPrusaSlicerResources)
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.webkit:webkit:1.12.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
}
