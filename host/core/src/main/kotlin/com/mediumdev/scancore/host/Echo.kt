package com.mediumdev.scancore.host

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import okhttp3.OkHttpClient
import java.io.File
import java.security.MessageDigest

// Echo extensions (.eapk files: APKs built against Echo's extension
// library). Like CloudStream's, their runtime (host/echo: Echo's library
// and the adapter, the Unabandon Public License) is not on the host's class
// path: the platform says where its classes are, and it is loaded the first
// time an extension is.
interface EchoRuntime {
    // Once, before the first extension: the app's Context, the host's
    // client and a folder for the extensions' settings.
    fun start(
        context: Any,
        client: () -> OkHttpClient,
        settings: File,
    )

    // The extension's music source. `classes` loads its code with the
    // runtime as its parent; `meta` is its manifest's meta-data (class, id,
    // name, version, ...).
    fun load(
        pkg: String,
        file: File,
        classes: ClassLoader,
        meta: Map<String, String>,
    ): List<MusicSource>
}

// A music source for SCANCORE's Music section, in the terms of its music
// plugins (docs/music-plugins.md): songs are {id, title, artists, album,
// cover, duration (seconds), ...}, and whatever else a song carries comes
// back to stream and lyrics as it was given.
interface MusicSource {
    val id: Long
    val name: String
    val lang: String
    val baseUrl: String

    suspend fun search(
        query: String,
        page: Int,
    ): JsonArray

    suspend fun popular(page: Int): JsonArray

    // {url, headers}.
    suspend fun stream(song: JsonObject): JsonObject

    // {synced: LRC text, plain}, empty when it has none.
    suspend fun lyrics(song: JsonObject): JsonObject
}

object Echo {
    private const val RUNTIME_CLASS = "com.mediumdev.scancore.host.echo.Runtime"

    fun isPlugin(path: String) = path.endsWith(".eapk")

    // The runtime's class loader, or null when it is not there yet.
    @Volatile var runtimeClasses: (() -> ClassLoader?)? = null

    private var runtime: EchoRuntime? = null

    @Synchronized
    fun runtime(
        context: Any,
        client: () -> OkHttpClient,
        settings: File,
    ): EchoRuntime {
        runtime?.let { return it }
        val classes =
            runtimeClasses?.invoke()
                ?: throw HostError("Echo support is not downloaded yet. Open Extensions to get it.", 503)
        val created = Class.forName(RUNTIME_CLASS, true, classes).getDeclaredConstructor().newInstance() as EchoRuntime
        created.start(context, client, settings)
        runtime = created
        return created
    }

    // An extension's source id, the same SCANCORE gives it
    // (ext::echoSourceId): the first 8 bytes of MD5 of "echo/<id>".
    fun sourceId(pkg: String): Long {
        val bytes = MessageDigest.getInstance("MD5").digest("echo/$pkg".toByteArray())
        return (0..7).map { bytes[it].toLong() and 0xff shl 8 * (7 - it) }.reduce(Long::or) and Long.MAX_VALUE
    }
}
