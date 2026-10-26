#include "Translator.h"

#include "App.h"
#include "OnDevice.h"
#include "core/Database.h"
#include "core/Http.h"
#include "core/ImageCache.h"
#include "core/Task.h"
#include "core/translation/Cloud.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#endif

namespace sc {

namespace {

using namespace translation;

constexpr qint64 kMonth = 30LL * 24 * 3600;
// Tall webtoon strips are read in slices this high, overlapping so a line
// cut at a slice's edge is whole in the next one.
constexpr int kSlice = 3000;
constexpr int kOverlap = 240;

QString setting(const char *key) { return App::instance()->settings().value(QLatin1String(key)).toString().trimmed(); }

bool onAndroid() {
#ifdef Q_OS_ANDROID
    return true;
#else
    return false;
#endif
}

#ifdef Q_OS_ANDROID
constexpr auto kMlKit = "com/mediumdev/scancore/MlKit";
#endif

// The page's lines from Android's recogniser.
QList<TextLine> mlkitLines(const QByteArray &image, const QString &language) {
#ifdef Q_OS_ANDROID
    QJniEnvironment env;
    auto bytes = env->NewByteArray(jsize(image.size()));
    env->SetByteArrayRegion(bytes, 0, jsize(image.size()), reinterpret_cast<const jbyte *>(image.constData()));
    const auto answer = QJniObject::callStaticMethod<jstring>(kMlKit, "recognize", "([BLjava/lang/String;)Ljava/lang/String;", bytes,
                                                             QJniObject::fromString(language).object<jstring>())
                            .toString();
    env->DeleteLocalRef(bytes);
    const auto json = QJsonDocument::fromJson(answer.toUtf8());
    if (json.isObject())
        throw Error(QObject::tr("Android could not read the page: %1").arg(json.object().value(QStringLiteral("error")).toString()));
    QList<TextLine> out;
    for (const auto &v : json.array()) {
        const auto o = v.toObject();
        out.append({QRectF(o.value(QStringLiteral("x")).toDouble(), o.value(QStringLiteral("y")).toDouble(),
                           o.value(QStringLiteral("w")).toDouble(), o.value(QStringLiteral("h")).toDouble()),
                    o.value(QStringLiteral("text")).toString()});
    }
    return out;
#else
    Q_UNUSED(image)
    Q_UNUSED(language)
    throw Error(QObject::tr("This device has no text recogniser."));
#endif
}

QStringList mlkitTranslate(const QStringList &texts, const QString &from, const QString &to) {
#ifdef Q_OS_ANDROID
    if (from.isEmpty())
        throw Error(QObject::tr("Android's translator needs to know the language: pick it in the reader."));
    const auto answer = QJniObject::callStaticMethod<jstring>(
                            kMlKit, "translate", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                            QJniObject::fromString(QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(texts)).toJson(QJsonDocument::Compact)))
                                .object<jstring>(),
                            QJniObject::fromString(from).object<jstring>(), QJniObject::fromString(to).object<jstring>())
                            .toString();
    const auto json = QJsonDocument::fromJson(answer.toUtf8());
    if (json.isObject())
        throw Error(QObject::tr("Android could not translate: %1").arg(json.object().value(QStringLiteral("error")).toString()));
    QStringList out;
    for (const auto &v : json.array())
        out.append(v.toString());
    return out;
#else
    Q_UNUSED(texts)
    Q_UNUSED(from)
    Q_UNUSED(to)
    throw Error(QObject::tr("This device has no translator of its own."));
#endif
}

QByteArray encoded(const QImage &image) {
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 90);
    return out;
}

