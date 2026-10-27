// Echo extensions (.eapk: an APK) in the extension host: Echo's extension
// library (dev.brahmkshatriya.echo:common, the Unabandon Public License,
// GPL-3.0 with public source; see README.md) and the adapter that shows a
// music extension as a SCANCORE music source. Kept out of the host's own
// class path, like CloudStream's runtime: lib/echo in the desktop Extensions
// runtime, one dex downloaded on Android (gradle/plugin-runtime.gradle.kts).
plugins {
    alias(libs.plugins.kotlin.jvm)
    alias(libs.plugins.kotlin.serialization)
}

dependencies {
    // api: extensions are compiled against it (the test extension too).
    api(libs.echo.common)
    compileOnly(project(":core"))
    testImplementation(project(":core"))
    testImplementation(libs.junit)
}

extra["runtimeName"] = "echo"
extra["runtimeJarTask"] = "jar"
extra["runtimeClasspath"] = "runtimeClasspath"
apply(from = rootProject.file("gradle/plugin-runtime.gradle.kts"))
