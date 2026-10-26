#include "Registry.h"

#include "extensions/Host.h"
#include "sources/servers/Servers.h"

#include <QReadWriteLock>

namespace sc::anime {

namespace {
QReadWriteLock g_lock;
QList<std::shared_ptr<ext::AnimeSource>> g_sources;
} // namespace

void Registry::rebuild() {
    QList<std::shared_ptr<ext::AnimeSource>> sources;
    for (const auto &entry : ext::Manager::sources(ext::Kind::Aniyomi))
        sources.append(std::make_shared<ext::AnimeSource>(entry));
    // The reader's own Jellyfin servers.
    servers::registerImages();
    for (const auto &server : servers::load())
        if (auto source = servers::createAnimeSource(server))
            sources.append(std::shared_ptr<ext::AnimeSource>(std::move(source)));
    QWriteLocker lock(&g_lock);
    g_sources = sources;
}

QList<std::shared_ptr<ext::AnimeSource>> Registry::all() {
    QReadLocker lock(&g_lock);
    return g_sources;
}

std::shared_ptr<ext::AnimeSource> Registry::byKey(const QString &key) {
    for (const auto &source : all())
        if (source->key() == key)
            return source;
    return nullptr;
}

std::shared_ptr<ext::AnimeSource> Registry::forUrl(const QString &url) {
    const auto wrapped = ext::unwrapUrl(url);
    return wrapped ? byKey(wrapped->key) : nullptr;
}

} // namespace sc::anime