// Translates in batches the services take.
QStringList translateWith(const QString &engine, const QStringList &texts, const QString &from, const QString &to) {
    QStringList out;
    const int batch = engine == QLatin1String("chat") ? 40 : 50;
    for (int i = 0; i < texts.size(); i += batch) {
        const auto part = texts.mid(i, batch);
        if (engine == QLatin1String("deepl"))
            out += deepl(part, from, to, setting("translateDeeplKey"));
        else if (engine == QLatin1String("google"))
            out += google(part, from, to, setting("translateGoogleKey"));
        else if (engine == QLatin1String("chat"))
            out += chatModel(part, from, to, setting("translateChatUrl"), setting("translateChatKey"), setting("translateChatModel"));
        else if (engine == QLatin1String("mlkit"))
            out += mlkitTranslate(part, from, to);
        else if (engine == QLatin1String("local"))
            out += OnDevice::instance()->translate(part, from, to);
        else if (engine == QLatin1String("fake"))
            for (const auto &t : part)
                out += QStringLiteral("Translated: ") + t;
        else
            throw Error(QObject::tr("Pick a way to translate in Settings → Reading → Translate."));
    }
    return out;
}

QString hashOf(const QString &text) {
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex());
}

// A page's text, found once per reader and language and kept.
struct Recognised {
    int width = 0, height = 0;
    QList<TextBlock> blocks;
};

Recognised recognize(const QString &url, const QString &referer, const QString &reader, const QString &from) {
    const auto key = QStringLiteral("translate:ocr:") + hashOf(url + QLatin1Char('|') + reader + QLatin1Char('|') + from);
    if (const auto cached = Database::cacheGet(key)) {
        const auto o = QJsonDocument::fromJson(*cached).object();
        Recognised r{o.value(QStringLiteral("w")).toInt(), o.value(QStringLiteral("h")).toInt(), {}};
        for (const auto &v : o.value(QStringLiteral("b")).toArray()) {
            const auto b = v.toObject();
            r.blocks.append({QRectF(b.value(QStringLiteral("x")).toDouble(), b.value(QStringLiteral("y")).toDouble(),
                                    b.value(QStringLiteral("w")).toDouble(), b.value(QStringLiteral("h")).toDouble()),
                             b.value(QStringLiteral("t")).toString(), b.value(QStringLiteral("v")).toBool()});
        }
        return r;
    }

    const auto bytes = ImageCache::fetch(url, referer);
    QImage image;
    if (bytes.isEmpty() || !image.loadFromData(bytes))
        throw Error(QObject::tr("The page's picture could not be read."));

    // Text found in slices of a tall strip, then grouped into bubbles.
    QList<TextLine> lines;
    QList<TextBlock> blocks;
    const int height = image.height();
    for (int top = 0; top < height; top += kSlice - kOverlap) {
        const int bottom = std::min(height, top + kSlice);
        const bool whole = top == 0 && bottom == height;
        const auto part = whole || reader == QLatin1String("local") ? bytes : encoded(image.copy(0, top, image.width(), bottom - top));
        // Lines whose middle is in this slice's own share (not the
        // overlap the next slice reads again).
        const double from0 = top == 0 ? 0 : top + kOverlap / 2.0;
        const double to0 = bottom == height ? height : bottom - kOverlap / 2.0;
        if (reader == QLatin1String("fake")) {
            if (top == 0) {
                const double w = image.width(), line = w * 0.05;
                lines.append({QRectF(w * 0.12, line * 2, w * 0.4, line), QStringLiteral("¿Qué demonios")});
                lines.append({QRectF(w * 0.14, line * 3.1, w * 0.36, line), QStringLiteral("es esto?")});
                lines.append({QRectF(w * 0.55, line * 8, w * 0.3, line * 1.2), QStringLiteral("¡Corre!")});
            }
        } else if (reader == QLatin1String("vision")) {
            for (auto b : googleVision(part, from, setting("translateGoogleKey"))) {
                b.box.translate(0, top);
                if (b.box.center().y() >= from0 && b.box.center().y() < to0)
                    blocks.append(b);
            }
        } else if (reader == QLatin1String("local")) {
            // On the computer: the slice as a picture, no re-encoding.
            for (auto l : OnDevice::instance()->read(image.copy(0, top, image.width(), bottom - top), from)) {
                l.box.translate(0, top);
                if (l.box.center().y() >= from0 && l.box.center().y() < to0)
                    lines.append(l);
            }
        } else {
            for (auto l : mlkitLines(part, from)) {
                l.box.translate(0, top);
                if (l.box.center().y() >= from0 && l.box.center().y() < to0)
                    lines.append(l);
            }
        }
        if (bottom == height)
            break;
    }
    if (!lines.isEmpty())
        blocks = groupLines(lines, from);

    QJsonArray stored;
    for (const auto &b : std::as_const(blocks))
        stored.append(QJsonObject{{"x", b.box.x()}, {"y", b.box.y()}, {"w", b.box.width()}, {"h", b.box.height()}, {"t", b.text}, {"v", b.vertical}});
    Database::cachePut(key, QJsonDocument(QJsonObject{{"w", image.width()}, {"h", height}, {"b", stored}}).toJson(QJsonDocument::Compact), kMonth);
    return {image.width(), height, blocks};
}

