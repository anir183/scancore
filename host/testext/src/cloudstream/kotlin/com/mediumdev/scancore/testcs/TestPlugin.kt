package com.mediumdev.scancore.testcs

import android.content.Context
import com.lagradost.cloudstream3.HomePageResponse
import com.lagradost.cloudstream3.LoadResponse
import com.lagradost.cloudstream3.MainAPI
import com.lagradost.cloudstream3.MainPageRequest
import com.lagradost.cloudstream3.SearchResponse
import com.lagradost.cloudstream3.SubtitleFile
import com.lagradost.cloudstream3.TvType
import com.lagradost.cloudstream3.app
import com.lagradost.cloudstream3.mainPageOf
import com.lagradost.cloudstream3.newEpisode
import com.lagradost.cloudstream3.newHomePageResponse
import com.lagradost.cloudstream3.newMovieSearchResponse
import com.lagradost.cloudstream3.newSubtitleFile
import com.lagradost.cloudstream3.newTvSeriesLoadResponse
import com.lagradost.cloudstream3.plugins.CloudstreamPlugin
import com.lagradost.cloudstream3.plugins.Plugin
import com.lagradost.cloudstream3.utils.AppUtils.parseJson
import com.lagradost.cloudstream3.utils.ExtractorLink
import com.lagradost.cloudstream3.utils.ExtractorLinkType
import com.lagradost.cloudstream3.utils.newExtractorLink

// SCANCORE's test CloudStream plugin: a provider reading the fixture site
// the host tests serve (tests/fixtures/host/cs), packaged as a .cs3 by
// `./gradlew :testext:cs3`.
@CloudstreamPlugin
class TestPlugin : Plugin() {
    override fun load(context: Context) {
        registerMainAPI(TestProvider())
    }
}

data class Item(val name: String, val url: String, val poster: String? = null)

data class Title(val name: String, val poster: String? = null, val plot: String? = null, val episodes: List<Ep> = emptyList())

data class Ep(val name: String? = null, val data: String, val season: Int? = null, val episode: Int? = null)

class TestProvider : MainAPI() {
    override var mainUrl = System.getProperty("scancore.testext.base") ?: "http://fixture.test"
    override var name = "Test CloudStream"
    override var lang = "en"
    override val hasMainPage = true
    override val supportedTypes = setOf(TvType.TvSeries)
    override val mainPage = mainPageOf("cs/home.json" to "Popular", "cs/new.json" to "New")

    private suspend fun items(path: String) = parseJson<List<Item>>(app.get("$mainUrl/$path").text).map(::result)

    private fun result(item: Item): SearchResponse = newMovieSearchResponse(item.name, "$mainUrl/${item.url}", TvType.TvSeries) { posterUrl = item.poster?.let { "$mainUrl/$it" } }

    override suspend fun getMainPage(page: Int, request: MainPageRequest): HomePageResponse = newHomePageResponse(request.name, items(request.data), hasNext = page < 2)

    override suspend fun search(query: String): List<SearchResponse> = items("cs/search.json?q=$query")

    override suspend fun load(url: String): LoadResponse {
        val title = parseJson<Title>(app.get(url).text)
        val episodes = title.episodes.map { e -> newEpisode(e.data) { name = e.name; season = e.season; episode = e.episode } }
        return newTvSeriesLoadResponse(title.name, url, TvType.TvSeries, episodes) {
            posterUrl = title.poster?.let { "$mainUrl/$it" }
            plot = title.plot
            tags = listOf("Test")
        }
    }

    override suspend fun loadLinks(data: String, isCasting: Boolean, subtitleCallback: (SubtitleFile) -> Unit, callback: (ExtractorLink) -> Unit): Boolean {
        subtitleCallback(newSubtitleFile("English", "$mainUrl/cs/$data.vtt"))
        callback(newExtractorLink("Alpha", "Alpha", "$mainUrl/cs/$data-720.m3u8", ExtractorLinkType.M3U8) { quality = 720; referer = "$mainUrl/" })
        callback(newExtractorLink("Alpha", "Alpha", "$mainUrl/cs/$data-1080.m3u8", ExtractorLinkType.M3U8) { quality = 1080; referer = "$mainUrl/" })
        callback(newExtractorLink("Beta", "Beta", "magnet:?xt=urn:btih:test", ExtractorLinkType.MAGNET))
        return true
    }
}
