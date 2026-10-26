# CloudStream plugins in the extension host

SCANCORE runs CloudStream plugins (`.cs3` files from CloudStream
repositories) as anime sources. This module is everything that takes:

| Path | What | Licence |
| --- | --- | --- |
| `library/` | CloudStream's library (`library/src` of [recloudstream/cloudstream](https://github.com/recloudstream/cloudstream) at commit a7ac454af8fb, 22 Oct 2026): `commonMain`, `jvmCommonMain` (without the NewPipe YouTube extractor) and `androidMain`, unchanged. Plugins are compiled against it. | GPL-3.0 (`library/LICENSE`) |
| `app/` | The few classes of CloudStream's app that plugins call (`CloudStreamApp`, `CommonActivity`, `MainActivity`'s events, `Plugin`, `DataStore`, `CloudflareKiller`, `RequestsHelper`), copied from its `app/src/main/java` and trimmed; each file says what changed. | GPL-3.0 |
| `src/commonMain` | `BuildConfig`, which their build generates. | SCANCORE's, distributed under GPL-3.0 with the above |
| `src/jvmMain/.../extractors/YoutubeExtractor.kt` | The YouTube extractor without NewPipe (no videos). | SCANCORE's, distributed under GPL-3.0 with the above |
| `src/jvmMain/.../scancore/host/cloudstream` | The adapter: `Runtime` loads a plugin as CloudStream's PluginManager does; `CloudStreamSource` shows its providers as an Aniyomi anime source. | SCANCORE's, distributed under GPL-3.0 with the above |

The host itself (core, api, compat, desktop, android) does not depend on
this module: core's `CloudStream.kt` loads it by name from its own class
loader. On desktop its jars are `lib/cloudstream` in the Extensions runtime;
on Android they are one dex (`./gradlew :cloudstream:androidRuntime`)
published as `SCANCORE-cloudstream-runtime-<host version>-android.jar` in
the same `extensions-runtime-<host version>` release, which the app
downloads when the first CloudStream plugin is installed. The SCANCORE app
stays MIT; this module, and the runtimes that contain it, are GPL-3.0.

To update the library: copy `library/src/{commonMain,jvmCommonMain,androidMain}`
from a newer CloudStream commit over `library/`, delete
`jvmCommonMain/.../extractors/YoutubeExtractor.jvmCommon.kt` and
`androidMain/AndroidManifest.xml`, match the versions in
`gradle/libs.versions.toml` to theirs, and run `./gradlew :desktop:test`.