QString pageKey(const QString &url, const QString &reader, const QString &translator, const QString &from, const QString &to) {
    return QStringLiteral("translate:page:") +
           hashOf(url + QLatin1Char('|') + reader + QLatin1Char('|') + translator + QLatin1Char('|') + from + QLatin1Char('|') + to);
}

// What the model is told besides a page's bubbles: the story so far and the
// names kept for this work, the pages before (translated, else only read)
// and the page after, and the page itself when the reader allows it.
ChatContext contextFor(const QVariantMap &hint, const QString &referer, const QString &reader, const QString &translator, const QString &from,
                       const QString &to) {
    ChatContext out;
    out.work = hint.value(QStringLiteral("work")).toString();
    out.chapter = hint.value(QStringLiteral("chapter")).toString();
    const auto scope = hint.value(QStringLiteral("scope")).toString();
    if (!scope.isEmpty()) {
        const auto o = QJsonDocument::fromJson(Database::cacheGet(QStringLiteral("translate:memory:") + hashOf(scope + QLatin1Char('|') + to)).value_or(QByteArray())).object();
        out.memory = o.value(QStringLiteral("summary")).toString();
        for (const auto &g : o.value(QStringLiteral("glossary")).toArray())
            out.glossary.append({g.toObject().value(QStringLiteral("term")).toString(), g.toObject().value(QStringLiteral("translation")).toString()});
    }
    // Nothing here may fail the page: the context is a help, not a need.
    for (const auto &u : hint.value(QStringLiteral("before")).toStringList()) {
        try {
            if (const auto cached = Database::cacheGet(pageKey(u, reader, translator, from, to))) {
                for (const auto &b : QJsonDocument::fromJson(*cached).object().value(QStringLiteral("blocks")).toArray())
                    out.before.append({b.toObject().value(QStringLiteral("source")).toString(), b.toObject().value(QStringLiteral("text")).toString()});
            } else {
                for (const auto &b : recognize(u, referer, reader, from).blocks)
                    out.before.append({b.text, QString()});
            }
        } catch (const Error &) {
        }
    }
    for (const auto &u : hint.value(QStringLiteral("after")).toStringList()) {
        try {
            for (const auto &b : recognize(u, referer, reader, from).blocks)
                out.after.append({b.text, QString()});
        } catch (const Error &) {
        }
    }
    constexpr int kKept = 60;
    while (out.before.size() > kKept)
        out.before.removeFirst();
    while (out.after.size() > 20)
        out.after.removeLast();
    return out;
}

// The page as the model should see it: no larger than it can use.
QByteArray pictureFor(const QString &url, const QString &referer) {
    QImage image;
    if (!image.loadFromData(ImageCache::fetch(url, referer)) || image.height() > image.width() * 3)
        return {};
    if (image.width() > 1400)
        image = image.scaledToWidth(1400, Qt::SmoothTransformation);
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 80);
    return out;
}

