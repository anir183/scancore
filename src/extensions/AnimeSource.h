#pragma once

#include "Manager.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace sc::ext {

// An anime in a source's listing (or a season of one). `url` is wrapped
// with the source key (see wrapUrl).
struct AnimeItem {
    QString url;
    QString title;
    QString cover;
    // The anime is split into seasons, each an anime of its own.
    bool seasons = false;
    double season = -1;
};

struct AnimeListing {
    QList<AnimeItem> items;
    bool hasNextPage = false;
};

// An Aniyomi extension's anime source, run by the extension host. Like
// ExtensionSource (comics), its links are the extension's own, wrapped with
// the source key.
//
// Host calls (JSON in, JSON out; `source` is the source id as text):
//   popular / latest {source, page}, search {source, page, query, filters}
//       -> {items: [{url, title, cover, fetchType, season?}], hasNextPage}
//   details {source, url, title, fetchType}
//       -> {title, cover, background, description, author, artist, genres,
//           status, fetchType: "episodes" | "seasons"}
//   episodes {source, url, title} -> [{url, name, number, uploaded,
//       scanlator, filler, summary, preview}] (newest first)
//   seasons {source, url, title}  -> [{url, title, cover, season, fetchType}]
//   hosters {source, url, name}   -> [{id, name, lazy, videos?}]
//   hosterVideos {source, hoster} -> [video]
//   resolveVideo {source, video}  -> video, with its final address
//   fetchImage, filters, preferences, setPreference: as for comics.
// A video: {id, url, title, resolution, bitrate, preferred, initialized,
// localServer, headers: {name: value}, subtitles: [{url, lang}], audio:
// [{url, lang}], timestamps: [{start, end, name, type}], mpvArgs: {k: v}}.
// Hoster and video ids live in the host; after it restarts they are
// refused (410) and the episode's hosters are asked again.
// The reader's own video servers (Jellyfin, servers::createAnimeSource)
// answer the same calls themselves instead of the host.
class AnimeSource {
public:
    struct Config {
        QString language;
    };

    explicit AnimeSource(const SourceEntry &entry);
    virtual ~AnimeSource() = default;

    QString key() const { return m_entry.key; }
    QString label() const { return m_entry.name; }
    QString baseUrl() const { return m_entry.baseUrl; }
    bool enabled() const { return m_entry.enabled && m_entry.blocked.isEmpty(); }
    bool nsfw() const { return m_entry.nsfw; }
    // Why it cannot run (untrusted, lib version); empty when it can.
    QString blocked() const { return m_entry.blocked; }
    const Config &config() const { return m_config; }

    QList<AnimeItem> search(const QString &query);
    // "popular", "latest" or "search" (with the query and filter states).
    AnimeListing browse(const QString &listing, int page, const QString &query = {}, const QJsonArray &filters = {});
    QJsonObject details(const QString &url, const QString &title = {}, bool seasons = false);
    // Oldest first, links wrapped; `number` is -1 when the source has none.
    QJsonArray episodes(const QString &url, const QString &title = {});
    QList<AnimeItem> seasons(const QString &url, const QString &title = {});
    QJsonArray hosters(const QString &episodeUrl, const QString &name = {});
    QJsonArray hosterVideos(const QString &hosterId);
    QJsonObject resolveVideo(const QString &videoId);
    QJsonArray filters();
    QJsonArray preferences();
    void setPreference(const QString &key, const QJsonValue &value);

    // Covers go through the extension (ImageCache's "extsource:" writer).
    virtual QString imageReferer() const { return QStringLiteral("extsource:") + key(); }
    void fetchImage(const QString &url, const QString &path);
    // An anime's page on its site.
    virtual QString webUrl(const QString &url) const;
    QString unwrap(const QString &url) const;
    // One of the reader's own servers, not an extension.
    virtual bool isServer() const { return false; }

protected:
    // One host call: method and params as above, the answer as JSON.
    virtual QJsonValue call(const QString &method, QJsonObject params);

private:
    AnimeItem item(const QJsonObject &o) const;

    SourceEntry m_entry;
    Config m_config;
};

} // namespace sc::ext
