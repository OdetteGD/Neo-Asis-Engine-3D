// ---------------------------------------------------------------------------
//  Neo-Asis Engine 3D — Android player
//  Native build is driven by CMake (see app/CMakeLists.txt); Gradle only
//  packages it plus a thin GameActivity shell.
// ---------------------------------------------------------------------------
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "org.neoasis.engine"
    compileSdk = 35
    ndkVersion = "26.3.11579264"

    defaultConfig {
        applicationId = "org.neoasis.engine"
        minSdk = 26          // Vulkan + GameActivity floor; covers ~97% of devices
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // arm64-v8a only for now: every Vulkan-capable Android device is
            // 64-bit ARM. Add x86_64 for emulator CI runs.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DNEO_BUILD_PLAYER=OFF",
                    "-DNEO_ENABLE_VALIDATION=OFF",
                    "-DANDROID_STL=c++_shared"
                )
                cppFlags += listOf("-std=c++20")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "3.24.0+"
        }
    }

    // Prefab pulls GameActivity's native headers + static libs.
    buildFeatures {
        prefab = true
        buildConfig = true
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
            isJniDebuggable = true
            externalNativeBuild {
                cmake { arguments += "-DNEO_ENABLE_VALIDATION=ON" }
            }
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    sourceSets["main"].assets.srcDirs("src/main/assets")

    packaging {
        jniLibs {
            // The engine links libc++_shared; keep exactly one copy.
            pickFirsts += setOf("lib/**/libc++_shared.so")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.games:games-activity:3.0.5")
}
