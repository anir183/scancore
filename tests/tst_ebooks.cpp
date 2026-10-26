// FB2 and MOBI books for the novel reader: their titles, authors, covers and
// chapters, windows-1251 and zipped FB2, PalmDOC with trailing entries, the
// books refused with a message, and the "On this device" source reading
// them by chapter. Fixtures: tests/fixtures/ebooks (make.py writes them).

#include "core/Http.h"
#include "sources/local/Ebooks.h"
#include "sources/local/LocalSources.h"

#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace sc;
using namespace sc::local;

namespace {

QString fixturePath(const QString &name) { return QStringLiteral(SCANCORE_SOURCE_DIR "/tests/fixtures/ebooks/") + name; }

QByteArray fixture(const QString &name) {
    QFile f(fixturePath(name));
    if (!f.open(QIODevice::ReadOnly))
        qFatal("missing fixture %s", qPrintable(name));
    return f.readAll();
}

QString textOf(const EbookChapter &chapter) { return QString::fromUtf8(chapter.html); }

// Record 0 of a PDB file starts at the first entry of its record list.
void setRecord0Field(QByteArray &book, int at, quint16 value) {
    const auto start = qsizetype(quint32(uchar(book[78])) << 24 | quint32(uchar(book[79])) << 16 | quint32(uchar(book[80])) << 8 | uchar(book[81]));
    book[start + at] = char(value >> 8);
    book[start + at + 1] = char(value & 0xFF);
}

} // namespace

