#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// Books in FB2 and MOBI for the novel reader, next to EPUB (LocalNovels):
// each read into chapters of plain HTML that the reader's text blocks come
// from (novels::textBlocks).
//   - FB2 (FictionBook 2, and .fb2.zip): its first body's sections, with
//     emphasis, poems, epigraphs and the like; windows-1251 and 1252 files
//     too.
//   - MOBI (Mobipocket, .mobi .azw .prc) and AZW3 (KF8): PalmDOC or no
//     compression. Books locked to a device (DRM) and Amazon's Huffman
//     compression are refused with a message.
// Written from the formats' descriptions (MobileRead's wiki, FictionBook's
// schema).
namespace sc::local {

struct EbookChapter {
    QString title;
    QByteArray html;
};

struct Ebook {
    QString title;
    QString author;
    QString description;
    QByteArray cover;
    QString coverSuffix; // "jpg", "png", "gif"
    QList<EbookChapter> chapters;
};

bool isEbook(const QString &name);
// The book in `path` (by its name). `metaOnly`: its title, author and
// cover only, for lists. Throws sc::Error with a message for the reader.
Ebook readEbook(const QString &path, bool metaOnly = false);

Ebook readFb2(const QByteArray &data, bool metaOnly = false);
Ebook readMobi(const QByteArray &data, bool metaOnly = false);

} // namespace sc::local
