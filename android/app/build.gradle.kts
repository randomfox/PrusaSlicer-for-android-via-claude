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

val copyPrusaSlicerResources by tasks.registering(Sync::class) {
    description = "Copies PrusaSlicer resources into the APK assets."
    into(generatedAssets.map { it.dir("resources") })
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

android {
    namespace = "org.prusaslicer.android"
    compileSdk = 35

    defaultConfig {
        applicationId = "org.prusaslicer.android"
        minSdk = 28
        targetSdk = 35
        versionCode = 3
        versionName = "3.0.0-alpha14-android3"

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
        // Preset/config files must be stored as-is.
        noCompress += listOf("ini", "json", "yaml", "lua")
    }
}

tasks.named("preBuild") {
    dependsOn(copyPrusaSlicerResources)
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