class Ebooks : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void kinds() {
        QVERIFY(isEbook(QStringLiteral("a.fb2")));
        QVERIFY(isEbook(QStringLiteral("A.FB2.ZIP")));
        QVERIFY(isEbook(QStringLiteral("b.mobi")));
        QVERIFY(isEbook(QStringLiteral("c.azw3")));
        QVERIFY(isEbook(QStringLiteral("d.prc")));
        QVERIFY(!isEbook(QStringLiteral("e.epub")));
        QVERIFY(!isEbook(QStringLiteral("f.cbz")));
    }

    void fb2() {
        const auto book = readFb2(fixture(QStringLiteral("book.fb2")));
        QCOMPARE(book.title, QStringLiteral("The Test Book"));
        QCOMPARE(book.author, QStringLiteral("Anna K. Writer"));
        QCOMPARE(book.description, QStringLiteral("A book for the tests.\n\nSecond line."));
        QVERIFY(book.cover.startsWith("\x89PNG"));
        QCOMPARE(book.coverSuffix, QStringLiteral("png"));
        // The book's title before the first section is no chapter; "Part
        // Two" has no text of its own; the footnotes are one chapter.
        QStringList titles;
        for (const auto &c : book.chapters)
            titles.append(c.title);
        QCOMPARE(titles, (QStringList{QStringLiteral("Chapter 1 The Start"), QStringLiteral("Chapter 2"), QStringLiteral("Part 3"),
                                      QStringLiteral("Notes")}));
        const auto first = textOf(book.chapters[0]);
        QVERIFY2(first.startsWith(QStringLiteral("<h2>Chapter 1 The Start</h2>")), qPrintable(first));
        QVERIFY(first.contains(QStringLiteral("<blockquote><p>An epigraph.</p><p><i>Someone</i></p></blockquote>")));
        QVERIFY(first.contains(QStringLiteral("<i>slanted</i> and <b>bold</b>.</p>")));
        QVERIFY(first.contains(QStringLiteral("<br>")));
        QVERIFY(!first.contains(QStringLiteral("The Test Book")));
        const auto second = textOf(book.chapters[1]);
        QVERIFY2(second.contains(QStringLiteral("<h3>A subtitle</h3><div><p>Line one</p><p>Line two</p></div>")), qPrintable(second));
        QVERIFY(!second.contains(QStringLiteral("Part Two")));
        QCOMPARE(textOf(book.chapters[2]), QStringLiteral("<p>A section without a title.</p>"));
        QCOMPARE(textOf(book.chapters[3]), QStringLiteral("<p><b>1</b></p><p>The footnote.</p>"));

        // For lists: no chapters, the rest the same.
        const auto meta = readFb2(fixture(QStringLiteral("book.fb2")), true);
        QCOMPARE(meta.title, book.title);
        QCOMPARE(meta.author, book.author);
        QCOMPARE(meta.cover, book.cover);
        QVERIFY(meta.chapters.isEmpty());
    }

    void fb2Cyrillic() {
        const auto raw = fixture(QStringLiteral("book-1251.fb2"));
        QVERIFY(!raw.contains(QStringLiteral("Глава").toUtf8())); // really windows-1251
        const auto book = readFb2(raw);
        QCOMPARE(book.title, QStringLiteral("Тестовая книга"));
        QCOMPARE(book.author, QStringLiteral("Анна К. Писатель"));
        QCOMPARE(book.chapters.size(), 4);
        QCOMPARE(book.chapters[0].title, QStringLiteral("Глава 1 The Start"));
        QVERIFY(textOf(book.chapters[0]).contains(QStringLiteral("Первый абзац, с буквами ёЁ «кавычками» и")));
    }

    void fb2Zip() {
        const auto book = readEbook(fixturePath(QStringLiteral("book.fb2.zip")));
        QCOMPARE(book.title, QStringLiteral("The Test Book"));
        QCOMPARE(book.chapters.size(), 4);
    }

    void fb2Broken() {
        QVERIFY_THROWS_EXCEPTION(Error, readFb2("<?xml version=\"1.0\"?><FictionBook><body><section><p>cut"));
    }

    void mobi() {
        const auto book = readMobi(fixture(QStringLiteral("book.mobi")));
        // EXTH's updated title over the header's; its author, description
        // and cover (the first image record).
        QCOMPARE(book.title, QStringLiteral("The MOBI Book (Updated)"));
        QCOMPARE(book.author, QStringLiteral("Mo Bi"));
        QCOMPARE(book.description, QStringLiteral("A MOBI for the tests."));
        QVERIFY(book.cover.startsWith("\x89PNG"));
        QCOMPARE(book.coverSuffix, QStringLiteral("png"));
        // The short title page goes with chapter one.
        QCOMPARE(book.chapters.size(), 3);
        QCOMPARE(book.chapters[0].title, QStringLiteral("Chapter One"));
        QCOMPARE(book.chapters[1].title, QStringLiteral("Chapter Two"));
        QCOMPARE(book.chapters[2].title, QStringLiteral("Part 3"));
        const auto one = textOf(book.chapters[0]);
        QVERIFY(one.contains(QStringLiteral("<h1>The MOBI Book</h1>")));
        QVERIFY(one.contains(QStringLiteral("<p>Ünïcödé — “quotes” ✓</p>")));
        // The character over the first record's end, whole (the multibyte
        // overlap and the trailing entry cut off).
        QVERIFY2(one.contains(QStringLiteral("xxx✓</p>")), qPrintable(one.right(40)));
        QVERIFY(!one.contains(QChar(0xFFFD)));
        const auto three = textOf(book.chapters[2]);
        QVERIFY(three.contains(QStringLiteral("sit29 amet29")));
        QVERIFY(three.trimmed().endsWith(QStringLiteral("</body></html>")));
        for (const auto &c : book.chapters)
            QVERIFY(!c.html.contains('\x82') && !c.html.contains('\x07'));

        const auto meta = readMobi(fixture(QStringLiteral("book.mobi")), true);
        QCOMPARE(meta.title, book.title);
        QVERIFY(meta.chapters.isEmpty());
    }

    void mobiRefused_data() {
        QTest::addColumn<int>("field");
        QTest::addColumn<int>("value");
        QTest::addColumn<QString>("message");
        QTest::newRow("drm") << 12 << 2 << QStringLiteral("DRM");
        QTest::newRow("huffman") << 0 << 17480 << QStringLiteral("Huffman");
        QTest::newRow("unknown") << 0 << 9 << QStringLiteral("compression 9");
    }
    void mobiRefused() {
        QFETCH(int, field);
        QFETCH(int, value);
        QFETCH(QString, message);
        auto data = fixture(QStringLiteral("book.mobi"));
        setRecord0Field(data, field, quint16(value));
        try {
            readMobi(data);
            QFAIL("no error");
        } catch (const Error &e) {
            QVERIFY2(e.message().contains(message), qPrintable(e.message()));
        }
        QVERIFY_THROWS_EXCEPTION(Error, readMobi("not a book"));
    }

    // The novels' "On this device" source: the book and its chapters by
    // "#ch=N" addresses, as text blocks with the heading as the title.
    void source() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (const auto name : {"book.fb2", "book.mobi"})
            QVERIFY(QFile::copy(fixturePath(QLatin1String(name)), dir.filePath(QLatin1String(name))));
        novels::SourceConfig config;
        config.key = QStringLiteral("local");
        config.engine = QStringLiteral("local");
        config.base = QStringLiteral("file://");
        auto source = createNovelSource(config);

        const auto url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("book.fb2"))).toString();
        QList<novels::ChapterRef> chapters;
        const auto book = source->book(url, chapters);
        QCOMPARE(book.title, QStringLiteral("The Test Book"));
        QCOMPARE(book.author, QStringLiteral("Anna K. Writer"));
        QCOMPARE(book.chapterCount, 4);
        QVERIFY(book.cover.startsWith(QStringLiteral("file://")));
        QVERIFY(QFile::exists(QUrl(book.cover).toLocalFile()));
        QCOMPARE(chapters.size(), 4);
        QCOMPARE(chapters[1].url, url + QStringLiteral("#ch=1"));
        QCOMPARE(chapters[1].title, QStringLiteral("Chapter 2"));
        QCOMPARE(chapters[1].number, QStringLiteral("2"));

        QString title;
        const auto blocks = source->chapter(chapters[0].url, title);
        QCOMPARE(title, QStringLiteral("Chapter 1 The Start"));
        QStringList texts;
        for (const auto &b : blocks)
            texts.append(b.kind + QLatin1Char(':') + b.text);
        QVERIFY2(texts.contains(QStringLiteral("paragraph:The first paragraph, with slanted and bold.")), qPrintable(texts.join(u'|')));
        QVERIFY(!texts.join(u'|').contains(QStringLiteral("Chapter 1")));

        QVERIFY_THROWS_EXCEPTION(Error, source->chapter(url + QStringLiteral("#ch=9"), title));

        const auto mobiUrl = QUrl::fromLocalFile(dir.filePath(QStringLiteral("book.mobi"))).toString();
        chapters.clear();
        QCOMPARE(source->book(mobiUrl, chapters).title, QStringLiteral("The MOBI Book (Updated)"));
        QCOMPARE(chapters.size(), 3);
        const auto two = source->chapter(chapters[1].url, title);
        QCOMPARE(title, QStringLiteral("Chapter Two"));
        QVERIFY(two.size() >= 30);

        // A changed file is read again, not served from memory.
        QFile::remove(dir.filePath(QStringLiteral("book.fb2")));
        QVERIFY(QFile::copy(fixturePath(QStringLiteral("book-1251.fb2")), dir.filePath(QStringLiteral("book.fb2"))));
        QFile touched(dir.filePath(QStringLiteral("book.fb2")));
        QVERIFY(touched.open(QIODevice::ReadWrite));
        QVERIFY(touched.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime));
        touched.close();
        chapters.clear();
        QCOMPARE(source->book(url, chapters).title, QStringLiteral("Тестовая книга"));
    }
};

QTEST_GUILESS_MAIN(Ebooks)
#include "tst_ebooks.moc"
