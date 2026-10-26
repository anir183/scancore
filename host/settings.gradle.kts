// SCANCORE's extension host: runs Mihon (and, later, Aniyomi) extensions
// for the app. Built by CMake (target scancore_host) or on its own with
// ./gradlew build.
rootProject.name = "scancore-host"

pluginManagement {
    repositories {
        gradlePluginPortal()
        google()
        mavenCentral()
    }
}

dependencyResolutionManagement {
    repositories {
        mavenCentral()
        google()
        // Only for QuickJS on Android (the fork Mihon uses).
        maven("https://jitpack.io") {
            content {
                includeGroup("com.github.zhanghai.quickjs-java")
                // NiceHttp, CloudStream's HTTP library.
                includeGroup("com.github.Blatzar")
            }
        }
        // JetBrains' JCEF (the web view extensions open on computers, as in
        // Suwayomi); its native part comes with JetBrains Runtime, downloaded
        // on first use.
        maven("https://packages.jetbrains.team/maven/p/ij/intellij-dependencies") {
            content { includeGroup("org.jetbrains.intellij.deps.jcef") }
        }
    }
}

include(":api", ":compat", ":core", ":cloudstream", ":desktop", ":testext")

// The Android host (an AAR for the app's APK) needs an Android SDK.
val androidSdk =
    listOfNotNull(System.getenv("ANDROID_SDK_ROOT"), System.getenv("ANDROID_HOME"), System.getProperty("user.home") + "/Android/Sdk")
        .map(::File)
        .firstOrNull { File(it, "platforms").isDirectory }
if (androidSdk != null) {
    include(":android")
    if (!File(rootDir, "local.properties").exists()) File(rootDir, "local.properties").writeText("sdk.dir=${androidSdk.path.replace("\\", "/")}\n")
}
