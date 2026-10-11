plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.freecrane.adrenotest"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.freecrane.adrenotest"
        minSdk = 26
        targetSdk = 28          // low on purpose: keeps native-library loading permissive
        versionCode = 2
        versionName = "0.2"
        ndk { abiFilters += "arm64-v8a" }
    }

    // Same key on every build, so new APKs install over old ones.
    signingConfigs {
        getByName("debug") {
            storeFile = file("debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        getByName("debug") {
            // Debuggable apps get Android's strict JNI checker, which kills many games.
            isDebuggable = false
        }
    }

    // libadrenotools hook libs must exist as real files on disk
    packaging { jniLibs { useLegacyPackaging = true } }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}
