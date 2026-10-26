package com.mediumdev.scancore.host.cloudstream

import android.content.Context
import com.lagradost.cloudstream3.APIHolder
import com.lagradost.cloudstream3.CloudStreamApp
import com.lagradost.cloudstream3.MainAPI
import com.lagradost.cloudstream3.MainActivity
import com.lagradost.cloudstream3.app
import com.lagradost.cloudstream3.network.hostClient
import com.lagradost.cloudstream3.plugins.BasePlugin
import com.lagradost.cloudstream3.plugins.Plugin
import com.lagradost.cloudstream3.utils.AppUtils.parseJson
import com.mediumdev.scancore.host.CloudStream
import com.mediumdev.scancore.host.CloudStreamRuntime
import com.mediumdev.scancore.host.HostError
import eu.kanade.tachiyomi.animesource.AnimeSource
import okhttp3.OkHttpClient
import java.io.File

// The host's way into CloudStream (core's CloudStream.kt makes one by name):
// loads plugins as CloudStream's PluginManager does (manifest.json names
// the plugin class; load() registers its providers) and gives back each
// plugin's providers as one anime source.
class Runtime : CloudStreamRuntime {
    private lateinit var context: Context
    private lateinit var client: () -> OkHttpClient

    // What each plugin registered, so a newer version replaces it.
    private val registered = HashMap<String, List<MainAPI>>()

    override fun start(
        context: Any,
        client: () -> OkHttpClient,
    ) {
        this.context = context as Context
        this.client = client
        CloudStreamApp.context = this.context
        hostClient = client
        // Plugins' requests go through the host's client: SCANCORE's cookies,
        // DNS and browser check, CloudStream's default headers.
        app.baseClient = client()
    }

    @Synchronized
    override fun load(
        pkg: String,
        file: File,
        classes: ClassLoader,
    ): List<AnimeSource> {
        val manifest =
            classes.getResourceAsStream("manifest.json")?.use { parseJson<BasePlugin.Manifest>(it.reader().readText()) }
                ?: throw HostError("The plugin has no manifest.json.", 422)
        val className = manifest.pluginClassName ?: throw HostError("The plugin does not say which class it is.", 422)
        val plugin =
            try {
                Class.forName(className, true, classes).getDeclaredConstructor().newInstance() as BasePlugin
            } catch (e: ClassCastException) {
                throw HostError("$className is not a CloudStream plugin.", 422)
            }
        plugin.filename = file.absolutePath

        // A plugin registers into CloudStream's lists: what it adds is its.
        registered.remove(pkg)?.let { old ->
            synchronized(APIHolder.allProviders) { APIHolder.allProviders.removeAll { p -> old.any { it === p } } }
        }
        val before = synchronized(APIHolder.allProviders) { APIHolder.allProviders.toList() }
        try {
            if (plugin is Plugin) plugin.load(context) else plugin.load()
        } catch (e: Throwable) {
            e.printStackTrace()
            throw HostError("${manifest.name ?: pkg} did not start: ${e.message ?: e.javaClass.simpleName}", 500)
        }
        val added = synchronized(APIHolder.allProviders) { APIHolder.allProviders.filter { p -> before.none { it === p } } }
        if (added.isEmpty()) throw HostError("${manifest.name ?: pkg} has no sites SCANCORE can show.", 422)
        registered[pkg] = added
        MainActivity.afterPluginsLoadedEvent.invoke(false)
        return listOf(CloudStreamSource(CloudStream.sourceId(pkg), manifest.name ?: pkg, added, client))
    }
}
