plugins {
    id("com.android.application")
}

android {
    namespace = "com.freecrane.guestgame"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.freecrane.guestgame"
        minSdk = 26
        targetSdk = 28
        versionCode = 1
        versionName = "1.0"
        ndk { abiFilters += "arm64-v8a" }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    packaging { jniLibs { useLegacyPackaging = true } }

    lint {
        checkReleaseBuilds = false
        abortOnError = false
    }
}