// Texts through a model in batches, each told what came before it (earlier
// batches, translated) and what follows; the memory is kept for the work.
QStringList translateChat(const QStringList &texts, const QString &from, const QString &to, ChatContext context, const QString &scope) {
    QStringList out;
    constexpr int batch = 40;
    const auto after = context.after;
    for (int i = 0; i < texts.size(); i += batch) {
        const auto part = texts.mid(i, batch);
        context.after = after;
        if (i + batch < texts.size()) {
            context.after.clear();
            for (const auto &t : texts.mid(i + batch, 8))
                context.after.append({t, QString()});
        }
        auto answer = chatModel(part, from, to, setting("translateChatUrl"), setting("translateChatKey"), setting("translateChatModel"), context);
        out += answer.texts;
        // The next batch goes on from this one.
        for (int j = 0; j < part.size(); ++j)
            context.before.append({part[j], answer.texts[j]});
        while (context.before.size() > 60)
            context.before.removeFirst();
        if (!answer.summary.isEmpty())
            context.memory = answer.summary;
        for (const auto &g : std::as_const(answer.glossary)) {
            bool found = false;
            for (auto &known : context.glossary)
                if (known.first == g.first) {
                    known.second = g.second;
                    found = true;
                }
            if (!found)
                context.glossary.append(g);
        }
    }
    if (!scope.isEmpty()) {
        QJsonArray glossary;
        for (const auto &[term, translation] : std::as_const(context.glossary))
            glossary.append(QJsonObject{{"term", term}, {"translation", translation}});
        while (glossary.size() > 150)
            glossary.removeFirst();
        Database::cachePut(QStringLiteral("translate:memory:") + hashOf(scope + QLatin1Char('|') + to),
                           QJsonDocument(QJsonObject{{"summary", context.memory}, {"glossary", glossary}}).toJson(QJsonDocument::Compact),
                           90LL * 24 * 3600);
    }
    return out;
}

} // namespace

Translator *Translator::instance() {
    static auto *translator = [] {
        auto *created = new Translator;
        QJSEngine::setObjectOwnership(created, QJSEngine::CppOwnership);
        return created;
    }();
    return translator;
}

Translator::Translator(QObject *parent) : QObject(parent) {
    connect(App::instance(), &App::settingsChanged, this, &Translator::changed);
    connect(OnDevice::instance(), &OnDevice::changed, this, &Translator::changed);
}

QString Translator::target() const {
    const auto own = setting("translateTarget");
    if (!own.isEmpty())
        return own;
    const auto app = QLocale().name().section(QLatin1Char('_'), 0, 0);
    return app.isEmpty() || app == QLatin1String("C") ? QStringLiteral("en") : app;
}

// Development hook: SCANCORE_TRANSLATE_FAKE=1 finds two bubbles on every
// page and "translates" by marking the text, to see the reader's overlay
// without keys.
bool fakeEngines() { return qEnvironmentVariableIsSet("SCANCORE_TRANSLATE_FAKE"); }

QString Translator::readerEngine() const {
    if (fakeEngines())
        return QStringLiteral("fake");
    const auto chosen = setting("translateReader");
    if (chosen == QLatin1String("vision"))
        return setting("translateGoogleKey").isEmpty() ? QString() : chosen;
    if (chosen == QLatin1String("mlkit"))
        return onAndroid() ? chosen : QString();
    if (chosen == QLatin1String("local"))
        return OnDevice::instance()->any() ? chosen : QString();
    // Automatic: languages downloaded to this device (on phones too, for
    // those without Google Play services), else the phone's own, else Vision
    // with a Google key.
    if (onAndroid())
        return OnDevice::instance()->any() ? QStringLiteral("local") : QStringLiteral("mlkit");
    if (!setting("translateGoogleKey").isEmpty())
        return QStringLiteral("vision");
    return OnDevice::instance()->any() ? QStringLiteral("local") : QString();
}

