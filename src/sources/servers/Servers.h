#pragma once

#include "core/Http.h"
#include "sources/comics/ComicSource.h"
#include "sources/novels/NovelSource.h"

#include <QList>
#include <QString>
#include <memory>

namespace sc::servers {

// The reader's own comic servers (Settings → Sources → Your servers):
// Komga, Kavita, Suwayomi and OPDS catalogues (Opds.h), each a comic
// source keyed "server-<id>" (OPDS also a novel source). Not
// part of the bundled sites: they are the reader's own libraries. Covers
// and pages are plain links on the server with the referer
// "server:<id>", so the image cache asks for them with the server's login
// (registerImages) and no password is ever kept in a link.
struct Server {
    QString id;
    QString kind; // "komga", "kavita", "suwayomi" or "opds"
    QString label;
    // Up to the server's root, no trailing slash; an OPDS catalogue's own
    // feed address.
    QString url;
    QString username;
    QString password;
    // Kavita: an API key from the user's Kavita settings (instead of a
    // username and password).
    QString apiKey;
};

QList<Server> load();
void save(const QList<Server> &servers);
std::optional<Server> byId(const QString &id);

// The headers that log in to `server` (Basic for Komga and Suwayomi, a
// token from logging in for Kavita, kept for its lifetime). Throws on a
// refused login.
Headers authHeaders(const Server &server, bool fresh = false);

std::unique_ptr<comics::Source> createSource(const Server &server);
// OPDS catalogues also hold books (EPUB): the novels' side of the server.
std::unique_ptr<novels::Source> createNovelSource(const Server &server);

// A chapter finished in SCANCORE is marked read on the server too (as
// Mihon's Komga and Kavita trackers do). Blocks (call it in the
// background); failures are only logged. `sourceKey` "server-<id>"; other sources are ignored.
void chapterRead(const QString &sourceKey, const QString &seriesUrl, const QString &chapterUrl, const QString &number);

// Komga and Kavita as trackers for series read from other sources (as
// Mihon's are): the server's series by title ([{id, title, titles,
// detail, total}]), and one moved forward so every chapter up to
// `progress` is read. Blocking; throw Error.
QVariantList trackerSearch(const Server &server, const QString &title);
void trackerPush(const Server &server, const QVariant &id, int progress);

// Lets the image cache fetch "server:<id>" images with their login. Once.
void registerImages();

} // namespace sc::servers
