#include "ExtensionSource.h"

#include "Host.h"
#include "LineHost.h"
#include "Manager.h"
#include "core/Cookies.h"
#include "core/ImageCache.h"
#include "sources/anime/Registry.h"
#include "sources/comics/Registry.h"

#include <QDateTime>
#include <QUrl>

#include <algorithm>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QTimeZone>

namespace sc::ext {

namespace {

comics::SourceConfig configFor(const SourceEntry &entry) {
    comics::SourceConfig config;
    config.key = entry.key;
    config.label = entry.name;
    config.engine = kindName(entry.kind);
    config.base = entry.baseUrl;
    config.enabled = entry.enabled && entry.blocked.isEmpty();
    config.disabledReason = entry.blocked;
    config.nsfw = entry.nsfw;
    config.language = entry.lang;
    return config;
}

QString text(const QJsonValue &value) {
    if (value.isDouble()) {
        const double d = value.toDouble();
        return d == double(qint64(d)) ? QString::number(qint64(d)) : QString::number(d);
    }
    return value.toString();
}

} // namespace

ExtensionSource::ExtensionSource(const SourceEntry &entry) : Source(configFor(entry)), m_entry(entry) {}

QString ExtensionSource::unwrap(const QString &url) const {
    const auto wrapped = unwrapUrl(url);
    if (!wrapped || wrapped->key != key())
        throw Error(QStringLiteral("That link does not belong to %1.").arg(label()));
    return wrapped->url;
}

QJsonValue ExtensionSource::call(const QString &method, QJsonObject params) {
    if (!m_entry.blocked.isEmpty())
        throw Error(m_entry.blocked);
    params.insert(QStringLiteral("source"), m_entry.id);
    params.insert(QStringLiteral("pkg"), m_entry.pkg);
    params.insert(QStringLiteral("apk"), m_entry.path);
    if (!m_entry.signer.isEmpty())
        params.insert(QStringLiteral("signer"), m_entry.signer);
    try {
        const auto result = requireHost(m_entry.kind)->call(method, params);
        Manager::recordSourceResult(key(), {});
        return result;
    } catch (const Error &e) {
        if (e.status() != 499)
            Manager::recordSourceResult(key(), e.message());
        throw;
    }
}

comics::Listing ExtensionSource::listing(const QJsonValue &result) const {
    comics::Listing out;
    const auto o = result.toObject();
    for (const auto &value : o.value("items").toArray()) {
        const auto item = value.toObject();
        comics::Summary s;
        s.title = item.value("title").toString().trimmed();
        s.url = wrapUrl(key(), item.value("url").toString());
        s.cover = item.value("cover").toString();
        s.source = key();
        if (!s.title.isEmpty() && !item.value("url").toString().isEmpty())
            out.items.append(s);
    }
    out.hasNextPage = o.value("hasNextPage").toBool();
    return out;
}

QList<comics::Summary> ExtensionSource::search(const QString &query) {
    return browse(QStringLiteral("search"), 1, query, {}).items;
}

comics::Listing ExtensionSource::browse(const QString &kind, int page, const QString &query, const QJsonArray &filters) {
    QJsonObject params{{"page", std::max(1, page)}};
    if (kind == QLatin1String("search")) {
        params.insert("query", query);
        params.insert("filters", filters);
    }
    const auto method = kind == QLatin1String("latest") ? QStringLiteral("latest")
                        : kind == QLatin1String("popular") ? QStringLiteral("popular")
                                                            : QStringLiteral("search");
    return listing(call(method, params));
}

QList<comics::Chapter> ExtensionSource::chapters(const QString &seriesUrl) {
    const auto result = call(QStringLiteral("chapters"), {{"url", unwrap(seriesUrl)}}).toArray();
    QList<comics::Chapter> out;
    out.reserve(result.size());
    // Extensions list the newest first; SCANCORE keeps the oldest first.
    for (auto it = result.end(); it != result.begin();) {
        const auto c = (--it)->toObject();
        comics::Chapter ch;
        const double number = c.value("number").toDouble(-1);
        ch.number = number >= 0 ? text(number) : comics::chapterNumberFrom(c.value("name").toString());
        ch.title = c.value("name").toString().trimmed();
        if (ch.title.isEmpty())
            ch.title = chapterLabel(ch.number);
        ch.url = wrapUrl(key(), c.value("url").toString());
        ch.slug = c.value("url").toString();
        ch.scanlator = c.value("scanlator").toString().trimmed();
        if (const auto uploaded = qint64(c.value("uploaded").toDouble()); uploaded > 0)
            ch.released = QDateTime::fromMSecsSinceEpoch(uploaded, QTimeZone::UTC).toString(Qt::ISODate);
        out.append(ch);
    }
    return out;
}

QList<comics::Panel> ExtensionSource::parsePages(const QString &key, const QJsonArray &pages) {
    QList<comics::Panel> out;
    out.reserve(pages.size());
    for (const auto &value : pages) {
        const auto page = value.toObject();
        // The page as the host needs it back, in a stable form so the image
        // cache finds it again.
        const QJsonObject keep{{"index", page.value("index")}, {"url", page.value("url")}, {"imageUrl", page.value("imageUrl")}};
        const auto encoded = QJsonDocument(keep).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        out.append({QStringLiteral("extimage:%1:%2").arg(key, QString::fromLatin1(encoded))});
    }
    return out;
}

QList<comics::Panel> ExtensionSource::panels(const QString &chapterUrl) {
    return parsePages(key(), call(QStringLiteral("pages"), {{"url", unwrap(chapterUrl)}}).toArray());
}

void ExtensionSource::fetchImage(const QJsonObject &page, const QString &path) {
    call(QStringLiteral("fetchImage"), {{"page", page}, {"target", path}});
}

void ExtensionSource::installImageWriter() {
    ImageCache::setSchemeWriter(QStringLiteral("extimage"), [](const QString &url, const QString &path) {
        // extimage:<kind>:<id>:<page>
        const auto rest = url.mid(int(qstrlen("extimage:")));
        const auto cut = rest.lastIndexOf(QLatin1Char(':'));
        const auto key = rest.left(cut);
        const auto page = QJsonDocument::fromJson(QByteArray::fromBase64(rest.mid(cut + 1).toLatin1(), QByteArray::Base64UrlEncoding)).object();
        auto source = std::dynamic_pointer_cast<ExtensionSource>(comics::Registry::byKey(key));
        if (!source)
            throw Error(QStringLiteral("The extension for this page is not installed."), 404);
        source->fetchImage(page, path);
    });
    // A cover (any plain image link) through the source's own client, for
    // comic and anime sources alike.
    ImageCache::setRefererWriter(QStringLiteral("extsource"), [](const QString &url, const QString &referer, const QString &path) {
        const auto key = referer.mid(int(qstrlen("extsource:")));
        if (auto show = anime::Registry::byKey(key))
            return show->fetchImage(url, path);
        auto source = std::dynamic_pointer_cast<ExtensionSource>(comics::Registry::byKey(key));
        if (!source)
            throw Error(QStringLiteral("The extension for this image is not installed."), 404);
        source->fetchImage(QJsonObject{{"index", 0}, {"url", QString()}, {"imageUrl", url}}, path);
    });
}

void ExtensionSource::shareCookies(const QString &url) {
    const QUrl site(url);
    // Never the account's own site (its cookies are the session).
    if (CookieJar::isProtected(site))
        return;
    // Comic and anime extensions share the host and its cookies. Every
    // site's, not only their listed one: an extension may read a mirror the
    // reader picked, or an API or image server on another domain.
    auto h = host(Kind::Mihon);
    if (!h || !h->available())
        return;
    QJsonArray cookies;
    for (const auto &cookie : CookieJar::shared().cookiesForUrl(site))
        cookies.append(QString::fromUtf8(cookie.toRawForm()));
    if (!cookies.isEmpty())
        h->call(QStringLiteral("setCookies"), {{"url", site.toString()}, {"cookies", cookies}});
}

QString ExtensionSource::checkAgent(const QString &url) {
    if (const auto agent = browserCheckAgent(url); !agent.isEmpty())
        return agent;
    const auto host = QUrl(url).host();
    auto entries = Manager::sources(Kind::Mihon);
    entries += Manager::sources(Kind::Aniyomi);
    entries += Manager::sources(Kind::CloudStream);
    const bool extensionSite = std::any_of(entries.begin(), entries.end(), [&](const SourceEntry &e) {
        const auto base = QUrl(e.baseUrl).host();
        return !base.isEmpty() && (hostMatches(host, base) || hostMatches(base, host));
    });
    return extensionSite ? extensionAgent() : QString::fromUtf8(Http::userAgent());
}

QString ExtensionSource::statusName(int status) {
    switch (status) {
    case 1: return QStringLiteral("Ongoing");
    case 2: return QStringLiteral("Completed");
    case 3: return QStringLiteral("Licensed");
    case 4: return QStringLiteral("Publishing finished");
    case 5: return QStringLiteral("Cancelled");
    case 6: return QStringLiteral("Hiatus");
    }
    return {};
}

std::optional<comics::Details> ExtensionSource::details(const QString &seriesUrl) {
    const auto o = call(QStringLiteral("details"), {{"url", unwrap(seriesUrl)}}).toObject();
    comics::Details d;
    d.title = o.value("title").toString();
    d.cover = o.value("cover").toString();
    d.description = o.value("description").toString();
    QStringList people{o.value("author").toString(), o.value("artist").toString()};
    people.removeAll(QString());
    people.removeDuplicates();
    d.author = people.join(QStringLiteral(", "));
    d.status = statusName(o.value("status").toInt());
    for (const auto &g : o.value("genres").toArray())
        if (!g.toString().trimmed().isEmpty())
            d.genres.append(g.toString().trimmed());
    return d;
}

// Covers go through the extension too, with the headers its site wants
// (ImageCache's referer writer below).
QString ExtensionSource::imageReferer() const { return QStringLiteral("extsource:") + key(); }

QString ExtensionSource::webUrl(const QString &url) const {
    const auto path = unwrap(url);
    if (path.startsWith(QLatin1String("http")))
        return path;
    const auto base = m_config.base.endsWith(QLatin1Char('/')) ? m_config.base : m_config.base + QLatin1Char('/');
    return QUrl(base).resolved(QUrl(path)).toString();
}

QJsonArray ExtensionSource::filters() { return call(QStringLiteral("filters"), {}).toArray(); }

QJsonArray ExtensionSource::preferences() { return call(QStringLiteral("preferences"), {}).toArray(); }

void ExtensionSource::setPreference(const QString &prefKey, const QJsonValue &value) {
    call(QStringLiteral("setPreference"), {{"key", prefKey}, {"value", value}});
    // Kept by SCANCORE too: it syncs, and the host gets it back on start.
    Manager::setPreference(key(), prefKey, QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)));
}

} // namespace sc::ext