QString Translator::textEngine() const {
    if (fakeEngines())
        return QStringLiteral("fake");
    const auto chosen = setting("translateText");
    if (chosen == QLatin1String("deepl"))
        return setting("translateDeeplKey").isEmpty() ? QString() : chosen;
    if (chosen == QLatin1String("google"))
        return setting("translateGoogleKey").isEmpty() ? QString() : chosen;
    if (chosen == QLatin1String("chat"))
        return setting("translateChatUrl").isEmpty() || setting("translateChatModel").isEmpty() ? QString() : chosen;
    if (chosen == QLatin1String("mlkit"))
        return onAndroid() ? chosen : QString();
    if (chosen == QLatin1String("local"))
        return OnDevice::instance()->any() ? chosen : QString();
    // Automatic: a key the reader gave, else the phone's own, else the
    // computer's.
    if (!setting("translateDeeplKey").isEmpty())
        return QStringLiteral("deepl");
    if (!setting("translateGoogleKey").isEmpty())
        return QStringLiteral("google");
    if (!setting("translateChatUrl").isEmpty() && !setting("translateChatModel").isEmpty())
        return QStringLiteral("chat");
    if (onAndroid())
        return OnDevice::instance()->any() ? QStringLiteral("local") : QStringLiteral("mlkit");
    return OnDevice::instance()->any() ? QStringLiteral("local") : QString();
}

bool Translator::pagesReady() const { return !readerEngine().isEmpty() && !textEngine().isEmpty(); }
bool Translator::textReady() const { return !textEngine().isEmpty(); }

QString Translator::hint() const {
    if (pagesReady())
        return {};
    if (readerEngine().isEmpty())
        return OnDevice::instance()->available()
                   ? tr("To translate pages on a computer, download a language in Settings → Reading → Translate on this computer, or add a "
                        "Google Cloud key there.")
                   : tr("To translate pages on a computer, add a Google Cloud key in Settings → Reading → Translate (Vision reads the "
                        "page's text).");
    return tr("Add a DeepL or Google key, or a model's address, in Settings → Reading → Translate.");
}

QVariantList Translator::readers() const {
    QVariantList out{QVariantMap{{"value", "auto"}, {"label", tr("Automatic")}}};
    if (onAndroid())
        out.append(QVariantMap{{"value", "mlkit"}, {"label", tr("This phone")}, {"detail", tr("On the phone, with Google Play services")}});
    if (OnDevice::instance()->available())
        out.append(onAndroid() ? QVariantMap{{"value", "local"}, {"label", tr("Downloaded languages")}, {"detail", tr("Downloaded once; pages never leave the device")}}
                               : QVariantMap{{"value", "local"}, {"label", tr("This computer")}, {"detail", tr("Downloaded once; pages never leave the computer")}});
    out.append(QVariantMap{{"value", "vision"}, {"label", tr("Google Cloud Vision")}, {"detail", tr("Your Google Cloud key")}});
    return out;
}

QVariantList Translator::translators() const {
    QVariantList out{QVariantMap{{"value", "auto"}, {"label", tr("Automatic")}}};
    if (onAndroid())
        out.append(QVariantMap{{"value", "mlkit"}, {"label", tr("This phone")}, {"detail", tr("On the phone; a model per language")}});
    if (OnDevice::instance()->available())
        out.append(onAndroid() ? QVariantMap{{"value", "local"}, {"label", tr("Downloaded languages")}, {"detail", tr("Downloaded once; nothing leaves the device")}}
                               : QVariantMap{{"value", "local"}, {"label", tr("This computer")}, {"detail", tr("Downloaded once; nothing leaves the computer")}});
    out.append(QVariantMap{{"value", "deepl"}, {"label", QStringLiteral("DeepL")}, {"detail", tr("Your DeepL key")}});
    out.append(QVariantMap{{"value", "google"}, {"label", tr("Google Translate")}, {"detail", tr("Your Google Cloud key")}});
    out.append(QVariantMap{{"value", "chat"}, {"label", tr("A language model")}, {"detail", tr("Any OpenAI-compatible service, or one on your computer")}});
    return out;
}

