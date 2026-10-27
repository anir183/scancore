package com.mediumdev.scancore.host.echo

import com.mediumdev.scancore.host.Echo
import com.mediumdev.scancore.host.EchoRuntime
import com.mediumdev.scancore.host.HostError
import com.mediumdev.scancore.host.MusicSource
import dev.brahmkshatriya.echo.common.clients.ExtensionClient
import dev.brahmkshatriya.echo.common.clients.SearchFeedClient
import dev.brahmkshatriya.echo.common.clients.TrackClient
import dev.brahmkshatriya.echo.common.models.ExtensionType
import dev.brahmkshatriya.echo.common.models.ImportType
import dev.brahmkshatriya.echo.common.models.Metadata
import dev.brahmkshatriya.echo.common.models.NetworkConnection
import dev.brahmkshatriya.echo.common.providers.MetadataProvider
import dev.brahmkshatriya.echo.common.providers.NetworkConnectionProvider
import dev.brahmkshatriya.echo.common.settings.Settings
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.OkHttpClient
import java.io.File

// The host's way into Echo (core's Echo.kt makes one by name): loads an
// extension as Echo's ExtensionParser does (the manifest's meta-data names
// its class), gives it what Echo's app injects (settings, its metadata, the
// network state) and initializes it. Music extensions (search and tracks)
// become a music source; lyrics, tracker and other kinds are not shown.
class Runtime : EchoRuntime {
    private lateinit var client: () -> OkHttpClient
    private lateinit var settings: File

    override fun start(
        context: Any,
        client: () -> OkHttpClient,
        settings: File,
    ) {
        this.client = client
        this.settings = settings.apply { mkdirs() }
    }

    @Synchronized
    override fun load(
        pkg: String,
        file: File,
        classes: ClassLoader,
        meta: Map<String, String>,
    ): List<MusicSource> {
        val className = meta["class"] ?: throw HostError("The extension does not say which class it is.", 422)
        val name = meta["name"] ?: pkg
        val extension =
            try {
                Class.forName(className, true, classes).getDeclaredConstructor().newInstance() as ExtensionClient
            } catch (e: ClassCastException) {
                throw HostError("$className is not an Echo extension.", 422)
            }
        if (extension !is SearchFeedClient || extension !is TrackClient) {
            throw HostError("$name is not an Echo music extension (only those play in SCANCORE).", 422)
        }
        try {
            extension.setSettings(JsonSettings(File(settings, "${pkg.replace(Regex("[^A-Za-z0-9._-]"), "_")}.json")))
            (extension as? MetadataProvider)?.setMetadata(
                Metadata(
                    className = className,
                    path = file.absolutePath,
                    importType = ImportType.App,
                    type = ExtensionType.MUSIC,
                    id = meta["id"] ?: pkg,
                    name = name,
                    version = meta["version"].orEmpty(),
                    description = meta["description"].orEmpty(),
                    author = meta["author"].orEmpty(),
                    authorUrl = meta["author_url"],
                    repoUrl = meta["repo_url"],
                    updateUrl = meta["update_url"],
                ),
            )
            (extension as? NetworkConnectionProvider)?.setNetworkConnection(NetworkConnection.Unmetered)
            runBlocking {
                extension.onInitialize()
                extension.onExtensionSelected()
            }
        } catch (e: Throwable) {
            throw HostError("$name did not start: ${e.message ?: e.javaClass.simpleName}", 500)
        }
        return listOf(EchoMusicSource(Echo.sourceId(pkg), name, meta["repo_url"] ?: meta["author_url"].orEmpty(), extension, client))
    }
}

// An extension's settings as Echo's app keeps them (SharedPreferences
// there), in a JSON file of its own.
private class JsonSettings(
    private val file: File,
) : Settings {
    private val values: MutableMap<String, JsonElement> =
        runCatching { Json.parseToJsonElement(file.readText()) as JsonObject }.getOrNull()?.toMutableMap() ?: mutableMapOf()

    @Synchronized
    private fun put(
        key: String,
        value: JsonElement?,
    ) {
        if (value == null || value == JsonNull) values.remove(key) else values[key] = value
        runCatching { file.writeText(JsonObject(values).toString()) }
    }

    @Synchronized
    private fun get(key: String) = values[key]

    override fun getString(key: String) = (get(key) as? JsonPrimitive)?.takeIf { it.isString }?.content

    override fun putString(
        key: String,
        value: String?,
    ) = put(key, value?.let(::JsonPrimitive))

    override fun getStringSet(key: String) = runCatching { get(key)?.jsonArray?.map { it.jsonPrimitive.content }?.toSet() }.getOrNull()

    override fun putStringSet(
        key: String,
        value: Set<String>?,
    ) = put(key, value?.let { kotlinx.serialization.json.JsonArray(it.map(::JsonPrimitive)) })

    override fun getInt(key: String) = (get(key) as? JsonPrimitive)?.intOrNull

    override fun putInt(
        key: String,
        value: Int?,
    ) = put(key, value?.let(::JsonPrimitive))

    override fun getBoolean(key: String) = (get(key) as? JsonPrimitive)?.booleanOrNull

    override fun putBoolean(
        key: String,
        value: Boolean?,
    ) = put(key, value?.let(::JsonPrimitive))
}
