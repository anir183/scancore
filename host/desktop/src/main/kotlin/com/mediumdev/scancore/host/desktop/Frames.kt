package com.mediumdev.scancore.host.desktop

import org.objectweb.asm.ClassReader
import org.objectweb.asm.ClassWriter
import java.io.File
import java.net.URLClassLoader
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

// A converted jar's stack map frames worked out again, with the classes
// they name loadable: the jar's own and those of `parent` (the runtime it
// runs on).
object Frames {
    fun recompute(
        from: File,
        to: File,
        parent: ClassLoader,
    ) {
        URLClassLoader(arrayOf(from.toURI().toURL()), parent).use { classes ->
            ZipFile(from).use { zip ->
                ZipOutputStream(to.outputStream().buffered()).use { out ->
                    for (entry in zip.entries()) {
                        val bytes = zip.getInputStream(entry).use { it.readBytes() }
                        out.putNextEntry(ZipEntry(entry.name))
                        out.write(if (entry.name.endsWith(".class")) frames(bytes, classes) else bytes)
                        out.closeEntry()
                    }
                }
            }
        }
    }

    private fun frames(
        bytes: ByteArray,
        classes: ClassLoader,
    ): ByteArray =
        try {
            val writer =
                object : ClassWriter(COMPUTE_FRAMES) {
                    override fun getClassLoader() = classes

                    override fun getCommonSuperClass(
                        type1: String,
                        type2: String,
                    ): String =
                        try {
                            super.getCommonSuperClass(type1, type2)
                        } catch (_: Throwable) {
                            "java/lang/Object"
                        }
                }
            ClassReader(bytes).accept(writer, ClassReader.SKIP_FRAMES)
            writer.toByteArray()
        } catch (_: Throwable) {
            bytes
        }
}
