import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

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

// The jars SCANCORE ships for it: this module's and the libraries the host
// does not already have (those come from the host's class loader).
val runtimeJars by tasks.registering(Sync::class) {
    val jvmJar = tasks.named("jvmJar")
    dependsOn(jvmJar)
    val host = project(":desktop").configurations.named("runtimeClasspath")
    val own = configurations.named("jvmRuntimeClasspath")
    from(jvmJar)
    // By artifact, whatever its version: the host's copy is the one used.
    val artifact = { f: File -> f.name.removeSuffix(".jar").replace(Regex("""-\d[\w.]*$"""), "") }
    from(own.map { cs -> cs.files.filter { f -> host.get().files.none { artifact(it) == artifact(f) } && !artifact(f).startsWith("kotlin-stdlib") } })
    into(layout.buildDirectory.dir("runtime-jars"))
}

// On Android: the same classes as one dex in a jar, which the app downloads
// (the extensions-runtime release) and the host loads with a DexClassLoader:
// what the app's own host has (its libraries are in the APK) left out, and
// what only the desktop host has (Rhino, Kotlin's reflection) put in.
val androidRuntime by tasks.registering {
    val jvmJar = tasks.named("jvmJar")
    dependsOn(jvmJar)
    val own = configurations.named("jvmRuntimeClasspath")
    val app = rootProject.findProject(":android")?.configurations?.named("releaseRuntimeClasspath")
    val out = layout.buildDirectory.file("android/cloudstream-runtime-android.jar")
    inputs.files(jvmJar)
    outputs.file(out)
    doLast {
        val sdk =
            listOfNotNull(System.getenv("ANDROID_SDK_ROOT"), System.getenv("ANDROID_HOME"), System.getProperty("user.home") + "/Android/Sdk")
                .map(::File)
                .firstOrNull { File(it, "build-tools").isDirectory && File(it, "platforms").isDirectory }
                ?: throw GradleException("No Android SDK for the CloudStream runtime.")
        fun newest(dir: File) = dir.listFiles()!!.filter { it.isDirectory }.maxBy { it.name.replace(Regex("\\D+"), ".").split('.').filter(String::isNotEmpty).map(String::toInt).fold(0L) { a, b -> a * 1000 + b } }
        val tools = newest(File(sdk, "build-tools"))
        val platform = File(newest(File(sdk, "platforms")), "android.jar")
        val artifact = { f: File -> f.name.removeSuffix(".jar").removeSuffix(".aar").replace(Regex("""-\d[\w.]*$"""), "") }
        val inApp = app?.get()?.incoming?.artifactView { attributes.attribute(Attribute.of("artifactType", String::class.java), "jar") }?.files?.files.orEmpty().map(artifact).toSet() +
            setOf("kotlin-stdlib", "kotlin-reflect", "kotlinx-coroutines-core-jvm", "kotlinx-serialization-core-jvm", "kotlinx-serialization-json-jvm", "okhttp-jvm", "okio-jvm", "jsoup", "json", "annotations")
        // Not in the APK whatever the app's class path says (Rhino comes in
        // there only for other libraries' builds). Kotlin's reflection is in
        // the APK (android/build.gradle.kts): the standard library looks for
        // it in its own class loader.
        val always = setOf("rhino")
        val jars = listOf(jvmJar.get().outputs.files.singleFile) +
            own.get().files.filter { (artifact(it) in always || artifact(it) !in inApp) && !artifact(it).startsWith("kotlin-stdlib") && !artifact(it).startsWith("compat") }
        logger.lifecycle("CloudStream for Android takes: " + jars.joinToString { artifact(it) })
        val target = out.get().asFile.apply { parentFile.mkdirs(); delete() }
        val exe = if (System.getProperty("os.name").startsWith("Windows")) ".bat" else ""
        // The arguments in a file: too long for a command line on Windows.
        val args = File(target.parentFile, "d8.args")
        args.writeText(
            (listOf("--release", "--min-api", "28", "--lib", platform.path) + own.get().files.flatMap { listOf("--classpath", it.path) } +
                listOf("--output", target.path) + jars.map { it.path }).joinToString("\n"),
        )
        val process = ProcessBuilder(File(tools, "d8$exe").path, "@" + args.path).redirectErrorStream(true).start()
        val log = process.inputStream.bufferedReader().readText()
        check(process.waitFor() == 0) { "d8 failed:\n$log" }
        // d8 keeps only classes: the libraries' other files go in beside
        // them (the dex class loader reads them from the jar), service
        // lists merged (cryptography-kotlin finds its provider by one).
        val resources = LinkedHashMap<String, ByteArray>()
        for (jar in jars) {
            ZipFile(jar).use { zip ->
                for (entry in zip.entries()) {
                    val name = entry.name
                    if (entry.isDirectory || name.endsWith(".class") || name.endsWith("module-info.class") || name == "META-INF/MANIFEST.MF" ||
                        Regex("""META-INF/[^/]+\.(SF|RSA|DSA|EC)""").matches(name) || name.startsWith("META-INF/versions/")) continue
                    val bytes = zip.getInputStream(entry).use { it.readBytes() }
                    resources[name] = if (name.startsWith("META-INF/services/") && name in resources) resources[name]!! + "\n".toByteArray() + bytes else resources[name] ?: bytes
                }
            }
        }
        val merged = File(target.path + ".tmp")
        ZipOutputStream(merged.outputStream().buffered()).use { out ->
            ZipFile(target).use { zip ->
                for (entry in zip.entries()) {
                    out.putNextEntry(ZipEntry(entry.name))
                    zip.getInputStream(entry).use { it.copyTo(out) }
                    out.closeEntry()
                }
            }
            for ((name, bytes) in resources) {
                out.putNextEntry(ZipEntry(name))
                out.write(bytes)
                out.closeEntry()
            }
        }
        merged.copyTo(target, overwrite = true)
        merged.delete()
        val sum = MessageDigest.getInstance("SHA-256").digest(target.readBytes()).joinToString("") { "%02x".format(it) }
        File(target.path + ".sha256").writeText("$sum  ${target.name}\n")
        logger.lifecycle("CloudStream for Android: ${jars.size} jars, ${target.length() / 1024} KiB")
    }
}
