package com.mediumdev.scancore.host.cloudstream

import com.lagradost.cloudstream3.AnimeLoadResponse
import com.lagradost.cloudstream3.DubStatus
import com.lagradost.cloudstream3.Episode
import com.lagradost.cloudstream3.LiveStreamLoadResponse
import com.lagradost.cloudstream3.LoadResponse
import com.lagradost.cloudstream3.MainAPI
import com.lagradost.cloudstream3.MainPageRequest
import com.lagradost.cloudstream3.MovieLoadResponse
import com.lagradost.cloudstream3.SearchResponse
import com.lagradost.cloudstream3.ShowStatus
import com.lagradost.cloudstream3.SubtitleFile
import com.lagradost.cloudstream3.TorrentLoadResponse
import com.lagradost.cloudstream3.TvSeriesLoadResponse
import com.lagradost.cloudstream3.USER_AGENT
import com.lagradost.cloudstream3.utils.ExtractorLink
import com.lagradost.cloudstream3.utils.ExtractorLinkType
import com.mediumdev.scancore.host.HostError
import com.mediumdev.scancore.host.PluginAnimeSource
import eu.kanade.tachiyomi.animesource.AnimeCatalogueSource
import eu.kanade.tachiyomi.animesource.model.AnimeFilter
import eu.kanade.tachiyomi.animesource.model.AnimeFilterList
import eu.kanade.tachiyomi.animesource.model.AnimesPage
import eu.kanade.tachiyomi.animesource.model.Hoster
import eu.kanade.tachiyomi.animesource.model.SAnime
import eu.kanade.tachiyomi.animesource.model.SAnimeEpisodeUpdate
import eu.kanade.tachiyomi.animesource.model.SEpisode
import eu.kanade.tachiyomi.animesource.model.Track
import eu.kanade.tachiyomi.animesource.model.Video
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import okhttp3.Headers.Companion.toHeaders
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import rx.Observable
import java.util.concurrent.ConcurrentHashMap

