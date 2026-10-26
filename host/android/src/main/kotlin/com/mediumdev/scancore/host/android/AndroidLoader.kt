package com.mediumdev.scancore.host.android

import android.content.Context
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import com.mediumdev.scancore.host.CloudStream
import com.mediumdev.scancore.host.ExtensionLoader
import uy.kohesive.injekt.api.get
import com.mediumdev.scancore.host.HostError
import com.mediumdev.scancore.host.LoadedExtension
import dalvik.system.DelegateLastClassLoader
import eu.kanade.tachiyomi.animesource.AnimeSource
import eu.kanade.tachiyomi.animesource.AnimeSourceFactory
import eu.kanade.tachiyomi.source.Source
import eu.kanade.tachiyomi.source.SourceFactory
import java.io.File
import java.security.MessageDigest

// Loads Mihon and Aniyomi extensions inside the app, as Mihon and Aniyomi
// load their private ones:
// the package manager reads the APK's manifest and checks its signature,
// and the classes load from the read-only file SCANCORE stored, children
// first. No conversion.
class AndroidLoader(
    private val context: Context,
) : ExtensionLoader {
    override val libs = listOf("1.4", "1.5", "1.6")
    override val animeLibs = listOf("14", "16", "17")

    override fun load(
        pkg: String,
        path: String,
        signer: String?,
    ): LoadedExtension {
        val apk = File(path)
        if (!apk.isFile) throw HostError("The extension's file is missing. Install it again.", 404)
        // Android 14 runs code only from files nobody can write to.
        if (apk.canWrite()) apk.setReadOnly()
        // A CloudStream plugin: its dex over CloudStream's runtime (no
        // signature: SCANCORE trusts the repository the reader added).
        if (CloudStream.isPlugin(path)) {
            val runtime = CloudStream.runtime(context) { uy.kohesive.injekt.Injekt.get<eu.kanade.tachiyomi.network.NetworkHelper>().client }
            val classes = dalvik.system.DexClassLoader(apk.path, context.codeCacheDir.path, null, runtime.javaClass.classLoader)
            return LoadedExtension(anime = runtime.load(pkg, apk, classes))
        }
        val flags = PackageManager.GET_META_DATA or PackageManager.GET_SIGNING_CERTIFICATES
        val info: PackageInfo =
            context.packageManager.getPackageArchiveInfo(apk.path, flags)
                ?: throw HostError("The extension's file could not be read.", 422)
        val app = info.applicationInfo ?: throw HostError("The extension's file could not be read.", 422)
        app.sourceDir = apk.path
        app.publicSourceDir = apk.path

        val signing = info.signingInfo ?: throw HostError("The extension is not signed.", 403)
        val certificates = if (signing.hasMultipleSigners()) signing.apkContentsSigners else signing.signingCertificateHistory
        val signers = certificates.map { sha256(it.toByteArray()) }
        if (signers.isEmpty()) throw HostError("The extension is not signed.", 403)
        if (signer != null && signer.lowercase() !in signers) throw HostError("The extension is signed with a key SCANCORE did not trust.", 403)

        val meta = app.metaData ?: throw HostError("The extension does not say which sources it has.", 422)
        // Aniyomi's extensions name their classes under their own key, and
        // their lib as a whole number.
        val anime = meta.containsKey("tachiyomi.animeextension.class")
        val lib =
            if (anime) {
                meta.getInt("aniyomix.extensionLib").takeIf { it != 0 }?.toString()
            } else {
                meta.getFloat("tachiyomix.extensionLib").takeIf { it != 0f }?.toString()
            } ?: info.versionName.orEmpty().substringBeforeLast('.')
        if (lib.toDoubleOrNull() !in (if (anime) animeLibs else libs).map { it.toDouble() }) {
            throw HostError("This extension uses lib $lib, which this SCANCORE does not run.", 426)
        }
        val classes =
            meta
                .getString(if (anime) "tachiyomi.animeextension.class" else "tachiyomi.extension.class")
                ?.split(";")
                ?.map { it.trim() }
                ?.filter { it.isNotEmpty() }
                ?: throw HostError("The extension does not say which sources it has.", 422)
        val loader = DelegateLastClassLoader(apk.path, app.nativeLibraryDir, context.classLoader)
        val instances =
            classes.map { name ->
                val className = if (name.startsWith(".")) pkg + name else name
                Class.forName(className, false, loader).getDeclaredConstructor().newInstance()
            }
        return if (anime) {
            LoadedExtension(
                anime =
                    instances.flatMap {
                        when (it) {
                            is AnimeSource -> listOf(it)
                            is AnimeSourceFactory -> it.createSources()
                            else -> throw HostError("The extension's class ${it.javaClass.name} is not a source.", 422)
                        }
                    },
            )
        } else {
            LoadedExtension(
                sources =
                    instances.flatMap {
                        when (it) {
                            is Source -> listOf(it)
                            is SourceFactory -> it.createSources()
                            else -> throw HostError("The extension's class ${it.javaClass.name} is not a source.", 422)
                        }
                    },
            )
        }
    }

    private fun sha256(bytes: ByteArray) = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
}
