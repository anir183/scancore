package com.mediumdev.scancore.host

import eu.kanade.tachiyomi.animesource.AnimeSource
import okhttp3.OkHttpClient
import okhttp3.Response
import java.io.File
import java.security.MessageDigest

// CloudStream plugins (.cs3 files: a dex and a manifest.json). Their runtime
// (host/cloudstream: CloudStream's library and the adapter, GPL-3.0) is not
// on the host's class path: the platform says where its classes are
// (desktop: lib/cloudstream in the Extensions runtime; Android: the dex
// downloaded with the first CloudStream repository), and it is loaded the
// first time a plugin is.
interface CloudStreamRuntime {
    // Once, before the first plugin: the app's Context and the host's
    // client (SCANCORE's cookies, DNS and browser check).
    fun start(
        context: Any,
        client: () -> OkHttpClient,
    )

    // The plugin's providers as anime sources. `classes` loads the plugin's
    // code with the runtime as its parent; `file` is the .cs3.
    fun load(
        pkg: String,
        file: File,
        classes: ClassLoader,
    ): List<AnimeSource>
}

// An anime source that is not an AnimeHttpSource (a CloudStream provider)
// but has a site and covers that may need its headers.
interface PluginAnimeSource : AnimeSource {
    val baseUrl: String

    suspend fun fetchCover(url: String): Response
}

object CloudStream {
    private const val RUNTIME_CLASS = "com.mediumdev.scancore.host.cloudstream.Runtime"

    fun isPlugin(path: String) = path.endsWith(".cs3")

    // The runtime's class loader, or null when it is not there yet.
    @Volatile var runtimeClasses: (() -> ClassLoader?)? = null

    private var runtime: CloudStreamRuntime? = null

    @Synchronized
    fun runtime(
        context: Any,
        client: () -> OkHttpClient,
    ): CloudStreamRuntime {
        runtime?.let { return it }
        val classes =
            runtimeClasses?.invoke()
                ?: throw HostError("CloudStream support is not downloaded yet. Open Extensions to get it.", 503)
        val created = Class.forName(RUNTIME_CLASS, true, classes).getDeclaredConstructor().newInstance() as CloudStreamRuntime
        created.start(context, client)
        runtime = created
        return created
    }

    // A plugin's source id, the same SCANCORE gives it from the repository's
    // list (ext::cloudStreamSourceId): the first 8 bytes of MD5 of
    // "cloudstream/<internal name>", as Mihon makes its ids.
    fun sourceId(pkg: String): Long {
        val bytes = MessageDigest.getInstance("MD5").digest("cloudstream/$pkg".toByteArray())
        return (0..7).map { bytes[it].toLong() and 0xff shl 8 * (7 - it) }.reduce(Long::or) and Long.MAX_VALUE
    }
}
