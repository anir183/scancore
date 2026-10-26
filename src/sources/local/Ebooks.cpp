#include "Ebooks.h"

#include "Local.h"
#include "core/Error.h"
#include "core/Html.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QXmlStreamReader>

namespace sc::local {

namespace {

// ------------------------------------------------------------------ text

// The upper halves of windows-1251 (Cyrillic, most Russian FB2 books) and
// windows-1252 (0x80-0x9F; the rest is Latin-1).
constexpr char16_t kCp1251[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F};
constexpr char16_t kCp1252[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                  0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                  0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

QString decodeCodePage(const QByteArray &bytes, int page) {
    QString out;
    out.reserve(bytes.size());
    for (const char c : bytes) {
        const auto b = uchar(c);
        if (b < 0x80)
            out.append(QChar(b));
        else if (page == 1251)
            out.append(QChar(kCp1251[b - 0x80]));
        else if (b < 0xA0)
            out.append(QChar(kCp1252[b - 0x80]));
        else
            out.append(QChar(b));
    }
    return out;
}

QString esc(const QString &s) { return s.toHtmlEscaped(); }

// ------------------------------------------------------------------ FB2

// FictionBook's XML as Unicode: Qt's reader takes UTF-8 and UTF-16 only, so
// books in windows-1251 or 1252 are turned to UTF-8 first.
QByteArray fb2Utf8(const QByteArray &data) {
    static const QRegularExpression declaration(QStringLiteral(R"(^\s*<\?xml[^>]*encoding\s*=\s*["']([^"']+)["'][^>]*\?>)"),
                                                QRegularExpression::CaseInsensitiveOption);
    const auto head = QString::fromLatin1(data.left(200));
    const auto m = declaration.match(head);
    if (!m.hasMatch())
        return data;
    const auto encoding = m.captured(1).toLower().remove(QLatin1Char('-')).remove(QLatin1Char('_'));
    int page = 0;
    if (encoding == QLatin1String("windows1251") || encoding == QLatin1String("cp1251"))
        page = 1251;
    else if (encoding == QLatin1String("windows1252") || encoding == QLatin1String("cp1252") || encoding == QLatin1String("iso88591") ||
             encoding == QLatin1String("latin1"))
        page = 1252;
    if (!page)
        return data;
    const auto rest = data.mid(m.capturedEnd());
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>" + decodeCodePage(rest, page).toUtf8();
}

QString hrefOf(const QXmlStreamAttributes &attributes) {
    for (const auto &a : attributes)
        if (a.name() == QLatin1String("href"))
            return a.value().toString();
    return {};
}

// The base64 of <binary id="…"> without reading the whole book.
QByteArray fb2Binary(const QByteArray &data, const QString &id, QString *type) {
    if (id.isEmpty())
        return {};
    const auto needle = QByteArray("id=\"") + id.toUtf8() + '"';
    qsizetype at = data.indexOf(needle);
    if (at < 0)
        at = data.indexOf(QByteArray("id='") + id.toUtf8() + '\'');
    if (at < 0)
        return {};
    const auto open = data.lastIndexOf("<", at);
    const auto start = data.indexOf('>', at);
    const auto end = data.indexOf("</", start);
    if (open < 0 || start < 0 || end < 0)
        return {};
    const auto tag = data.mid(open, start - open);
    static const QRegularExpression contentType(QStringLiteral(R"(content-type\s*=\s*["']([^"']+)["'])"));
    if (type)
        *type = contentType.match(QString::fromLatin1(tag)).captured(1);
    return QByteArray::fromBase64(data.mid(start + 1, end - start - 1).simplified().replace(' ', ""));
}

QString suffixFor(const QByteArray &image, const QString &type = {}) {
    if (image.startsWith("\x89PNG") || type.contains(QLatin1String("png")))
        return QStringLiteral("png");
    if (image.startsWith("GIF8") || type.contains(QLatin1String("gif")))
        return QStringLiteral("gif");
    return QStringLiteral("jpg");
}

} // namespace

