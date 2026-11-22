plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.gamecore.magpietest"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.gamecore.magpietest"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"

        // The layer must live in the app's own nativeLibraryDir for the loader to find it
        // (verified AOSP behavior); arm64 only matches the CI device targets.
        ndk { abiFilters += listOf("arm64-v8a") }
        externalNativeBuild { cmake { arguments += listOf("-DANDROID_STL=c++_shared") } }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    // No lint gate on the prototype; the main app module keeps its own policy.
    lint { abortOnError = false }
}
