package com.mediumdev.scancore.testecho

import dev.brahmkshatriya.echo.common.clients.ExtensionClient
import dev.brahmkshatriya.echo.common.clients.HomeFeedClient
import dev.brahmkshatriya.echo.common.clients.LyricsClient
import dev.brahmkshatriya.echo.common.clients.SearchFeedClient
import dev.brahmkshatriya.echo.common.clients.TrackClient
import dev.brahmkshatriya.echo.common.helpers.PagedData
import dev.brahmkshatriya.echo.common.models.Feed
import dev.brahmkshatriya.echo.common.models.Feed.Companion.toFeed
import dev.brahmkshatriya.echo.common.models.ImageHolder.Companion.toImageHolder
import dev.brahmkshatriya.echo.common.models.Lyrics
import dev.brahmkshatriya.echo.common.models.Shelf
import dev.brahmkshatriya.echo.common.models.Streamable
import dev.brahmkshatriya.echo.common.models.Streamable.Media.Companion.toServerMedia
import dev.brahmkshatriya.echo.common.models.Track
import dev.brahmkshatriya.echo.common.models.Artist
import dev.brahmkshatriya.echo.common.settings.Setting
import dev.brahmkshatriya.echo.common.settings.Settings

// SCANCORE's test Echo extension: songs from the fixture site the host
// tests serve (tests/fixtures/host/echo is not needed: it makes them up),
// a search, a home feed, a stream with a header, and timed lyrics.
class TestEchoExtension : ExtensionClient, SearchFeedClient, HomeFeedClient, TrackClient, LyricsClient {
    private val base = System.getProperty("scancore.testext.base") ?: "http://fixture.test"
    private lateinit var store: Settings

    override suspend fun getSettingItems(): List<Setting> = emptyList()

    override fun setSettings(settings: Settings) {
        store = settings
    }

    override suspend fun onInitialize() {
        store.putInt("started", (store.getInt("started") ?: 0) + 1)
    }

    private fun song(n: Int) =
        Track(
            id = "song-$n",
            title = "Song $n",
            artists = listOf(Artist("artist-1", "The Testers")),
            cover = "$base/img/cover.png".toImageHolder(),
            duration = 180_000L + n,
            streamables = listOf(Streamable.server("low-$n", 128), Streamable.server("high-$n", 320)),
        )

    override suspend fun loadSearchFeed(query: String): Feed<Shelf> =
        listOf<Shelf>(Shelf.Lists.Tracks("found", "Songs", listOf(song(1), song(2))), Shelf.Item(song(3))).toFeed()

    override suspend fun loadHomeFeed(): Feed<Shelf> =
        Feed(listOf()) {
            Feed.Data(
                PagedData.Continuous { continuation ->
                    val page = continuation?.toInt() ?: 1
                    dev.brahmkshatriya.echo.common.helpers.Page(
                        listOf<Shelf>(Shelf.Lists.Tracks("home-$page", "Home", listOf(song(10 + page)))),
                        if (page < 2) (page + 1).toString() else null,
                    )
                },
            )
        }

    override suspend fun loadTrack(
        track: Track,
        isDownload: Boolean,
    ): Track = track

    override suspend fun loadStreamableMedia(
        streamable: Streamable,
        isDownload: Boolean,
    ): Streamable.Media = "$base/audio/${streamable.id}.mp3".toServerMedia(mapOf("X-Test-Echo" to store.getInt("started").toString()))

    override suspend fun loadFeed(track: Track): Feed<Shelf>? = null

    override suspend fun searchTrackLyrics(
        clientId: String,
        track: Track,
    ): Feed<Lyrics> = listOf(Lyrics("l-${track.id}", track.title)).toFeed()

    override suspend fun loadLyrics(lyrics: Lyrics): Lyrics =
        lyrics.copy(lyrics = Lyrics.Timed(listOf(Lyrics.Item("First line", 1_500, 3_000), Lyrics.Item("Second line", 62_250, 64_000))))
}