Ebook readFb2(const QByteArray &raw, bool metaOnly) {
    const auto data = fb2Utf8(raw);
    Ebook book;
    QXmlStreamReader xml(data);
    QString coverId;
    QStringList author;
    bool inTitleInfo = false, inAuthor = false, inAnnotation = false, authorDone = false;
    // The body: chapters, one per section that has text of its own.
    bool inBody = false, bodySeen = false;
    int sections = 0, titleDepth = 0;
    EbookChapter chapter;
    bool chapterHasText = false;
    QString titleText;
    QStringList open; // the HTML elements open in the chapter
    QString notesTitle;
    const auto flush = [&] {
        if (chapterHasText)
            book.chapters.append(chapter);
        chapter = EbookChapter();
        chapterHasText = false;
    };
    const auto put = [&](const QString &html) { chapter.html += html.toUtf8(); };
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name();
            if (name == QLatin1String("title-info")) {
                inTitleInfo = true;
            } else if (inTitleInfo && name == QLatin1String("book-title")) {
                book.title = xml.readElementText().simplified();
            } else if (inTitleInfo && name == QLatin1String("author")) {
                inAuthor = !authorDone;
            } else if (inAuthor && (name == QLatin1String("first-name") || name == QLatin1String("middle-name") || name == QLatin1String("last-name"))) {
                if (const auto part = xml.readElementText().simplified(); !part.isEmpty())
                    author.append(part);
            } else if (inAuthor && name == QLatin1String("nickname") && author.isEmpty()) {
                author.append(xml.readElementText().simplified());
            } else if (inTitleInfo && name == QLatin1String("annotation")) {
                inAnnotation = true;
            } else if (inAnnotation && name == QLatin1String("p")) {
                const auto p = xml.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
                book.description += (book.description.isEmpty() ? QString() : QStringLiteral("\n\n")) + p;
            } else if (inTitleInfo && name == QLatin1String("image") && coverId.isEmpty()) {
                coverId = hrefOf(xml.attributes()).remove(QLatin1Char('#'));
            } else if (name == QLatin1String("body")) {
                if (metaOnly)
                    break;
                const auto bodyName = xml.attributes().value(QStringLiteral("name")).toString();
                inBody = true;
                // Footnotes and comments: a chapter of their own, at the end.
                if (bodySeen) {
                    flush();
                    notesTitle = bodyName == QLatin1String("notes") ? QStringLiteral("Notes") : bodyName;
                    chapter.title = notesTitle;
                }
                bodySeen = true;
            } else if (inBody) {
                if (name == QLatin1String("section")) {
                    ++sections;
                    // A section with text of its own before this one: that
                    // text is a chapter.
                    if (chapterHasText)
                        flush();
                } else if (name == QLatin1String("title")) {
                    ++titleDepth;
                    titleText.clear();
                    put(QStringLiteral("<h2>"));
                    open.append(QStringLiteral("h2"));
                } else if (name == QLatin1String("p") || name == QLatin1String("v") || name == QLatin1String("text-author")) {
                    if (titleDepth) {
                        if (!titleText.isEmpty()) put(QStringLiteral(" "));
                    } else {
                        put(QStringLiteral("<p>"));
                        open.append(QStringLiteral("p"));
                    }
                    if (name == QLatin1String("text-author")) {
                        put(QStringLiteral("<i>"));
                        open.append(QStringLiteral("i"));
                    }
                } else if (name == QLatin1String("subtitle")) {
                    put(QStringLiteral("<h3>"));
                    open.append(QStringLiteral("h3"));
                } else if (name == QLatin1String("epigraph") || name == QLatin1String("cite")) {
                    put(QStringLiteral("<blockquote>"));
                    open.append(QStringLiteral("blockquote"));
                } else if (name == QLatin1String("stanza")) {
                    put(QStringLiteral("<div>"));
                    open.append(QStringLiteral("div"));
                } else if (name == QLatin1String("empty-line")) {
                    put(QStringLiteral("<br>"));
                } else if (name == QLatin1String("emphasis")) {
                    put(QStringLiteral("<i>"));
                    open.append(QStringLiteral("i"));
                } else if (name == QLatin1String("strong")) {
                    put(QStringLiteral("<b>"));
                    open.append(QStringLiteral("b"));
                } else if (name == QLatin1String("strikethrough")) {
                    put(QStringLiteral("<s>"));
                    open.append(QStringLiteral("s"));
                } else if (name == QLatin1String("sup") || name == QLatin1String("sub") || name == QLatin1String("code")) {
                    put(QLatin1Char('<') + name.toString() + QLatin1Char('>'));
                    open.append(name.toString());
                } else {
                    open.append(QString()); // poem, a, style, image…: their text only
                }
            }
        } else if (xml.isEndElement()) {
            const auto name = xml.name();
            if (name == QLatin1String("title-info")) {
                inTitleInfo = false;
            } else if (name == QLatin1String("author") && inAuthor) {
                inAuthor = false;
                authorDone = !author.isEmpty();
            } else if (name == QLatin1String("annotation")) {
                inAnnotation = false;
            } else if (name == QLatin1String("body")) {
                inBody = false;
            } else if (inBody) {
                if (name == QLatin1String("section")) {
                    --sections;
                } else if (name == QLatin1String("title")) {
                    --titleDepth;
                    if (chapter.title.isEmpty())
                        chapter.title = titleText.simplified();
                }
                if (name != QLatin1String("section") && name != QLatin1String("empty-line") && !open.isEmpty()) {
                    // In a title, a <p> opened nothing.
                    if (!(titleDepth && (name == QLatin1String("p") || name == QLatin1String("v")))) {
                        const auto tag = open.takeLast();
                        if (!tag.isEmpty())
                            put(QStringLiteral("</") + tag + QLatin1Char('>'));
                    }
                }
            }
        } else if (xml.isCharacters() && inBody && !xml.isWhitespace()) {
            const auto text = xml.text().toString();
            put(esc(text));
            if (titleDepth)
                titleText += text;
            else
                chapterHasText = true;
        }
    }
    if (xml.hasError() && book.chapters.isEmpty() && !chapterHasText && !metaOnly)
        throw Error(QStringLiteral("This FB2 book could not be read (%1).").arg(xml.errorString()));
    flush();
    book.author = author.join(QLatin1Char(' '));
    QString type;
    book.cover = fb2Binary(data, coverId, &type);
    if (!book.cover.isEmpty())
        book.coverSuffix = suffixFor(book.cover, type);
    for (int i = 0; i < book.chapters.size(); ++i)
        if (book.chapters[i].title.isEmpty())
            book.chapters[i].title = QStringLiteral("Part %1").arg(i + 1);
    return book;
}

