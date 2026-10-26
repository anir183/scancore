// A tiny Mihon extension for SCANCORE's tests, written against the same API
// real extensions use (lib 1.4 style: request/parse overrides). It reads a
// fixture site on fixture.test that the tests serve, and touches what real
// extensions lean on: Injekt's Json, SimpleDateFormat, JavaScript, filters,
// settings, an interceptor on its own client, pages that need a second
// request for their image, and the app's main thread.
package com.mediumdev.scancore.testext

import android.content.SharedPreferences
import android.os.Handler
import android.os.Looper
import androidx.preference.ListPreference
import androidx.preference.MultiSelectListPreference
import androidx.preference.PreferenceScreen
import app.cash.quickjs.QuickJs
import eu.kanade.tachiyomi.network.GET
import eu.kanade.tachiyomi.source.ConfigurableSource
import eu.kanade.tachiyomi.source.model.Filter
import eu.kanade.tachiyomi.source.model.FilterList
import eu.kanade.tachiyomi.source.model.MangasPage
import eu.kanade.tachiyomi.source.model.Page
import eu.kanade.tachiyomi.source.model.SChapter
import eu.kanade.tachiyomi.source.model.SManga
import eu.kanade.tachiyomi.source.model.SMangaUpdate
import eu.kanade.tachiyomi.source.online.HttpSource
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.float
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import uy.kohesive.injekt.injectLazy
import java.text.SimpleDateFormat
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class TestSource :
    HttpSource(),
    ConfigurableSource {
    override val name = "Test Source"
    override val lang = "en"
    override val supportsLatest = true
    override val baseUrl: String = System.getProperty("scancore.testext.base") ?: "http://fixture.test"

    private val json: Json by injectLazy()
    private val preferences: SharedPreferences by lazy { getSourcePreferences() }
    private val dates = SimpleDateFormat("yyyy-MM-dd", Locale.US)

    // Made with the source, as Komga's is: needs the app's main thread (#20).
    private val main = Handler(Looper.getMainLooper())

    // Every request of this source carries a header the fixture site checks,
    // images included, so the tests see the source's own client was used.
    override val client: OkHttpClient =
        network.client
            .newBuilder()
            .addInterceptor { chain -> chain.proceed(chain.request().newBuilder().header("X-Test-Source", "1").build()) }
            .build()

    private fun Response.obj(): JsonObject = json.parseToJsonElement(body.string()).jsonObject

    // As Keiyoushi's sources (KeiSource) do: one update per series at a
    // time. The series page asks for details and chapters together.
    private val updating = ConcurrentHashMap<String, Boolean>()

    override suspend fun getMangaUpdate(
        manga: SManga,
        chapters: List<SChapter>,
        fetchDetails: Boolean,
        fetchChapters: Boolean,
    ): SMangaUpdate {
        check(updating.putIfAbsent(manga.url, true) == null) { "getMangaUpdate must not be called concurrently for same manga" }
        try {
            // A slow site: a second call would arrive while this one runs.
            Thread.sleep(300)
            return super.getMangaUpdate(manga, chapters, fetchDetails, fetchChapters)
        } finally {
            updating.remove(manga.url)
        }
    }

    private fun mangas(response: Response): MangasPage {
        val root = response.obj()
        val items =
            root["items"]!!.jsonArray.map {
                SManga.create().apply {
                    url = it.jsonObject["url"]!!.jsonPrimitive.content
                    title = it.jsonObject["title"]!!.jsonPrimitive.content
                    thumbnail_url = it.jsonObject["cover"]?.jsonPrimitive?.content
                }
            }
        return MangasPage(items, root["next"]?.jsonPrimitive?.content == "true")
    }

    override fun popularMangaRequest(page: Int) = GET("$baseUrl/popular?page=$page&quality=${preferences.getString("quality", "high")}", headers)

    override fun popularMangaParse(response: Response) = mangas(response)

    override fun latestUpdatesRequest(page: Int) = GET("$baseUrl/popular?page=$page&latest=1", headers)

    // Hands work to the main thread and waits for it, as extensions do for
    // toasts and web views.
    override fun latestUpdatesParse(response: Response): MangasPage {
        val ran = CountDownLatch(1)
        main.post { if (Looper.myLooper() == Looper.getMainLooper()) ran.countDown() }
        check(ran.await(5, TimeUnit.SECONDS)) { "The main thread did not run." }
        return mangas(response)
    }

    override fun searchMangaRequest(
        page: Int,
        query: String,
        filters: FilterList,
    ): Request {
        val url = "$baseUrl/search".toHttpUrl().newBuilder().addQueryParameter("q", query)
        filters.forEach {
            when (it) {
                is GenreFilter -> url.addQueryParameter("genre", it.values[it.state])
                is AdultFilter -> if (it.state) url.addQueryParameter("adult", "1")
                else -> {}
            }
        }
        return GET(url.build(), headers)
    }

    override fun searchMangaParse(response: Response) = mangas(response)

    override fun mangaDetailsRequest(manga: SManga) = GET(baseUrl + manga.url, headers)

    override fun mangaDetailsParse(response: Response): SManga {
        val o = response.obj()
        return SManga.create().apply {
            title = o["title"]!!.jsonPrimitive.content
            author = o["author"]?.jsonPrimitive?.content
            genre = o["genres"]?.jsonArray?.joinToString(", ") { it.jsonPrimitive.content }
            status = SManga.ONGOING
            // JavaScript, as some extensions run to unpack data.
            description =
                o["description"]!!.jsonPrimitive.content + " " +
                QuickJs.create().use { it.evaluate("['js', 6 * 7].join('-')") as String }
        }
    }

    // Reads the title, as MangaDex's extension does: the host must set it
    // (Mihon always has one), or this throws "lateinit property title has
    // not been initialized".
    override fun chapterListRequest(manga: SManga) = GET("$baseUrl${manga.url}/chapters", headers).also { manga.title.length }

    override fun chapterListParse(response: Response): List<SChapter> =
        response.obj()["chapters"]!!.jsonArray.map {
            val c = it.jsonObject
            SChapter.create().apply {
                url = c["url"]!!.jsonPrimitive.content
                name = c["name"]!!.jsonPrimitive.content
                chapter_number = c["number"]!!.jsonPrimitive.float
                date_upload = dates.parse(c["date"]!!.jsonPrimitive.content)!!.time
            }
        }

    override fun pageListRequest(chapter: SChapter) = GET("$baseUrl${chapter.url}/pages", headers)

    override fun pageListParse(response: Response): List<Page> =
        response.obj()["pages"]!!.jsonArray.mapIndexed { i, p ->
            val o = p.jsonObject
            Page(i, o["page"]?.jsonPrimitive?.content ?: "", o["image"]?.jsonPrimitive?.content?.let { baseUrl + it })
        }

    override fun imageUrlRequest(page: Page) = GET(baseUrl + page.url, headers)

    override fun imageUrlParse(response: Response) = baseUrl + response.obj()["image"]!!.jsonPrimitive.content

    override fun getFilterList() = FilterList(Filter.Header("Filters"), GenreFilter(), AdultFilter())

    class GenreFilter : Filter.Select<String>("Genre", arrayOf("All", "Action", "Drama"))

    class AdultFilter : Filter.CheckBox("Adult")

    override fun setupPreferenceScreen(screen: PreferenceScreen) {
        screen.addPreference(
            ListPreference(screen.context).apply {
                key = "quality"
                title = "Image quality"
                entries = arrayOf("High", "Low")
                entryValues = arrayOf("high", "low")
                setDefaultValue("high")
                summary = "%s"
            },
        )
        // No default, so the stored value starts as null (#21).
        screen.addPreference(
            MultiSelectListPreference(screen.context).apply {
                key = "blocked"
                title = "Blocked genres"
                entries = arrayOf("Action", "Drama")
                entryValues = arrayOf("action", "drama")
            },
        )
    }
}
