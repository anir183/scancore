package com.lagradost.cloudstream3

import com.lagradost.cloudstream3.utils.Event

// SCANCORE: CloudStream's main activity, only the events plugins listen to.
// afterPluginsLoadedEvent fires when the host has loaded a plugin.
class MainActivity {
    companion object {
        var lastError: String? = null
        val afterPluginsLoadedEvent = Event<Boolean>()
        val mainPluginsLoadedEvent = Event<Boolean>()
        val afterRepositoryLoadedEvent = Event<Boolean>()
        val bookmarksUpdatedEvent = Event<Boolean>()
        val reloadHomeEvent = Event<Boolean>()
        val reloadLibraryEvent = Event<Boolean>()
        val reloadAccountEvent = Event<Boolean>()
    }
}