namespace {

// ------------------------------------------------------------------ MOBI

quint32 be32(const QByteArray &b, qsizetype at) {
    if (at < 0 || at + 4 > b.size())
        return 0;
    return quint32(uchar(b[at])) << 24 | quint32(uchar(b[at + 1])) << 16 | quint32(uchar(b[at + 2])) << 8 | quint32(uchar(b[at + 3]));
}
quint16 be16(const QByteArray &b, qsizetype at) {
    if (at < 0 || at + 2 > b.size())
        return 0;
    return quint16(uchar(b[at]) << 8 | uchar(b[at + 1]));
}

// PalmDOC's LZ77.
QByteArray palmdoc(const QByteArray &in) {
    QByteArray out;
    out.reserve(in.size() * 2);
    for (qsizetype i = 0; i < in.size();) {
        const auto c = uchar(in[i++]);
        if (c >= 1 && c <= 8) {
            out.append(in.mid(i, c));
            i += c;
        } else if (c < 0x80) {
            out.append(char(c));
        } else if (c < 0xC0) {
            if (i >= in.size())
                break;
            const int pair = (c << 8) | uchar(in[i++]);
            const int distance = (pair >> 3) & 0x7FF;
            const int length = (pair & 7) + 3;
            if (distance <= 0 || distance > out.size())
                continue;
            for (int k = 0; k < length; ++k)
                out.append(out.at(out.size() - distance));
        } else {
            out.append(' ');
            out.append(char(c ^ 0x80));
        }
    }
    return out;
}

// How many bytes at the end of a text record are not text (the header's
// extra data flags): entries sized by a backward variable-width number,
// then the multibyte overlap.
qsizetype trailing(const QByteArray &record, quint16 flags) {
    qsizetype cut = 0;
    const auto entry = [&](qsizetype end) {
        quint32 value = 0;
        int shift = 0;
        while (end > 0) {
            const auto v = uchar(record[end - 1]);
            value |= quint32(v & 0x7F) << shift;
            shift += 7;
            --end;
            if ((v & 0x80) || shift >= 28)
                break;
        }
        return qsizetype(value);
    };
    for (quint16 bits = flags >> 1; bits; bits >>= 1)
        if (bits & 1)
            cut += entry(record.size() - cut);
    if ((flags & 1) && record.size() - cut > 0)
        cut += (uchar(record[record.size() - cut - 1]) & 3) + 1;
    return std::min(cut, record.size());
}

QString textOf(const QByteArray &html) { return HtmlDocument(html).root().textTrim(); }

} // namespace

