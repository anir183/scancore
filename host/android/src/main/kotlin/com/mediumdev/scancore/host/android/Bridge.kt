package com.mediumdev.scancore.host.android

import android.app.Application
import android.content.Context
import android.webkit.CookieManager
import com.mediumdev.scancore.host.CloudStream
import com.mediumdev.scancore.host.ExtensionHost
import com.mediumdev.scancore.host.HostError
import com.mediumdev.scancore.host.MemoStore
import com.mediumdev.scancore.host.RpcServer
import com.mediumdev.scancore.host.string
import eu.kanade.tachiyomi.AppInfo
import eu.kanade.tachiyomi.network.HostNetwork
import eu.kanade.tachiyomi.network.JavaScriptEngine
import eu.kanade.tachiyomi.network.NetworkHelper
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.Json
import kotlinx.serialization.protobuf.ProtoBuf
import okhttp3.Cookie
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import uy.kohesive.injekt.Injekt
import uy.kohesive.injekt.api.addSingleton
import uy.kohesive.injekt.api.addSingletonFactory
import java.io.File

// The extension host inside the Android app, spoken to by SCANCORE's C++
// over JNI with the same protocol as the desktop helper: send() takes a
// request line, and the answer comes back through the native reply().
object Bridge {
    private var server: RpcServer? = null

    @JvmStatic
    @Synchronized
    fun start(
        context: Context,
        root: String,
        userAgent: String,
        appVersion: String,
    ) {
        if (server != null) return
        val app = context.applicationContext as Application
        val folder = File(root).apply { mkdirs() }
        HostNetwork.userAgent = webViewUserAgent(app) ?: userAgent
        HostNetwork.cookieJar = WebViewCookieJar(File(folder, "cookies.json"))
        HostNetwork.browserCheckSolver = WebViewBrowserCheck(app)
        HostNetwork.cacheDir = File(folder, "cache")
        AppInfo.set(versionCode = 1, versionName = appVersion)
        Injekt.addSingleton<Application>(app)
        Injekt.addSingleton<Context>(app)
        Injekt.addSingletonFactory { NetworkHelper(app) }
        Injekt.addSingletonFactory { JavaScriptEngine(app) }
        Injekt.addSingletonFactory {
            Json {
                ignoreUnknownKeys = true
                explicitNulls = false
            }
        }
        Injekt.addSingletonFactory<ProtoBuf> { ProtoBuf }
        // CloudStream's runtime, which SCANCORE downloads beside the host's
        // folder (extensions/cloudstream-runtime) with the first CloudStream
        // repository; its classes load once it is there.
        val cloudStream = File(folder.parentFile, "cloudstream-runtime/cloudstream-runtime-$CLOUDSTREAM_RUNTIME.jar")
        var cloudStreamClasses: ClassLoader? = null
        CloudStream.runtimeClasses = {
            synchronized(this) {
                cloudStreamClasses ?: cloudStream.takeIf { it.isFile }?.let {
                    if (it.canWrite()) it.setReadOnly()
                    dalvik.system.DexClassLoader(it.path, app.codeCacheDir.path, null, Bridge::class.java.classLoader).also { loader -> cloudStreamClasses = loader }
                }
            }
        }
        val host = ExtensionHost(AndroidLoader(app), MemoStore(File(folder, "memos.json")), HOST_VERSION, AndroidSettings(app))
        server = RpcServer({ method, params -> handle(host, method, params) }).also {
            HostNetwork.readerBrowserCheck = com.mediumdev.scancore.host.ReaderBrowserCheck(it)
        }
    }

    // One request line; answered later through reply().
    @JvmStatic
    fun send(line: String) {
        val running = server ?: return reply("""{"error":{"message":"The extension host is not started.","status":503}}""")
        running.submit(line) { reply(it) }
    }

    @JvmStatic
    external fun reply(line: String)

    // The user agent extensions send, for SCANCORE's browser check window.
    @JvmStatic
    fun userAgent(): String = HostNetwork.userAgent

    // Chrome's own (reduced) user agent at the phone's web view version, as
    // Mihon sends: the hidden check and the check window are that web view,
    // and Cloudflare only lets a browser through without a click when the
    // user agent matches what it shows of itself (Android, its version).
    private fun webViewUserAgent(context: Context): String? =
        runCatching {
            val major = Regex("""Chrome/(\d+)""").find(android.webkit.WebSettings.getDefaultUserAgent(context))?.groupValues?.get(1)
            major?.let { "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/$it.0.0.0 Mobile Safari/537.36" }
        }.getOrNull()

    // On a phone the browser check runs in a web view; its cookies reach
    // extensions from Android's own cookie store.
    private suspend fun handle(
        host: ExtensionHost,
        method: String,
        params: JsonObject,
    ): JsonElement {
        if (method != "importWebCookies") return host.handle(method, params)
        val url = params.string("url").toHttpUrlOrNull() ?: throw HostError("That is not a link.", 400)
        val header = CookieManager.getInstance().getCookie(url.toString()).orEmpty()
        val cookies = header.split(';').mapNotNull { Cookie.parse(url, it.trim()) }
        HostNetwork.cookieJar.saveFromResponse(url, cookies)
        return JsonPrimitive(cookies.size)
    }

    const val HOST_VERSION = "1"

    // The CloudStream runtime this host loads: the desktop host's version
    // (ext::Runtime::HostVersion), whose release has it.
    const val CLOUDSTREAM_RUNTIME = "8"
}
