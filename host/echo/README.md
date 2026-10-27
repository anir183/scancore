# Echo extensions in the extension host

SCANCORE runs [Echo](https://github.com/brahmkshatriya/echo)'s music
extensions (`.eapk` files: APKs built against Echo's extension library) as
music sources. This module is what that takes:

| Path | What | Licence |
| --- | --- | --- |
| `dev.brahmkshatriya.echo:common` 1.0.0 | Echo's extension library, from Maven Central (not copied here). Extensions are compiled against it. | Unabandon Public License (GPL-3.0 plus public source) |
| `src/main/.../scancore/host/echo` | The adapter: `Runtime` loads an extension as Echo's app does (its manifest's meta-data names its class; parent-first class loading) and gives it settings, its metadata and the network state; `EchoMusicSource` shows a music extension (search, home feed, tracks, lyrics) in the terms of SCANCORE's music plugins (docs/music-plugins.md). | SCANCORE's, distributed under the Unabandon Public License with the above |

Only music extensions with search and tracks play (`SearchFeedClient` and
`TrackClient`); lyrics come from the same extension when it has them.
Streams SCANCORE's player cannot open (protected, or raw bytes) are not
offered. Lyrics-only, tracker and other extension kinds, sign-in and
library features are not used yet.

The host itself (core, api, compat, desktop, android) does not depend on
this module: core's `Echo.kt` loads it by name from its own class loader.
On desktop its jars are `lib/echo` in the Extensions runtime, and
extensions are converted with dex2jar like Mihon's (Echo's interfaces are
looked up in the runtime while fixing calls to their default methods); on
Android they are one dex (`./gradlew :echo:androidRuntime`) published as
`SCANCORE-echo-runtime-<host version>-android.jar` in the same
`extensions-runtime-<host version>` release, which the app downloads when
the first Echo extension is installed.

Repositories are Echo's extension lists (`[{id, name, subtitle, iconUrl,
updateUrl}]`) or their short codes (v.gd); an extension's file and version
are its latest GitHub release, as Echo's app reads them. Each extension is
signed by its author: SCANCORE keeps the key of the first install and
refuses updates signed with another.

The SCANCORE app stays MIT; this module, and the runtimes that contain it,
are under Echo's licence.

Test: `./gradlew :desktop:test --tests '*echoExtension'` (the test
extension in `testext/src/echo`).