void Translator::clearCache() { Database::query(QStringLiteral("DELETE FROM cache WHERE key LIKE 'translate:%'")); }

Task *Translator::page(const QString &url, const QString &referer, const QString &from) { return page(url, referer, from, {}); }

Task *Translator::page(const QString &url, const QString &referer, const QString &from, const QVariantMap &hint) {
    const auto reader = readerEngine();
    const auto translator = textEngine();
    const auto to = target();
    return Task::run([url, referer, from, hint, reader, translator, to]() -> QVariant {
        if (reader.isEmpty() || translator.isEmpty())
            throw Error(Translator::instance()->hint());
        const auto key = pageKey(url, reader, translator, from, to);
        if (const auto cached = Database::cacheGet(key))
            return QJsonDocument::fromJson(*cached).toVariant();

        const auto found = recognize(url, referer, reader, from);
        const auto &blocks = found.blocks;
        const int height = found.height;

        QStringList sources;
        for (const auto &b : blocks)
            sources.append(b.text);
        QStringList translated;
        if (!sources.isEmpty()) {
            if (translator == QLatin1String("chat")) {
                auto context = contextFor(hint, referer, reader, translator, from, to);
                if (setting("translateChatImage") == QLatin1String("true"))
                    context.image = pictureFor(url, referer);
                translated = translateChat(sources, from, to, context, hint.value(QStringLiteral("scope")).toString());
            } else {
                translated = translateWith(translator, sources, from, to);
            }
        }
        QJsonArray out;
        for (int i = 0; i < blocks.size(); ++i) {
            const auto &b = blocks[i];
            out.append(QJsonObject{{"x", b.box.x() / found.width},
                                   {"y", b.box.y() / height},
                                   {"w", b.box.width() / found.width},
                                   {"h", b.box.height() / height},
                                   {"text", translated.value(i)},
                                   {"source", b.text},
                                   {"vertical", b.vertical}});
        }
        const QJsonObject result{{"width", found.width}, {"height", height}, {"blocks", out}, {"engine", reader + QLatin1Char('+') + translator}};
        Database::cachePut(key, QJsonDocument(result).toJson(QJsonDocument::Compact), kMonth);
        return result.toVariantMap();
    });
}

Task *Translator::texts(const QStringList &texts, const QString &from) { return Translator::texts(texts, from, {}); }

Task *Translator::texts(const QStringList &texts, const QString &from, const QVariantMap &hint) {
    const auto translator = textEngine();
    const auto to = target();
    return Task::run([texts, from, hint, translator, to]() -> QVariant {
        if (translator.isEmpty())
            throw Error(Translator::instance()->hint());
        // Each text on its own in the cache, so a chapter read again (or
        // half of it) costs nothing.
        QStringList out(texts.size());
        QStringList missing;
        QList<int> where;
        const auto keyFor = [&](const QString &text) {
            return QStringLiteral("translate:text:") +
                   QString::fromLatin1(QCryptographicHash::hash((translator + QLatin1Char('|') + from + QLatin1Char('|') + to + QLatin1Char('|') + text).toUtf8(),
                                                                QCryptographicHash::Sha1)
                                           .toHex());
        };
        for (int i = 0; i < texts.size(); ++i) {
            if (texts[i].trimmed().isEmpty())
                continue;
            if (const auto cached = Database::cacheGet(keyFor(texts[i])))
                out[i] = QString::fromUtf8(*cached);
            else {
                missing.append(texts[i]);
                where.append(i);
            }
        }
        if (!missing.isEmpty()) {
            const auto done = translator == QLatin1String("chat")
                ? translateChat(missing, from, to, contextFor(hint, {}, {}, translator, from, to), hint.value(QStringLiteral("scope")).toString())
                : translateWith(translator, missing, from, to);
            for (int j = 0; j < where.size(); ++j) {
                out[where[j]] = done.value(j);
                Database::cachePut(keyFor(missing[j]), done.value(j).toUtf8(), kMonth);
            }
        }
        return out;
    });
}

} // namespace sc
