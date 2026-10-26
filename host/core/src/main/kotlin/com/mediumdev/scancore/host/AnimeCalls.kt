package com.mediumdev.scancore.host

import eu.kanade.tachiyomi.animesource.AnimeCatalogueSource
import eu.kanade.tachiyomi.animesource.AnimeSource
import eu.kanade.tachiyomi.animesource.ConfigurableAnimeSource
import eu.kanade.tachiyomi.animesource.model.AnimeFilterList
import eu.kanade.tachiyomi.animesource.model.FetchType
import eu.kanade.tachiyomi.animesource.model.Hoster
import eu.kanade.tachiyomi.animesource.model.Hoster.Companion.toHosterList
import eu.kanade.tachiyomi.animesource.model.SAnime
import eu.kanade.tachiyomi.animesource.model.SEpisode
import eu.kanade.tachiyomi.animesource.model.Video
import eu.kanade.tachiyomi.animesource.online.AnimeHttpSource
import eu.kanade.tachiyomi.animesource.online.ParsedAnimeHttpSource
import eu.kanade.tachiyomi.network.GET
import eu.kanade.tachiyomi.network.await
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import java.io.File
import java.util.concurrent.atomic.AtomicLong

// SCANCORE's calls for Aniyomi anime sources (src/extensions/AnimeSource.h
// lists them). Videos are found as Aniyomi's player does: the episode's
// hosters first, each hoster's videos when SCANCORE asks (lazy ones only
// then), and a video's final address only for the one that plays.
class AnimeCalls(
    private val memos: MemoStore,
    private val settings: SettingsBridge,
) {
    private val handles = Handles()

    suspend fun call(
        method: String,
        source: AnimeSource,
        params: JsonObject,
    ): JsonElement {
        val page = params.int("page", 1)
        val base = (source as? AnimeHttpSource)?.baseUrl
        return when (method) {
            "popular" -> AnimeCodec.animes(catalogue(source).getPopularAnime(page).also { remember(source, it.animes) }, base)
            "latest" -> AnimeCodec.animes(catalogue(source).getLatestUpdates(page).also { remember(source, it.animes) }, base)
            "search" -> {
                val filters = AnimeCodec.applyFilters(source.getFilterList(), params["filters"] as? JsonArray ?: JsonArray(emptyList()))
                AnimeCodec.animes(
                    catalogue(source).getSearchAnime(page, params.optString("query").orEmpty(), filters).also { remember(source, it.animes) },
                    base,
                )
            }
            "details" -> {
                val anime = anime(source, params)
                val updated =
                    if (anime.fetch_type == FetchType.Seasons) {
                        source.getAnimeSeasonUpdate(anime, emptyList(), fetchDetails = true, fetchSeasons = false).anime
                    } else {
                        source.getAnimeEpisodeUpdate(anime, emptyList(), fetchDetails = true, fetchEpisodes = false).anime
                    }
                remember(source, listOf(updated))
                AnimeCodec.details(updated, base)
            }
            "episodes" -> {
                val update = source.getAnimeEpisodeUpdate(anime(source, params), emptyList(), fetchDetails = false, fetchEpisodes = true)
                for (e in update.episodes) if (e.memo.isNotEmpty()) memos.putEpisode(source.id, e.url, e.memo)
                AnimeCodec.episodes(update.episodes)
            }
            "seasons" -> {
                val anime = anime(source, params).apply { fetch_type = FetchType.Seasons }
                val update = source.getAnimeSeasonUpdate(anime, emptyList(), fetchDetails = false, fetchSeasons = true)
                remember(source, update.seasons)
                AnimeCodec.seasons(update.seasons, base)
            }
            "hosters" -> hosters(source, episode(source, params))
            "hosterVideos" -> {
                val hoster = handles.find<Hoster>(source, params.string("hoster"))
                videos(source, hoster.videoList ?: source.getVideoList(hoster))
            }
            "resolveVideo" -> {
                val video = handles.find<Video>(source, params.string("video"))
                val resolved =
                    if (video.initialized || source !is AnimeHttpSource) {
                        video
                    } else {
                        source.resolveVideo(video) ?: throw HostError("This video is not available.", 404)
                    }
                val ready = LocalServers.serve(source, resolved.copy(initialized = true))
                AnimeCodec.video(handles.put(source, ready), ready, (source as? AnimeHttpSource)?.headers)
            }
            "fetchImage" -> fetchImage(source, params)
            "filters" -> AnimeCodec.filters(source.getFilterList())
            "preferences" -> (source as? ConfigurableAnimeSource)?.let { settings.describe(Configurable.of(it)) } ?: JsonArray(emptyList())
            "setPreference" -> {
                settings.set(configurable(source), params.string("key"), params["value"] ?: JsonNull)
                JsonNull
            }
            else -> throw HostError("The host does not know \"$method\" for anime.", 400)
        }
    }

    private fun catalogue(source: AnimeSource) = source as? AnimeCatalogueSource ?: throw HostError("${source.name} cannot be browsed.", 501)

    private fun configurable(source: AnimeSource) =
        (source as? ConfigurableAnimeSource)?.let(Configurable::of) ?: throw HostError("${source.name} has no settings.", 501)

    // Extensions read the title or name of what they are given now and
    // then, so both are filled in when SCANCORE knows them.
    private fun anime(
        source: AnimeSource,
        params: JsonObject,
    ): SAnime =
        SAnime.create().also {
            it.url = params.string("url")
            it.title = params.optString("title").orEmpty()
            if (params.optString("fetchType") == "seasons") it.fetch_type = FetchType.Seasons
            memos.anime(source.id, it.url)?.let { memo -> it.memo = memo }
        }

    private fun episode(
        source: AnimeSource,
        params: JsonObject,
    ): SEpisode =
        SEpisode.create().also {
            it.url = params.string("url")
            it.name = params.optString("name").orEmpty()
            memos.episode(source.id, it.url)?.let { memo -> it.memo = memo }
        }

    private fun remember(
        source: AnimeSource,
        animes: List<SAnime>,
    ) {
        for (a in animes) if (a.memo.isNotEmpty()) memos.putAnime(source.id, a.url, a.memo)
    }

    // The episode's hosters, preferred first. Extensions from before
    // hosters (lib 14) give videos straight away: one unnamed hoster then.
    private suspend fun hosters(
        source: AnimeSource,
        episode: SEpisode,
    ): JsonArray {
        val http = source as? AnimeHttpSource
        val list =
            when {
                http != null && hasHosters(http) -> http.run { getHosterList(episode).sortHosters() }
                http != null -> http.run { getVideoList(episode).sortVideos() }.toHosterList()
                else -> source.getHosterList(episode)
            }
        return buildJsonArray {
            for (hoster in list) {
                add(
                    buildJsonObject {
                        put("id", handles.put(source, hoster))
                        put("name", if (hoster.hosterName == Hoster.NO_HOSTER_LIST) "" else hoster.hosterName)
                        put("lazy", hoster.lazy)
                        hoster.videoList?.let { put("videos", videos(source, it)) }
                    },
                )
            }
        }
    }

    // As Aniyomi decides: the source (or a class between it and the API's)
    // declares one of the hoster calls.
    private fun hasHosters(source: AnimeHttpSource): Boolean {
        var current: Class<*> = source.javaClass
        while (current != ParsedAnimeHttpSource::class.java && current != AnimeHttpSource::class.java && current != Any::class.java) {
            if (current.declaredMethods.any { it.name in hosterMethods }) return true
            current = current.superclass ?: return false
        }
        return false
    }

    private val hosterMethods = setOf("getHosterList", "hosterListRequest", "hosterListParse")

    private suspend fun videos(
        source: AnimeSource,
        list: List<Video>,
    ): JsonArray {
        val http = source as? AnimeHttpSource
        // Lib 14: a video without an address gets one from the source.
        val withUrls =
            list.map { video ->
                if (video.videoUrl != "null" || http == null) video else video.copy(videoUrl = http.getVideoUrl(video))
            }
        val sorted = http?.run { withUrls.sortVideos() } ?: withUrls
        return buildJsonArray {
            for (v in sorted) {
                val served = LocalServers.serve(source, v)
                add(AnimeCodec.video(handles.put(source, served), served, http?.headers))
            }
        }
    }

    // Covers through the source's own client and headers.
    private suspend fun fetchImage(
        source: AnimeSource,
        params: JsonObject,
    ): JsonElement {
        val url =
            params["page"]?.jsonObject?.get("imageUrl")?.jsonPrimitive?.content?.ifEmpty { null }
                ?: throw HostError("The request has no image.", 400)
        // CloudStream plugins: covers with the headers their results gave.
        if (source is PluginAnimeSource) {
            return kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
                source.fetchCover(url).use { writeImage(it, File(params.string("target"))) }
            }
        }
        val http = source as? AnimeHttpSource ?: throw HostError("${source.name} has no images.", 501)
        return http.client.newCall(GET(url, http.headers)).await().use { writeImage(it, File(params.string("target"))) }
    }
}

// Hosters and videos SCANCORE was given, by id, so it can ask for a
// hoster's videos or a video's final address later. The newest few hundred
// are kept; an id names its source, so another source cannot use it.
private class Handles {
    private val counter = AtomicLong()
    private val map =
        object : LinkedHashMap<String, Any>(64, 0.75f, true) {
            override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Any>) = size > 1024
        }

    @Synchronized
    fun put(
        source: AnimeSource,
        value: Any,
    ): String {
        val id = "${source.id}.${counter.incrementAndGet()}"
        map[id] = value
        return id
    }

    @Synchronized
    fun get(
        source: AnimeSource,
        id: String,
    ): Any? {
        if (!id.startsWith("${source.id}.")) throw HostError("That video belongs to another source.", 400)
        return map[id]
    }
}

private inline fun <reified T> Handles.find(
    source: AnimeSource,
    id: String,
): T = get(source, id) as? T ?: throw HostError("The episode's video list expired. Open the episode again.", 410)
