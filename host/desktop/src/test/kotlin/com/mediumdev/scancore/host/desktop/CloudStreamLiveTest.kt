package com.mediumdev.scancore.host.desktop

import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files

// Real CloudStream plugins against their real sites, by hand only:
//   ./gradlew :desktop:test --tests '*CloudStreamLiveTest*' -Pcloudstream.live=<folder of .cs3 files>
// Prints what each one gives for its main page, a search, the first title's
// episodes and the first episode's videos.
class CloudStreamLiveTest {
    @Test
    fun plugins() {
        val dir = File(System.getProperty("scancore.cloudstream.live", ""))
        assumeTrue("No live plugin folder", dir.isDirectory)
        val root = Files.createTempDirectory("cs-live").toFile()
        val (host, _) = setUp(root, null, "test")
        for (file in dir.listFiles { f -> f.name.endsWith(".cs3") }!!.sortedBy { it.name }) {
            val pkg = file.nameWithoutExtension
            val base = buildJsonObject { put("pkg", pkg); put("apk", file.path) }
            fun call(method: String, extra: JsonObject = JsonObject(emptyMap())) = runBlocking { host.handle(method, JsonObject(base + extra)) }
            try {
                val sources = call("load").jsonArray
                val source = sources[0].jsonObject["id"]!!.jsonPrimitive.content
                val withSource = { extra: Map<String, kotlinx.serialization.json.JsonElement> -> JsonObject(extra + ("source" to JsonPrimitive(source))) }
                println("== $pkg: ${sources[0]}")
                val popular = call("popular", withSource(mapOf("page" to JsonPrimitive(1)))).jsonObject
                val items = popular["items"]!!.jsonArray
                println("   popular: ${items.size} ${items.take(2)}")
                val first = items.firstOrNull()?.jsonObject ?: continue
                val url = first["url"]!!.jsonPrimitive.content
                val details = call("details", withSource(mapOf("url" to JsonPrimitive(url)))).jsonObject
                println("   details: ${details["title"]} ${details["description"].toString().take(80)}")
                val episodes = call("episodes", withSource(mapOf("url" to JsonPrimitive(url)))).jsonArray
                println("   episodes: ${episodes.size} ${episodes.take(2).map { it.jsonObject["name"] }}")
                val episode = episodes.lastOrNull()?.jsonObject ?: continue
                val hosters = call("hosters", withSource(mapOf("url" to episode["url"]!!, "name" to episode["name"]!!))).jsonArray
                println("   hosters: ${hosters.map { h -> h.jsonObject["name"].toString() + " x" + (h.jsonObject["videos"] as? JsonArray)?.size }}")
                (hosters.firstOrNull()?.jsonObject?.get("videos") as? JsonArray)?.firstOrNull()?.let { println("   video: $it".take(400)) }
            } catch (e: Exception) {
                println("   FAILED: ${e.message}")
                e.printStackTrace()
            }
        }
    }
}
