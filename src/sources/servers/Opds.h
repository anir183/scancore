#pragma once

#include "sources/comics/ComicSource.h"
#include "sources/novels/NovelSource.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <memory>

namespace sc::servers {

struct Server;

// OPDS catalogues (opds.io: 1.x Atom feeds and 2.0 JSON) as the reader's
// own servers: Calibre-web, Kavita, Komga, Ubooquity, Codex and any other.
// A catalogue is a tree of feeds. Its entries are books (with acquisition
// links: a file, or pages streamed with OPDS-PSE) or folders (a link to
// another feed). A folder of books is a series and its books are the
// chapters; a book on its own is a series of one. Comics read CBZ, CBR,
// CB7, PDF and streamed pages; novels read EPUB. Files are fetched once
// into the cache and read as files on this device are.
namespace opds {

struct Link {
    QString rel;
    QString href; // resolved against the feed
    QString type;
    QString title;
    int pseCount = 0; // OPDS-PSE: how many pages the stream link has
};

struct Entry {
    QString id;
    QString title;
    QString author;
    QString summary;
    QString cover;
    QStringList genres;
    QString updated;
    QList<Link> links;

    // The feed it leads to (a folder), or "".
    QString navigation() const;
    // OPDS-PSE: the page template ({pageNumber}) and how many pages.
    const Link *stream() const;
    // The file to fetch for `kinds` ("comic" or "novel"), best first.
    const Link *acquisition(const QString &kind) const;
    bool isBook() const;
};

struct Feed {
    QString title;
    QList<Entry> entries;
    QString next;           // the next page of this feed
    QString searchTemplate; // with {searchTerms}, when the feed names one
    QString searchDescription; // an OpenSearch description to read it from
};

// Parses one feed (Atom or JSON, told apart by the body itself), with
// links resolved against `base`.
Feed parse(const QByteArray &body, const QUrl &base);
// The search URL template from an OpenSearch description document.
QString openSearchTemplate(const QByteArray &body, const QUrl &base);
// `templ` with the query filled in ({searchTerms}, {?query}, {query}).
QString searchUrl(const QString &templ, const QString &query);

// Which reader a file is for, from its type or name: "comic", "novel" or "".
QString kindOf(const QString &type, const QString &href);

std::unique_ptr<comics::Source> createComicSource(const comics::SourceConfig &config, const Server &server);
std::unique_ptr<novels::Source> createNovelSource(const novels::SourceConfig &config, const Server &server);

} // namespace opds
} // namespace sc::servers
