# Third-party code in the extension host

SCANCORE is MIT-licensed. The extension host contains code from other
projects under their own licences, kept with their notices:

| Path | From | Licence |
| --- | --- | --- |
| `compat/` | [Suwayomi-Server](https://github.com/Suwayomi/Suwayomi-Server)'s AndroidCompat (commit e2ce828, 13 Oct 2026): Android API stand-ins for the JVM | Mozilla Public License 2.0 (each file keeps its header) |
| `desktop/.../BytecodeEditor.kt` | Suwayomi-Server | Mozilla Public License 2.0 |
| `compat/.../webkit/KcefWebViewProvider.kt`, `KcefHelper.kt`, `CefHelper.kt` | Suwayomi-Server's JCEF web view (24 Oct 2026) | Mozilla Public License 2.0 |
| `compat/src/main/java/android/view/MotionEvent.java`, `InputEvent.java`, `android/util/DisplayMetrics.java`, `android/content/res/Resources.java` | Rebuilt from AOSP's android.jar stubs, with the parts extensions use filled in | Apache License 2.0 |
| `api/src/main/kotlin/eu/`, `api/src/main/kotlin/mihon/`, `api/src/main/kotlin/tachiyomi/` | [Mihon](https://github.com/mihonapp/mihon)'s source API and network helpers (from Tachiyomi) | Apache License 2.0 |
| `api/src/main/kotlin/eu/kanade/tachiyomi/animesource/`, the request helpers at the end of `network/Requests.kt`, `util/CoroutinesExtensions.kt`, `util/JsonExtensions.kt`, `util/RxExtension.kt` | [Aniyomi](https://github.com/aniyomiorg/aniyomi)'s anime source API (commit 9741444, 14 Sep 2026) and its [extensions-lib](https://github.com/aniyomiorg/extensions-lib) 17 | Apache License 2.0 |

| `cloudstream/library/`, `cloudstream/app/` | [CloudStream](https://github.com/recloudstream/cloudstream)'s library and the app classes its plugins call (commit a7ac454, 22 Oct 2026); see cloudstream/README.md | GNU General Public License 3.0 (cloudstream/library/LICENSE). The whole `cloudstream` module, and the CloudStream runtimes built from it, are distributed under the GPL-3.0; the rest of the host is not linked to it (it is loaded by name, in a class loader of its own) |

SCANCORE's changes to those files are marked "SCANCORE" in comments:
Koin and Typesafe Config replaced by plain singletons, the JCEF web view
set up by the host (CefRuntime) with scripts registered before the page's
own and touches sent as real ones, QuickJS's API on Mozilla Rhino instead of GraalJS, Java 17 instead
of 21, kotlin-logging replaced by SLF4J, its own NetworkHelper, cookie jar
and JavaScriptEngine; Aniyomi's local video server listening on this device only, and its
torrent helpers left out.

The MPL-2.0 text is at https://mozilla.org/MPL/2.0/ and the Apache License
2.0 at https://www.apache.org/licenses/LICENSE-2.0. The source of every
MPL-covered file is this folder.

Libraries the host uses at run time (downloaded by Gradle, shipped in the
Extensions runtime): Kotlin, kotlinx.coroutines, kotlinx.serialization,
OkHttp, Okio, Brotli, Jsoup, RxJava, Injekt, NanoHTTPD (BSD-3-Clause), dex2jar, apk-parser, apksig,
AOSP android.jar stubs, xmlpull, multiplatform-settings, ICU4J,
TwelveMonkeys ImageIO, webp-imageio, sqlite-jdbc, SLF4J, Mozilla Rhino,
JetBrains' JCEF (BSD), CloudStream's runtime (cloudstream/: GPL-3.0, with
NiceHttp (GPL-3.0), Jackson, Ktor HTTP, kotlinx-datetime, kotlinx-io,
atomicfu, cryptography-kotlin and Stately (Apache-2.0) and Ksoup (MIT)),
and a trimmed OpenJDK runtime (GPLv2 with the
Classpath Exception). The web view's native part (Chromium Embedded
Framework, BSD, with Chromium's licences, inside a JetBrains Runtime
release, GPLv2 with the Classpath Exception) is downloaded from JetBrains
the first time a source needs it, not shipped. On
Android, inside the APK: Kotlin, kotlinx.coroutines, kotlinx.serialization,
OkHttp, Okio, Brotli, Jsoup, RxJava, Injekt, NanoHTTPD (BSD-3-Clause), AndroidX Preference and
QuickJS (zhanghai's quickjs-java). Their licences are listed in the app's
licences screen.
