package com.lagradost.cloudstream3.extractors

import com.lagradost.cloudstream3.SubtitleFile
import com.lagradost.cloudstream3.utils.ExtractorApi
import com.lagradost.cloudstream3.utils.ExtractorLink

// SCANCORE: CloudStream reads YouTube with NewPipe Extractor, which
// SCANCORE does not ship (plugins use YouTube for trailers only), so a
// YouTube link gives no videos.
actual open class YoutubeExtractor actual constructor() : ExtractorApi() {
    actual override val mainUrl = "https://www.youtube.com"
    actual override val name = "YouTube"
    actual override val requiresReferer = false

    actual override suspend fun getUrl(
        url: String,
        referer: String?,
        subtitleCallback: (SubtitleFile) -> Unit,
        callback: (ExtractorLink) -> Unit,
    ) = Unit
}
