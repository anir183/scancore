// CloudStream plugins (.cs3) in the extension host: CloudStream's library
// (library/, GPL-3.0, from recloudstream/cloudstream; see README.md) built
// for the JVM against the host's Android stand-ins, the few app classes
// plugins use (src/jvmMain/kotlin/com/lagradost), and the adapter that shows
// each plugin as an Aniyomi anime source (src/jvmMain/kotlin/com/mediumdev).
//
// Kept out of the host's own class path: on desktop its jars sit in
// lib/cloudstream of the Extensions runtime, on Android they are one dex
// file downloaded when the first CloudStream repository is added. Either
// way the host loads them in a class loader of their own (core's
// CloudStream.kt), so their libraries never meet Mihon's or Aniyomi's.
plugins {
    alias(libs.plugins.kotlin.multiplatform)
    alias(libs.plugins.kotlin.serialization)
}

kotlin {
    jvmToolchain(17)
    jvm()

    compilerOptions {
        freeCompilerArgs.add("-Xexpect-actual-classes")
        // Their code, as their build compiles it.
        suppressWarnings.set(true)
    }

    sourceSets {
        all {
            languageSettings {
                optIn("com.lagradost.cloudstream3.InternalAPI")
                optIn("com.lagradost.cloudstream3.Prerelease")
            }
        }
        commonMain {
            kotlin.srcDirs("library/commonMain/kotlin", "src/commonMain/kotlin")
            // api: plugins are compiled against these too.
            dependencies {
                api(libs.android.annotations)
                api(libs.jackson.kotlin)
                api(libs.jsoup)
                api(libs.atomicfu)
                api(libs.coroutines.core)
                api(libs.kotlinx.datetime)
                api(libs.kotlinx.io)
                api(libs.serialization.json)
                api(libs.ksoup)
                api(libs.ktor.http)
                api(libs.nicehttp)
                api(libs.rhino)
                api(libs.cryptography.core)
                api(libs.cryptography.provider)
            }
        }
        jvmMain {
            // Their Android code, which the host's stand-ins (desktop) and
            // Android itself both run, and SCANCORE's own.
            kotlin.srcDirs("library/jvmCommonMain/kotlin", "library/androidMain/kotlin", "app")
            dependencies {
                implementation(libs.kotlin.reflect)
                compileOnly(project(":core"))
            }
        }
        jvmTest {
            dependencies {
                implementation(project(":core"))
                implementation(libs.junit)
            }
        }
    }
}

extra["runtimeName"] = "cloudstream"
extra["runtimeJarTask"] = "jvmJar"
extra["runtimeClasspath"] = "jvmRuntimeClasspath"
apply(from = rootProject.file("gradle/plugin-runtime.gradle.kts"))