Ebook readMobi(const QByteArray &data, bool metaOnly) {
    if (data.size() < 78 + 8 || (data.mid(60, 8) != "BOOKMOBI" && data.mid(60, 8) != "TEXtREAd"))
        throw Error(QStringLiteral("This is not a MOBI book."));
    const int records = be16(data, 76);
    QList<qsizetype> offsets;
    for (int i = 0; i < records; ++i)
        offsets.append(be32(data, 78 + i * 8));
    offsets.append(data.size());
    const auto record = [&](int i) -> QByteArray {
        if (i < 0 || i >= records || offsets[i] > offsets[i + 1] || offsets[i + 1] > data.size())
            return {};
        return data.mid(offsets[i], offsets[i + 1] - offsets[i]);
    };
    const auto first = record(0);
    const int compression = be16(first, 0);
    const int textRecords = be16(first, 8);
    const int encryption = be16(first, 12);
    if (encryption != 0)
        throw Error(QStringLiteral("This book is locked to a device (DRM), so SCANCORE cannot open it."));
    if (compression == 17480)
        throw Error(QStringLiteral("This MOBI uses Amazon's Huffman compression, which SCANCORE cannot read yet. Converted to EPUB (with Calibre, say), it opens."));
    if (compression != 1 && compression != 2)
        throw Error(QStringLiteral("This MOBI book could not be read (compression %1).").arg(compression));
    Ebook book;
    const bool mobi = first.mid(16, 4) == "MOBI";
    const quint32 headerLength = mobi ? be32(first, 20) : 0;
    const quint32 encoding = mobi ? be32(first, 28) : 1252;
    quint16 extraFlags = 0;
    int firstImage = -1;
    if (mobi) {
        firstImage = int(be32(first, 108));
        if (headerLength >= 0xE4)
            extraFlags = be16(first, 16 + 0xE2);
        const auto nameAt = be32(first, 84), nameLength = be32(first, 88);
        const auto name = first.mid(nameAt, nameLength);
        book.title = encoding == 65001 ? QString::fromUtf8(name) : decodeCodePage(name, 1252);
    }
    if (book.title.isEmpty())
        book.title = QString::fromLatin1(data.left(32)).section(QChar(0), 0, 0).replace(QLatin1Char('_'), QLatin1Char(' '));
    const auto decode = [&](const QByteArray &b) { return encoding == 65001 ? QString::fromUtf8(b) : decodeCodePage(b, 1252); };
    // EXTH: the author, a description, the cover, a later title.
    int coverOffset = -1;
    if (mobi && (be32(first, 128) & 0x40)) {
        const qsizetype at = 16 + headerLength;
        if (first.mid(at, 4) == "EXTH") {
            const quint32 count = be32(first, at + 8);
            qsizetype p = at + 12;
            for (quint32 i = 0; i < count && p + 8 <= first.size(); ++i) {
                const quint32 type = be32(first, p), length = be32(first, p + 4);
                if (length < 8)
                    break;
                const auto value = first.mid(p + 8, length - 8);
                if (type == 100)
                    book.author = book.author.isEmpty() ? decode(value) : book.author + QStringLiteral(", ") + decode(value);
                else if (type == 103)
                    book.description = textOf(value);
                else if (type == 503 && !value.isEmpty())
                    book.title = decode(value);
                else if (type == 201 && value.size() >= 4)
                    coverOffset = int(be32(value, 0));
                p += length;
            }
        }
    }
    if (firstImage > 0 && coverOffset >= 0 && firstImage + coverOffset < records) {
        book.cover = record(firstImage + coverOffset);
        book.coverSuffix = suffixFor(book.cover);
    }
    if (metaOnly)
        return book;

    QByteArray text;
    for (int i = 1; i <= textRecords && i < records; ++i) {
        auto r = record(i);
        r.chop(trailing(r, extraFlags));
        text += compression == 2 ? palmdoc(r) : r;
    }
    const auto markup = decode(text);
    // Chapters: Mobipocket's page breaks; KF8 (AZW3) gives each of its
    // files its own <html>.
    static const QRegularExpression pageBreak(QStringLiteral(R"(<mbp:pagebreak\s*/?>)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression htmlStart(QStringLiteral(R"((?=<html[\s>]))"), QRegularExpression::CaseInsensitiveOption);
    auto parts = markup.split(pageBreak);
    if (parts.size() <= 1)
        parts = markup.split(htmlStart, Qt::SkipEmptyParts);
    // Short pieces (a title page, a dedication) go with the next.
    QString pending;
    static const QRegularExpression heading(QStringLiteral(R"(<h[1-3][^>]*>(.*?)</h[1-3]>)"),
                                            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    for (int i = 0; i < parts.size(); ++i) {
        pending += parts[i];
        const auto plain = textOf(pending.toUtf8());
        if (plain.size() < 400 && i + 1 < parts.size())
            continue;
        if (plain.isEmpty()) {
            pending.clear();
            continue;
        }
        EbookChapter chapter;
        chapter.html = pending.toUtf8();
        const auto m = heading.match(pending);
        chapter.title = m.hasMatch() ? textOf(m.captured(1).toUtf8()).simplified() : QString();
        if (chapter.title.isEmpty() || chapter.title.size() > 120)
            chapter.title = QStringLiteral("Part %1").arg(book.chapters.size() + 1);
        book.chapters.append(chapter);
        pending.clear();
    }
    if (book.chapters.isEmpty())
        throw Error(QStringLiteral("This MOBI book has no text SCANCORE can read."));
    return book;
}

bool isEbook(const QString &name) {
    const auto lower = name.toLower();
    for (const auto ext : {".fb2", ".fb2.zip", ".mobi", ".azw", ".azw3", ".prc"})
        if (lower.endsWith(QLatin1String(ext)))
            return true;
    return false;
}

Ebook readEbook(const QString &path, bool metaOnly) {
    const auto lower = path.toLower();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw Error(QStringLiteral("%1 could not be opened.").arg(QFileInfo(path).fileName()), 404);
    if (lower.endsWith(QLatin1String(".fb2.zip"))) {
        for (const auto &name : zipEntries(path))
            if (name.toLower().endsWith(QLatin1String(".fb2")))
                return readFb2(zipEntry(path, name), metaOnly);
        throw Error(QStringLiteral("%1 holds no FB2 book.").arg(QFileInfo(path).fileName()));
    }
    if (lower.endsWith(QLatin1String(".fb2")))
        return readFb2(file.readAll(), metaOnly);
    return readMobi(file.readAll(), metaOnly);
}

} // namespace sc::local
