// The extension host inside the Android app: the same API and protocol as
// desktop (api/, core/ sources), on Android itself, so extensions load as
// they do in Mihon, without conversion. Built into an AAR the APK includes.
plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.serialization)
}

android {
    namespace = "com.mediumdev.scancore.host.android"
    compileSdk = 37
    defaultConfig {
        minSdk = 28
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        consumerProguardFiles("consumer-rules.pro")
    }
    sourceSets["main"].kotlin.directories += listOf("../api/src/main/kotlin", "../core/src/main/kotlin")
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    api(libs.coroutines.core)
    api(libs.coroutines.android)
    api(libs.serialization.json)
    api(libs.serialization.json.okio)
    api(libs.serialization.protobuf)
    api(libs.okhttp.core)
    api(libs.okhttp.brotli)
    // Keiyoushi's extensions decompress Zstd themselves (Mihon ships it too).
    api(libs.okhttp.zstd)
    api(libs.okhttp.dnsoverhttps)
    api(libs.okio)
    api(libs.jsoup)
    api(libs.rxjava)
    api(libs.injekt)
    // Aniyomi's local video servers (HttpServer).
    api(libs.nanohttpd)
    api(libs.androidx.preference)
    api(libs.quickjs.android)
    // CloudStream plugins (Jackson's Kotlin module) need Kotlin's reflection,
    // and the standard library finds it only in its own class loader: the app's.
    api(libs.kotlin.reflect)
    androidTestImplementation(libs.androidx.test.runner)
    androidTestImplementation(libs.androidx.test.junit)
}

// The instrumentation test gets the test extensions and the fixture site as
// assets.
val testAssets = layout.buildDirectory.dir("generated/testAssets")
val copyTestAssets by tasks.registering(Copy::class) {
    dependsOn(":testext:apk")
    from(project(":testext").layout.buildDirectory.dir("apk")) { include("testext.apk", "signer.txt") }
    from(project(":testext").layout.buildDirectory.dir("animeapk")) {
        include("testanime.apk", "signer.txt")
        rename("signer.txt", "animesigner.txt")
    }
    from(rootProject.layout.projectDirectory.dir("../tests/fixtures/host")) { into("site") }
    into(testAssets)
}
android.sourceSets["androidTest"].assets.directories += testAssets.get().asFile.path
tasks.configureEach { if (name.startsWith("merge") && name.contains("AndroidTest") && name.endsWith("Assets")) dependsOn(copyTestAssets) }

// What the app's APK build takes (packaging/android/build.gradle): the AAR,
// and the libraries it needs from Maven, one coordinate per line (an AAR in
// libs/ brings none of its own).
val forApp = layout.buildDirectory.dir("app")
val appBundle by tasks.registering(Copy::class) {
    dependsOn("assembleRelease")
    val coordinates =
        provider {
            configurations["api"].dependencies.joinToString("") { "${it.group}:${it.name}:${it.version}\n" }
        }
    inputs.property("coordinates", coordinates)
    from(layout.buildDirectory.file("outputs/aar/android-release.aar")) { rename { "scancore-host.aar" } }
    into(forApp)
    doLast { forApp.get().file("scancore-host.deps").asFile.writeText(coordinates.get()) }
}