// A CloudStream plugin as an Aniyomi anime source, so SCANCORE browses,
// searches and plays it like any other: its main page's sections are the
// popular list and a "Section" filter, search is its search, load() gives
// details and episodes (seasons and dub/sub folded into one list), and
// loadLinks() gives the videos, one hoster per extractor that found any.
// A plugin with several providers (sites) is one source with a "Site"
// filter.
class CloudStreamSource(
    override val id: Long,
    private val pluginName: String,
    private val providers: List<MainAPI>,
    private val client: () -> OkHttpClient,
) : AnimeCatalogueSource, PluginAnimeSource {
    private val main = providers.first()

    override val name = if (providers.size == 1) main.name else pluginName
    override val lang = main.lang.ifEmpty { "all" }
    override val supportsLatest = false
    override val baseUrl: String get() = main.mainUrl

    // Covers' headers by address (CloudStream gives them with each result).
    private val posterHeaders = ConcurrentHashMap<String, Map<String, String>>()

    // load() answers by address, for the details and episodes SCANCORE asks
    // for one after the other.
    private val loaded =
        object : LinkedHashMap<String, Pair<Long, LoadResponse>>(16, 0.75f, true) {
            override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Pair<Long, LoadResponse>>) = size > 32
        }

    // ------------------------------------------------------------ filters

    private class Choice(name: String, values: Array<String>) : AnimeFilter.Select<String>(name, values)

    override fun getFilterList(): AnimeFilterList {
        val filters = mutableListOf<AnimeFilter<*>>()
        if (providers.size > 1) filters += Choice("Site", providers.map { it.name }.toTypedArray())
        val sections = sections(main)
        if (sections.size > 1) {
            filters += AnimeFilter.Header("Browse a section (leave the search empty)")
            filters += Choice("Section", sections.map { it.name }.toTypedArray())
        }
        return AnimeFilterList(filters)
    }

    private fun sections(api: MainAPI) = if (api.hasMainPage) api.mainPage else emptyList()

    private fun chosen(
        filters: AnimeFilterList,
        name: String,
    ) = filters.filterIsInstance<AnimeFilter.Select<*>>().firstOrNull { it.name == name }?.state ?: 0

    // ------------------------------------------------------------ browsing

    override suspend fun getPopularAnime(page: Int): AnimesPage = section(main, 0, page)

    override suspend fun getLatestUpdates(page: Int): AnimesPage = getPopularAnime(page)

    override suspend fun getSearchAnime(
        page: Int,
        query: String,
        filters: AnimeFilterList,
    ): AnimesPage {
        val api = providers.getOrNull(chosen(filters, "Site")) ?: main
        if (query.isBlank()) return section(api, chosen(filters, "Section"), page)
        val found = api.search(query, page) ?: return AnimesPage(emptyList(), false)
        return AnimesPage(animes(found.items), found.hasNext)
    }

    private suspend fun section(
        api: MainAPI,
        index: Int,
        page: Int,
    ): AnimesPage {
        val section = sections(api).getOrNull(index) ?: return AnimesPage(emptyList(), false)
        val response = api.getMainPage(page, MainPageRequest(section.name, section.data, section.horizontalImages)) ?: return AnimesPage(emptyList(), false)
        return AnimesPage(animes(response.items.flatMap { it.list }), response.hasNext)
    }

    private fun animes(list: List<SearchResponse>): List<SAnime> =
        list.distinctBy { it.url }.map { r ->
            r.posterUrl?.let { url -> r.posterHeaders?.takeIf { it.isNotEmpty() }?.let { posterHeaders[url] = it } }
            SAnime.create().apply {
                url = r.url
                title = r.name
                thumbnail_url = r.posterUrl
            }
        }

    // ------------------------------------------------------------ a title

    private suspend fun load(url: String): LoadResponse {
        synchronized(loaded) {
            loaded[url]?.let { (at, response) -> if (System.currentTimeMillis() - at < LOAD_KEEP_MS) return response }
        }
        val api = providers.firstOrNull { url.contains(it.mainUrl.substringAfter("://").substringBefore('/')) } ?: main
        val response = api.load(url) ?: throw HostError("$name found nothing at that address.", 404)
        response.posterUrl?.let { poster -> response.posterHeaders?.takeIf { it.isNotEmpty() }?.let { posterHeaders[poster] = it } }
        synchronized(loaded) { loaded[url] = System.currentTimeMillis() to response }
        return response
    }

    override suspend fun getAnimeEpisodeUpdate(
        anime: SAnime,
        episodes: List<SEpisode>,
        fetchDetails: Boolean,
        fetchEpisodes: Boolean,
    ): SAnimeEpisodeUpdate {
        val response = load(anime.url)
        val details = if (fetchDetails) details(anime.url, response) else anime
        return SAnimeEpisodeUpdate(details, if (fetchEpisodes) episodes(response) else episodes)
    }

    override suspend fun getAnimeDetails(anime: SAnime): SAnime = details(anime.url, load(anime.url))

    override suspend fun getEpisodeList(anime: SAnime): List<SEpisode> = episodes(load(anime.url))

    private fun details(
        address: String,
        r: LoadResponse,
    ) = SAnime.create().apply {
        url = address
        title = r.name
        thumbnail_url = r.posterUrl
        background_url = r.backgroundPosterUrl
        description = listOfNotNull(r.plot, r.year?.toString()?.let { "($it)" }).joinToString("\n\n").ifEmpty { null }
        genre = r.tags?.joinToString(", ")
        artist = r.actors?.take(6)?.joinToString(", ") { it.actor.name }
        status =
            when ((r as? TvSeriesLoadResponse)?.showStatus ?: (r as? AnimeLoadResponse)?.showStatus) {
                ShowStatus.Ongoing -> SAnime.ONGOING
                ShowStatus.Completed -> SAnime.COMPLETED
                null -> if (r is MovieLoadResponse) SAnime.COMPLETED else SAnime.UNKNOWN
            }
        initialized = true
    }

    // Newest first, as Aniyomi's sources give them. Several seasons are
    // numbered on from one to the next, with "S2 E1" in the name.
    private fun episodes(r: LoadResponse): List<SEpisode> {
        val single = { data: String ->
            listOf(
                SEpisode.create().apply {
                    url = data
                    name = r.name
                    episode_number = 1f
                },
            )
        }
        val (list, dubs) =
            when (r) {
                is MovieLoadResponse -> return single(r.dataUrl)
                is LiveStreamLoadResponse -> return single(r.dataUrl)
                is TorrentLoadResponse -> throw HostError("$name gives torrents, which SCANCORE cannot play.", 501)
                is TvSeriesLoadResponse -> r.episodes.map { it to null } to false
                is AnimeLoadResponse -> {
                    val kinds = r.episodes.filterValues { it.isNotEmpty() }
                    kinds.flatMap { (dub, eps) -> eps.map { it to dub } } to (kinds.size > 1)
                }
                else -> return emptyList()
            }
        val seasons = list.mapNotNull { it.first.season }.toSet().size > 1
        val sorted = list.sortedWith(compareBy({ it.first.season ?: 0 }, { it.first.episode ?: Int.MAX_VALUE }))
        return sorted.mapIndexed { index, (e, dub) -> episode(e, dub, dubs, seasons, index) }.reversed()
    }

    private fun episode(
        e: Episode,
        dub: DubStatus?,
        dubs: Boolean,
        seasons: Boolean,
        index: Int,
    ) = SEpisode.create().apply {
        url = e.data
        val number = e.episode ?: (index + 1)
        val label = if (seasons) "S${e.season ?: 1} E$number" else "Episode $number"
        name = e.name?.takeIf { it.isNotBlank() }?.let { if (seasons) "$label · $it" else it } ?: label
        episode_number = if (seasons) (index + 1).toFloat() else number.toFloat()
        scanlator =
            when {
                !dubs -> null
                dub == DubStatus.Dubbed -> "Dub"
                dub == DubStatus.Subbed -> "Sub"
                else -> null
            }
        date_upload = e.date ?: 0L
        summary = e.description
        preview_url = e.posterUrl
    }

    // ------------------------------------------------------------ videos

    override suspend fun getHosterList(episode: SEpisode): List<Hoster> {
        val links = mutableListOf<ExtractorLink>()
        val subtitles = mutableListOf<SubtitleFile>()
        // The provider the episode came from: the one whose site its
        // address names, else the first.
        val api = providers.firstOrNull { episode.url.contains(it.mainUrl.substringAfter("://").substringBefore('/')) } ?: main
        val done =
            withTimeoutOrNull(LINKS_TIMEOUT_MS) {
                api.loadLinks(episode.url, false, { synchronized(subtitles) { subtitles += it } }, { synchronized(links) { links += it } })
            }
        val found = synchronized(links) { links.toList() }
        val tracks = synchronized(subtitles) { subtitles.distinctBy { it.url }.map { Track(it.url, it.lang) } }
        val playable = found.filter { it.type != ExtractorLinkType.TORRENT && it.type != ExtractorLinkType.MAGNET && it.url.isNotBlank() }
        if (playable.isEmpty()) {
            throw HostError(
                if (done == null) "$name took too long to find videos." else if (found.isNotEmpty()) "$name only found torrents, which SCANCORE cannot play." else "$name found no videos for this episode.",
                404,
            )
        }
        return playable.groupBy { it.source }.map { (source, list) ->
            Hoster(hosterName = source, videoList = list.sortedByDescending { it.quality }.map { video(it, tracks) })
        }
    }

    private fun video(
        link: ExtractorLink,
        subtitles: List<Track>,
    ): Video {
        val headers = link.getAllHeaders().toMutableMap()
        if (headers.keys.none { it.equals("user-agent", ignoreCase = true) }) headers["User-Agent"] = USER_AGENT
        val quality = link.quality.takeIf { it > 0 }
        return Video(
            videoUrl = link.url,
            videoTitle = listOfNotNull(link.name, quality?.let { "${it}p" }?.takeIf { !link.name.contains(it) }).joinToString(" · "),
            resolution = quality,
            headers = headers.toHeaders(),
            subtitleTracks = subtitles,
            audioTracks = link.audioTracks.map { Track(it.url, "") },
            initialized = true,
        )
    }

    // ------------------------------------------------------------ covers

    override suspend fun fetchCover(url: String): Response {
        val headers = (posterHeaders[url] ?: emptyMap()).toMutableMap()
        if (headers.keys.none { it.equals("user-agent", ignoreCase = true) }) headers["User-Agent"] = USER_AGENT
        return client().newCall(Request.Builder().url(url).headers(headers.toHeaders()).build()).execute()
    }

    // ------------------------------------------------------------ RxJava (unused)

    @Deprecated("Use the non-RxJava API instead")
    override fun fetchPopularAnime(page: Int): Observable<AnimesPage> = Observable.fromCallable { runBlocking { getPopularAnime(page) } }

    @Deprecated("Use the non-RxJava API instead")
    override fun fetchSearchAnime(
        page: Int,
        query: String,
        filters: AnimeFilterList,
    ): Observable<AnimesPage> = Observable.fromCallable { runBlocking { getSearchAnime(page, query, filters) } }

    @Deprecated("Use the non-RxJava API instead")
    override fun fetchLatestUpdates(page: Int): Observable<AnimesPage> = fetchPopularAnime(page)

    override fun toString() = "$name (CloudStream)"
}

// How long load() answers are reused, and how long loadLinks may look.
private const val LOAD_KEEP_MS = 5 * 60_000L
private const val LINKS_TIMEOUT_MS = 90_000L
