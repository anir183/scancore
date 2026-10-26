#include "Registry.h"

#include "LnCrawl.h"
#include "extensions/Host.h"
#include "extensions/Manager.h"
#include "plugins/PluginSource.h"
#include "core/Text.h"
#include "sources/Manifest.h"
#include "sources/local/LocalSources.h"
#include "sources/servers/Servers.h"

#include <QFile>
#include <QJsonDocument>

#include <QJsonArray>
#include <QReadWriteLock>

namespace sc::novels {

namespace {
QReadWriteLock g_lock;
QList<std::shared_ptr<Source>> g_sources;
} // namespace

void Registry::rebuild() {
    QList<std::shared_ptr<Source>> sources;
    auto wanted = builtinLanguages();
    // None of this medium's built-in sources in those languages: English.
    {
        bool any = wanted.contains(QStringLiteral("*"));
        for (const auto &value : Manifest::current().value(QStringLiteral("novels")).toArray())
            any = any || (value.toObject().value(QStringLiteral("enabled")).toBool(true) && Manifest::supports(value.toObject()) &&
                          wanted.contains(baseLanguage(value.toObject().value(QStringLiteral("language")).toString(QStringLiteral("en")))));
        if (!any)
            wanted.append(QStringLiteral("en"));
    }
    for (const auto &value : Manifest::current().value(QStringLiteral("novels")).toArray()) {
        const auto o = value.toObject();
        if (!Manifest::supports(o))
            continue;
        SourceConfig config;
        config.key = o.value(QStringLiteral("key")).toString();
        config.label = o.value(QStringLiteral("label")).toString(config.key);
        config.engine = o.value(QStringLiteral("engine")).toString();
        config.base = o.value(QStringLiteral("base")).toString();
        for (const auto &host : o.value(QStringLiteral("hosts")).toArray())
            config.hosts.append(host.toString().toLower());
        config.enabled = o.value(QStringLiteral("enabled")).toBool(true);
        config.searchable = o.value(QStringLiteral("searchable")).toBool(true);
        config.disabledReason = o.value(QStringLiteral("disabledReason")).toString();
        config.options = o.value(QStringLiteral("options")).toObject();
        config.language = o.value(QStringLiteral("language")).toString(QStringLiteral("en"));
        config.urlTag = o.value(QStringLiteral("urlTag")).toString();
        if (config.key.isEmpty() || config.base.isEmpty())
            continue;
        // The whole pack switched off (published manifest): series stay in
        // the library and history, and say why they no longer update.
        if (config.enabled && !Manifest::bundledEnabled()) {
            config.enabled = false;
            config.disabledReason = Manifest::bundledReason();
        }
        // Built-in sources in languages the user has not chosen stay off
        // (their series still open from the library and history).
        if (config.enabled && !wanted.contains(QStringLiteral("*")) && !wanted.contains(baseLanguage(config.language))) {
            config.enabled = false;
            config.disabledReason = QStringLiteral("Its language is off in Source languages above");
        }
        if (auto source = createSource(config))
            sources.append(std::shared_ptr<Source>(std::move(source)));
    }
    // The optional lightnovel-crawler pack adds every active adapter whose
    // site is not already handled natively.
    if (LnCrawlRuntime::installed()) {
        QFile file(QStringLiteral(":/lncrawl/sources.json"));
        if (file.open(QIODevice::ReadOnly)) {
            for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
                const auto entry = value.toObject();
                if (!entry.value("disabledReason").toString().isEmpty() || entry.value("manga").toBool())
                    continue;
                SourceConfig config;
                config.key = entry.value("key").toString();
                config.label = entry.value("label").toString();
                config.engine = QStringLiteral("lncrawl");
                for (const auto &origin : entry.value("origins").toArray())
                    config.hosts.append(QUrl(origin.toString()).host().toLower());
                if (config.hosts.isEmpty())
                    continue;
                config.base = entry.value("origins").toArray().first().toString();
                while (config.base.endsWith(QLatin1Char('/')))
                    config.base.chop(1);
                config.searchable = entry.value("search").toBool();
                config.options = QJsonObject{{"extended", true}, {"language", entry.value("language")}};
                config.language = entry.value("language").toString(QStringLiteral("en"));
                const QUrl probe(config.base);
                const bool native = std::any_of(sources.begin(), sources.end(),
                                                [&](const std::shared_ptr<Source> &s) { return s->owns(probe); });
                if (!native)
                    sources.append(std::make_shared<LnCrawlSource>(config, entry));
            }
        }
    }
    // Sources of installed LNReader plugins.
    for (const auto &entry : ext::Manager::sources(ext::Kind::LNReader))
        sources.append(std::make_shared<plugins::PluginSource>(entry));
    // The books of the reader's own OPDS catalogues.
    for (const auto &server : servers::load())
        if (auto source = servers::createNovelSource(server))
            sources.append(std::shared_ptr<Source>(std::move(source)));
    // EPUB books on this device.
    SourceConfig local;
    local.key = QStringLiteral("local");
    local.label = QStringLiteral("On this device");
    local.engine = QStringLiteral("local");
    local.base = QStringLiteral("file://");
    sources.append(std::shared_ptr<Source>(local::createNovelSource(local)));
    QWriteLocker lock(&g_lock);
    g_sources = sources;
}

QList<std::shared_ptr<Source>> Registry::all() {
    QReadLocker lock(&g_lock);
    return g_sources;
}

std::shared_ptr<Source> Registry::byKey(const QString &key) {
    for (const auto &source : all())
        if (source->key() == key)
            return source;
    return nullptr;
}

std::shared_ptr<Source> Registry::forUrl(const QString &url) {
    // Plugin links name their source ("lnreader:<id>:<path>").
    if (const auto wrapped = ext::unwrapUrl(url))
        return byKey(wrapped->key);
    return sourceForUrl(all(), url);
}

} // namespace sc::novels
