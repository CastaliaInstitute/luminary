plugins {
    id("com.android.application") version "8.13.2"
    id("org.jetbrains.kotlin.android") version "2.1.20"
}

android {
    namespace = "institute.castalia.luminary"
    compileSdk = 35

    defaultConfig {
        applicationId = "institute.castalia.luminary"
        // The deployment target is a wall-mounted Galaxy Note Edge on
        // Android 5.0.1; API 21 is the floor and the ceiling that matters.
        minSdk = 21
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
        externalNativeBuild {
            cmake {
                cppFlags("")
                arguments("-DANDROID_PLATFORM=android-21")
            }
        }
    }
    externalNativeBuild {
        cmake {
            path("src/main/cpp/CMakeLists.txt")
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
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

// The scene assets are the same binaries the P4 firmware embeds, and the
// solver bathymetry ships from tools/. Copied at build time so the repo has
// exactly one copy of each.
val repoRoot = rootDir.resolve("../..")
val syncSceneAssets by tasks.registering(Copy::class) {
    val runtime = repoRoot.resolve("firmware/luminary-background-viewer/assets/runtime")
    from(runtime) {
        include(
            "nubble_runtime_base.jpg",
            "nubble_runtime_water_mask.bin",
            "nubble_runtime_shore_distance.bin",
            "nubble_runtime_ocean_map.bin",
            "nubble_runtime_ocean_depth.bin",
            "nubble_runtime_cloud_low.bin",
            "nubble_runtime_cloud_mid.bin",
            "nubble_runtime_cloud_high.bin",
        )
    }
    from(repoRoot.resolve("firmware/luminary-background-viewer/assets/v2")) {
        include("nubble-runtime-scene-v1.json")
    }
    into(layout.projectDirectory.dir("src/main/assets"))
}
tasks.matching { it.name.startsWith("merge") && it.name.endsWith("Assets") }
    .configureEach { dependsOn(syncSceneAssets) }
