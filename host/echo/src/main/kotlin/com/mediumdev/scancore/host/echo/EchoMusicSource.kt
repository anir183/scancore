package com.mediumdev.scancore.host.echo

import com.mediumdev.scancore.host.HostError
import com.mediumdev.scancore.host.MusicSource
import dev.brahmkshatriya.echo.common.clients.ExtensionClient
import dev.brahmkshatriya.echo.common.clients.HomeFeedClient
import dev.brahmkshatriya.echo.common.clients.LyricsClient
import dev.brahmkshatriya.echo.common.clients.SearchFeedClient
import dev.brahmkshatriya.echo.common.clients.TrackClient
import dev.brahmkshatriya.echo.common.models.Feed
import dev.brahmkshatriya.echo.common.models.ImageHolder
import dev.brahmkshatriya.echo.common.models.Lyrics
import dev.brahmkshatriya.echo.common.models.Shelf
import dev.brahmkshatriya.echo.common.models.Streamable
import dev.brahmkshatriya.echo.common.models.Track
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import kotlinx.serialization.json.putJsonObject
import okhttp3.OkHttpClient
import java.util.concurrent.ConcurrentHashMap

// An Echo music extension as a SCANCORE music source: a search's (or the
// home feed's) shelves give its songs; a song plays from its best server's
// first source the player can open; lyrics come from the extension when it
// has them. A song carries its Track (Echo's JSON) back for those.
class EchoMusicSource(
    override val id: Long,
    override val name: String,
    override val baseUrl: String,
    private val extension: ExtensionClient,
    private val client: () -> OkHttpClient,
) : MusicSource {
    override val lang = "all"

    private val json =
        Json {
            ignoreUnknownKeys = true
            encodeDefaults = false
        }

    // Where the next page of a feed starts, by feed and page.
    private val continuations = ConcurrentHashMap<String, String>()

    override suspend fun search(
        query: String,
        page: Int,
    ): JsonArray = songs("search|$query", page) { (extension as SearchFeedClient).loadSearchFeed(query) }

    override suspend fun popular(page: Int): JsonArray {
        val home = extension as? HomeFeedClient ?: return JsonArray(emptyList())
        return songs("home", page) { home.loadHomeFeed() }
    }

    private suspend fun songs(
        key: String,
        page: Int,
        feed: suspend () -> Feed<Shelf>,
    ): JsonArray {
        val from = if (page <= 1) null else continuations["$key|$page"] ?: return JsonArray(emptyList())
        val loaded = feed()
        val data = loaded.getPagedData(loaded.notSortTabs.firstOrNull()).pagedData.loadPage(from)
        data.continuation?.let { continuations["$key|${page + 1}"] = it }
        val tracks =
            data.data.flatMap { shelf ->
                when (shelf) {
                    is Shelf.Lists.Tracks -> shelf.list
                    is Shelf.Lists.Items -> shelf.list.filterIsInstance<Track>()
                    is Shelf.Item -> listOfNotNull(shelf.media as? Track)
                    else -> emptyList()
                }
            }
        return buildJsonArray {
            for (t in tracks.distinctBy { it.id }.take(200)) add(song(t))
        }
    }

    private fun song(t: Track) =
        buildJsonObject {
            put("id", t.id)
            put("title", t.title)
            putJsonArray("artists") { t.artists.forEach { add(kotlinx.serialization.json.JsonPrimitive(it.name)) } }
            t.album?.title?.let { put("album", it) }
            image(t.cover)?.let { put("cover", it) }
            t.duration?.let { put("duration", it / 1000.0) }
            put("track", json.encodeToString(Track.serializer(), t))
        }

    private fun image(holder: ImageHolder?) = (holder as? ImageHolder.NetworkRequestImageHolder)?.request?.url

    private fun track(song: JsonObject): Track =
        song["track"]?.jsonPrimitive?.content?.let { json.decodeFromString(Track.serializer(), it) }
            ?: throw HostError("That song did not come from $name.", 400)

    override suspend fun stream(song: JsonObject): JsonObject {
        val tracks = extension as TrackClient
        val loaded = tracks.loadTrack(track(song), false)
        if (loaded.isPlayable != Track.Playable.Yes) {
            throw HostError(
                when (val p = loaded.isPlayable) {
                    is Track.Playable.No -> p.reason
                    Track.Playable.RegionLocked -> "This song is not available in your region."
                    Track.Playable.Unreleased -> "This song is not out yet."
                    else -> "This song cannot be played."
                },
                403,
            )
        }
        val servers = loaded.servers.sortedByDescending { it.quality }
        if (servers.isEmpty()) throw HostError("$name has no way to play this song.", 404)
        var problem: String? = null
        for (server in servers) {
            val media =
                try {
                    tracks.loadStreamableMedia(server, false) as? Streamable.Media.Server ?: continue
                } catch (e: Exception) {
                    problem = e.message
                    continue
                }
            // Plain addresses the player opens, sound before video, best first.
            val http =
                media.sources
                    .filterIsInstance<Streamable.Source.Http>()
                    .filter { it.decryption == null }
                    .sortedWith(compareBy({ it.isVideo }, { -it.quality }))
                    .firstOrNull() ?: continue
            return buildJsonObject {
                put("url", http.request.url)
                putJsonObject("headers") { http.request.headers.forEach { (k, v) -> put(k, v) } }
            }
        }
        throw HostError(problem ?: "$name gave no stream SCANCORE can play (protected or raw audio).", 404)
    }

    override suspend fun lyrics(song: JsonObject): JsonObject {
        val lyrics = extension as? LyricsClient ?: return JsonObject(emptyMap())
        val t = track(song)
        val first =
            lyrics.searchTrackLyrics(id.toString(), t).let { feed ->
                feed.getPagedData(feed.notSortTabs.firstOrNull()).pagedData.loadPage(null).data.firstOrNull()
            } ?: return JsonObject(emptyMap())
        val full = if (first.lyrics == null) lyrics.loadLyrics(first) else first
        return buildJsonObject {
            when (val l = full.lyrics) {
                is Lyrics.Simple -> put("plain", l.text)
                is Lyrics.Timed -> put("synced", l.list.joinToString("\n") { "${stamp(it.startTime)}${it.text}" })
                is Lyrics.WordByWord ->
                    put("synced", l.list.filter { it.isNotEmpty() }.joinToString("\n") { line -> stamp(line.first().startTime) + line.joinToString("") { it.text } })
                null -> {}
            }
        }
    }

    // [mm:ss.xx] for a time in milliseconds.
    private fun stamp(ms: Long) = "[%02d:%02d.%02d]".format(ms / 60000, ms / 1000 % 60, ms / 10 % 100)
}
