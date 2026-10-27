import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

// A plugin ecosystem's runtime (cloudstream/, echo/), which the host loads in
// a class loader of its own: `runtimeJars` for the desktop Extensions runtime
// (lib/<name>) and `androidRuntime`, one dex the Android app downloads.
// Applied after setting extra["runtimeName"], extra["runtimeJarTask"] and
// extra["runtimeClasspath"].
val runtimeName = extra["runtimeName"] as String
val jarTask = extra["runtimeJarTask"] as String
val classpath = extra["runtimeClasspath"] as String

// The jars SCANCORE ships for it: this module's and the libraries the host
// does not already have (those come from the host's class loader).
tasks.register<Sync>("runtimeJars") {
    val jvmJar = tasks.named(jarTask)
    dependsOn(jvmJar)
    val host = project(":desktop").configurations.named("runtimeClasspath")
    val own = configurations.named(classpath)
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
tasks.register("androidRuntime") {
    val jvmJar = tasks.named(jarTask)
    dependsOn(jvmJar)
    val own = configurations.named(classpath)
    val app = rootProject.findProject(":android")?.configurations?.named("releaseRuntimeClasspath")
    val out = layout.buildDirectory.file("android/$runtimeName-runtime-android.jar")
    inputs.files(jvmJar)
    outputs.file(out)
    doLast {
        val sdk =
            listOfNotNull(System.getenv("ANDROID_SDK_ROOT"), System.getenv("ANDROID_HOME"), System.getProperty("user.home") + "/Android/Sdk")
                .map(::File)
                .firstOrNull { File(it, "build-tools").isDirectory && File(it, "platforms").isDirectory }
                ?: throw GradleException("No Android SDK for the $runtimeName runtime.")
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
        logger.lifecycle("$runtimeName for Android takes: " + jars.joinToString { artifact(it) })
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
        logger.lifecycle("$runtimeName for Android: ${jars.size} jars, ${target.length() / 1024} KiB")
    }
}
