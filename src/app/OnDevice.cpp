#include "OnDevice.h"

#include "Archive.h"
#include "LocalOcr.h"
#include "LocalUpscale.h"
#include "core/Http.h"
#include "core/Task.h"
#include "core/translation/LocalMt.h"
#include "core/translation/Onnx.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMutex>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>

#include <memory>

#ifdef Q_OS_ANDROID
#include <zlib.h>
#endif

namespace sc {

namespace {

// ONNX Runtime's release for each computer, checked against these.
struct Runtime {
    const char *asset;
    const char *sha256;
    const char *library;
    // Android: the library inside Microsoft's package (an AAR, a zip) on
    // Maven Central, fetched alone with a range: where its entry starts and
    // how long it is compressed. `sha256` is then the library's own.
    const char *entry = nullptr;
    qint64 offset = 0;
    qint64 compressed = 0;
};
constexpr auto kRuntimeVersion = "1.31.0";
Runtime runtimeBuild() {
    const auto cpu = QSysInfo::currentCpuArchitecture();
#if defined(Q_OS_ANDROID)
    // onnxruntime-android-1.31.0.aar (sha256 72683787…0540a7, 55 MB): only
    // this phone's library comes, 11 to 15 MB.
    if (cpu == QLatin1String("arm64"))
        return {"onnxruntime-android-1.31.0.aar", "8005a741e57117b4d7b476417706c0e9af13cf1b89db47e60f0cbc56e1af91e0", "lib/libonnxruntime.so",
                "jni/arm64-v8a/libonnxruntime.so", 353648, 12887720};
    if (cpu == QLatin1String("arm"))
        return {"onnxruntime-android-1.31.0.aar", "2a63f9a56b211511209dbf4b5bc143f18d0b365977dd7a0afd57d95c85479af6", "lib/libonnxruntime.so",
                "jni/armeabi-v7a/libonnxruntime.so", 13277471, 11639973};
    if (cpu == QLatin1String("x86_64"))
        return {"onnxruntime-android-1.31.0.aar", "068896846a7fe679cf780c1c532745286d9ef2e469f3dbde5bb9cf2c02095b18", "lib/libonnxruntime.so",
                "jni/x86_64/libonnxruntime.so", 40108685, 14993057};
#elif defined(Q_OS_WIN)
    if (cpu == QLatin1String("x86_64"))
        return {"onnxruntime-win-x64-1.31.0.zip", "3958d8a44984160692c1b4ffd0de3cb48f8c39f9d34bb31e433db06e30b82195", "lib/onnxruntime.dll"};
#elif defined(Q_OS_MACOS)
    if (cpu == QLatin1String("arm64"))
        return {"onnxruntime-osx-arm64-1.31.0.tgz", "1031b36dd3bdaa8d976a6130d9cf2a05e9be320b93ae804f8427b91dc3432afc", "lib/libonnxruntime.1.31.0.dylib"};
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    if (cpu == QLatin1String("x86_64"))
        return {"onnxruntime-linux-x64-1.31.0.tgz", "cc5c72baf5ae5c8238a6841f0897227be2d02826b9cf98eaf02fdefaeeb03a57", "lib/libonnxruntime.so.1.31.0"};
    if (cpu == QLatin1String("arm64"))
        return {"onnxruntime-linux-aarch64-1.31.0.tgz", "c5b8b3cca31f3d643a3b313b8f42e2d0f16a4fcefd02cda343d97bd5306afaab", "lib/libonnxruntime.so.1.31.0"};
#endif
    Q_UNUSED(cpu)
    return {nullptr, nullptr, nullptr};
}

// PaddleOCR's models (official ONNX exports): one finds text, one per
// script reads it. Sizes in MB.
struct Model {
    const char *name;
    const char *repo;
    const char *revision;
    const char *sha256;
    int size;
};
constexpr Model kDetector{"det", "PaddlePaddle/PP-OCRv5_mobile_det_onnx", "e6f4fa85f00e168c862bc462aebca69eef9b3d3d",
                          "a431985659dc921974177a95adcfbb90fd9e51989a5e04d70d0b75f597b6e61d", 5};
constexpr Model kReaders[]{
    {"korean", "PaddlePaddle/korean_PP-OCRv5_mobile_rec_onnx", "5c6f574b8e2230adf4287b33e736d71b9fabd28e",
     "92f0b7785e64fc9090106a241cf4c1eb97472824558272751b88a2a4476d3a08", 14},
    {"cjk", "PaddlePaddle/PP-OCRv5_mobile_rec_onnx", "ed152b8b495f84de93cda5709d768548a9127622",
     "da72dc72ca4dc220df0dfde68c1dedc31c58d3e76a25871122e5056227d50092", 17},
    {"latin", "PaddlePaddle/latin_PP-OCRv5_mobile_rec_onnx", "89d3a50e2c27e2e7cceeab0e944c25c807d5db4f",
     "7888113072263cb471b93f66dd5e2ad70548dc526fa1ace760d0d973dd121498", 8},
    {"eslav", "PaddlePaddle/eslav_PP-OCRv5_mobile_rec_onnx", "9a32171fc5718746875e1a261818884517975013",
     "b3018ef2b09a0250b6e0c8e871c927098363e5fd4df890cc68e8358eb0aaf1bd", 8},
    {"cyrillic", "PaddlePaddle/cyrillic_PP-OCRv5_mobile_rec_onnx", "2cef88145434beb8afa9dd82d77d799eb1ad7b29",
     "5371ee1ddaa7983cc62d0818d99e982b6804638c85e4f960d59a574094e172e5", 8},
    {"arabic", "PaddlePaddle/arabic_PP-OCRv5_mobile_rec_onnx", "14aaedcd75825982689ecf5cd64ab33ee083215a",
     "799113ebf267fbe742deb99eb36e8d42c9ddc5291ceacf92add41b4d52a59110", 8},
    {"thai", "PaddlePaddle/th_PP-OCRv5_mobile_rec_onnx", "1d4adbbafb1034a2fd6618498575b81ea7b69f69",
     "27618be66018f8598ac0a526a593f9f1cebf794e7eded93428e8fb016e537f5f", 8},
    {"devanagari", "PaddlePaddle/devanagari_PP-OCRv5_mobile_rec_onnx", "251aec19e36739540d35e2cc943f6aa7503b98e5",
     "cb789212ce96c69d3e74728ae4309d179281d68cb3945d0616b67cafab41c986", 8},
};

// waifu2x's CUNet "art" model, 2x with light noise removal (nunif, MIT),
// as deepghs exports it to ONNX.
constexpr auto kUpscalerPath = "20250502/onnx_models/cunet/art/noise1_scale2x.onnx";
constexpr Model kUpscaler{"waifu2x", "deepghs/waifu2x_onnx", "333b95cc88a6a9f39abb6426ab580f0d673f1185",
                          "8676098939edfb0fc7d7a40839ec650ba9412229f703a9eb04d8ed521938f824", 5};

// The script a language's comics are written in.
QString scriptOf(const QString &language) {
    static const QHash<QString, QString> scripts{
        {"ko", "korean"}, {"ja", "cjk"}, {"zh", "cjk"}, {"en", "cjk"}, {"ru", "eslav"}, {"uk", "eslav"}, {"bg", "cyrillic"},
        {"ar", "arabic"}, {"th", "thai"}, {"hi", "devanagari"}};
    static const QStringList latin{"es", "fr", "de", "it", "pt", "id", "tr", "pl", "nl", "vi", "af", "cs", "da", "et", "fi",
                                   "hu", "sv", "sw", "xh", "ro", "tl", "fil", "ms"};
    if (scripts.contains(language))
        return scripts.value(language);
    return latin.contains(language) ? QStringLiteral("latin") : QString();
}

const Model *reader(const QString &script) {
    for (const auto &m : kReaders)
        if (script == QLatin1String(m.name))
            return &m;
    return nullptr;
}

// Opus-MT models in ONNX (Xenova's exports of Helsinki-NLP's), each at a
// pinned commit. Sizes in MB (the quantized encoder and decoder, and the
// tokenizer).
struct Pair {
    const char *pair;
    const char *revision;
    int size;
};
constexpr Pair kPairs[]{
    {"af-en", "0eba4e40af86c6ad316a58fc9a7ed1134b05fd90", 110},
    {"ar-en", "09c7556866400dcc53562ba8d7035119e7d9a2c1", 117},
    {"cs-en", "c9f7336d1cf49ba5b7a412cba3b86999fa781716", 116},
    {"da-en", "6496d5e47a016552357be8f06294096609256ac4", 112},
    {"de-en", "399dfd68706739fffd503f876093e455ae268a06", 111},
    {"en-af", "3d48312617d092d77cb5aef9f9e9636ad639dd8d", 110},
    {"en-ar", "034a684356c19021a187cda4d7f823298b913921", 117},
    {"en-cs", "ed163ca109a93297e2f055eb3397bd33230141ad", 116},
    {"en-da", "33862dc071a5cc43e821e60742553c501c587e77", 112},
    {"en-de", "1ca130c44c4c5441ef16d48aae521a424ab644f7", 111},
    {"en-es", "4b002a4c7edd54a7ced58877258b87f7efd3f892", 119},
    {"en-fi", "4e5799273dbd7cce94c1e99cc9d3acb9f191b8eb", 119},
    {"en-fr", "28726206f80896b90035bd99cccd5cc1e151f916", 112},
    {"en-hi", "7aad72006a1588a9c3485ed6203aed99eebbc872", 116},
    {"en-hu", "1b7df958c1bd8f094eff1ce76d536468923ed7e3", 116},
    {"en-id", "b3b41f654c2fb3286d007dee1da7b0f47fd27d82", 107},
    {"en-it", "075406e3c8c2c30634d4a1bd8f00c21d9e162011", 136},
    {"en-nl", "c3a0541231a7329b4492809fc2bd7ab7d2ceb0bc", 121},
    {"en-ro", "3d816891778276930df2a45cbfb70689b3fe8981", 112},
    {"en-ru", "e050376e960175e8ddc5cff85025dc8436cddd68", 117},
    {"en-sv", "33d522d806c041cdebecd549c558d22a2b9b4ea4", 109},
    {"en-uk", "e6ca587292984b343fd829b6430ed55858496525", 116},
    {"en-vi", "30bcd461c0d7338ad106b36314d3eedc2d4e998c", 106},
    {"en-xh", "1ab9c4f4400ea4805f220ff5d551d3d0169b7a96", 114},
    {"en-zh", "046f55aec303cdee3e0318604406d4df20f1e8ea", 119},
    {"es-en", "eadfd7c658a9d8929ac3b8e996b68a68e2c7d480", 119},
    {"et-en", "58104952b730220eb7a9cdbbe19533a064f40dc0", 112},
    {"fi-en", "a623e4812687be788e3ab720c8a950ab64704793", 113},
    {"fr-en", "6b166a182780e118c997879d0ad5be4b53671644", 112},
    {"hi-en", "99853100ed9d932987a6816e8cf7b1b6742a6116", 115},
    {"hu-en", "c9e8a7617fb4cb4fca3785c5bb22cfe457cde00b", 116},
    {"id-en", "c38ef36c843b71177da903442835c3c870975fd6", 107},
    {"it-en", "fd0b89b9c052adc1f2f64152f555aa17353728be", 136},
    {"ja-en", "1a906cfaaf7c8f4193f67f5885c082aa6dbd9d16", 114},
    {"ko-en", "d9fa1ac6008242100fecb4fff9b0a5917a1be81f", 119},
    {"nl-en", "82c42d95d1508037cbe7172d85fd4e940a2f3584", 121},
    {"pl-en", "aeaf0e003b045248c28a559a5ce6027b54ebdaba", 117},
    {"ru-en", "afe8c6c738ec81b6d033fd8f44f9678a639a7c67", 117},
    {"sv-en", "63a40256869b6a2b29a84e47aa7f03524fa5f3a2", 109},
    {"th-en", "7f0406b972f742985cfbe3be8f61dead9f1bc0e3", 116},
    {"tr-en", "78247f41f354af036d24e6f0f8a3d02c6806f96e", 116},
    {"uk-en", "b2ad6afa424aaeff8913ff350860b7a7b7c0a607", 116},
    {"vi-en", "541dfd72ec07e8f1563795e0224408ad8d729e80", 106},
    {"xh-en", "f2950c1503ae17ba532ea858914ea13838bc2ab1", 114},
    {"zh-en", "39d480d52a9ea3065a1f117adfe4dbc55de10e6f", 119},
};

const Pair *pairOf(const QString &from, const QString &to) {
    const auto key = from + QLatin1Char('-') + to;
    for (const auto &p : kPairs)
        if (key == QLatin1String(p.pair))
            return &p;
    return nullptr;
}

// The models from `from` to `to`: one, or two through English.
QList<const Pair *> route(const QString &from, const QString &to) {
    if (from == to)
        return {};
    if (const auto *direct = pairOf(from, to))
        return {direct};
    const auto *first = pairOf(from, QStringLiteral("en"));
    const auto *second = pairOf(QStringLiteral("en"), to);
    if (first && second)
        return {first, second};
    return {nullptr};
}

bool routable(const QString &from, const QString &to) {
    const auto r = route(from, to);
    return !r.contains(nullptr) && (!r.isEmpty() || from == to);
}

QString base(const QString &language) { return language.section(QLatin1Char('_'), 0, 0).section(QLatin1Char('-'), 0, 0).toLower(); }

QString root() { return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/on-device"); }
QString runtimeDir() { return root() + QStringLiteral("/runtime"); }
QString ocrDir() { return root() + QStringLiteral("/ocr"); }
QString pairDir(const char *pair) { return root() + QStringLiteral("/mt/") + QLatin1String(pair); }

bool runtimeInstalled() {
    const auto build = runtimeBuild();
    QFile marker(runtimeDir() + QStringLiteral("/scancore-version.txt"));
    return build.library && QFileInfo::exists(runtimeDir() + QLatin1Char('/') + QLatin1String(build.library)) &&
           marker.open(QIODevice::ReadOnly) && marker.readAll().trimmed() == kRuntimeVersion;
}
QString upscalerFile() { return root() + QStringLiteral("/upscale/waifu2x.onnx"); }

bool detectorInstalled() { return QFileInfo::exists(ocrDir() + QStringLiteral("/det.onnx")); }
bool readerInstalled(const QString &script) {
    return QFileInfo::exists(ocrDir() + QLatin1Char('/') + script + QStringLiteral("/rec.onnx")) &&
           QFileInfo::exists(ocrDir() + QLatin1Char('/') + script + QStringLiteral("/chars.txt"));
}
bool pairInstalled(const Pair *p) { return p && QFileInfo::exists(pairDir(p->pair) + QStringLiteral("/decoder.onnx")); }

bool languageInstalled(const QString &language, const QString &target) {
    if (!runtimeInstalled() || !detectorInstalled() || !readerInstalled(scriptOf(language)))
        return false;
    for (const auto *p : route(language, target))
        if (!pairInstalled(p))
            return false;
    return true;
}

void report(const QString &status) { Task::report(QVariantMap{{"status", status}}); }

QByteArray fetch(const QString &url, qint64 maxBytes, const QString &what) {
    Request request;
    request.url = QUrl(url);
    request.timeoutMs = 900000;
    request.maxBytes = maxBytes;
    const auto response = Http::send(request);
    if (!response.ok())
        throw Error(QStringLiteral("%1 could not be downloaded (HTTP %2).").arg(what).arg(response.status), response.status);
    return response.body;
}

void save(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path + QStringLiteral(".part"));
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size())
        throw Error(QStringLiteral("Could not write to the app data folder."));
    out.close();
    QFile::remove(path);
    if (!QFile::rename(path + QStringLiteral(".part"), path))
        throw Error(QStringLiteral("Could not write to the app data folder."));
}

void checkSha(const QByteArray &data, const QString &expected) {
    if (QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() != expected.toLatin1())
        throw Error(QStringLiteral("The download was damaged. Try again."));
}

#ifdef Q_OS_ANDROID
// The one library out of the AAR: its zip entry, asked for with a range
// (the local header, then the deflated data), and inflated.
QByteArray androidLibrary(const Runtime &build) {
    Request request;
    request.url = QUrl(QStringLiteral("https://repo1.maven.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/%1/onnxruntime-android-%1.aar")
                           .arg(QLatin1String(kRuntimeVersion)));
    const qint64 headerRoom = 30 + 512;
    request.headers.append(QPair<QByteArray, QByteArray>(
        "Range", QStringLiteral("bytes=%1-%2").arg(build.offset).arg(build.offset + headerRoom + build.compressed - 1).toLatin1()));
    request.timeoutMs = 900000;
    request.maxBytes = headerRoom + build.compressed + 1024;
    const auto response = Http::send(request);
    if (response.status != 206)
        throw Error(QStringLiteral("ONNX Runtime could not be downloaded (HTTP %1).").arg(response.status), response.status);
    const auto &b = response.body;
    const auto u16 = [&](int at) { return int(uchar(b[at])) | int(uchar(b[at + 1])) << 8; };
    if (b.size() < 30 || uchar(b[0]) != 0x50 || uchar(b[1]) != 0x4b || uchar(b[2]) != 0x03 || uchar(b[3]) != 0x04 || u16(8) != 8)
        throw Error(QStringLiteral("The download was damaged. Try again."));
    const int name = u16(26), extra = u16(28);
    if (b.mid(30, name) != QByteArray(build.entry) || b.size() < 30 + name + extra + build.compressed)
        throw Error(QStringLiteral("The download was damaged. Try again."));
    const auto *data = reinterpret_cast<const Bytef *>(b.constData() + 30 + name + extra);
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
        throw Error(QStringLiteral("ONNX Runtime could not be unpacked."));
    QByteArray out;
    out.resize(48 * 1024 * 1024);
    z.next_in = const_cast<Bytef *>(data);
    z.avail_in = uInt(build.compressed);
    int status = Z_OK;
    while (status == Z_OK) {
        if (z.total_out >= uLong(out.size()))
            out.resize(out.size() * 2);
        z.next_out = reinterpret_cast<Bytef *>(out.data()) + z.total_out;
        z.avail_out = uInt(out.size() - z.total_out);
        status = inflate(&z, Z_NO_FLUSH);
    }
    const auto size = z.total_out;
    inflateEnd(&z);
    if (status != Z_STREAM_END)
        throw Error(QStringLiteral("ONNX Runtime could not be unpacked."));
    out.resize(qsizetype(size));
    return out;
}
#endif

void installRuntime() {
    const auto build = runtimeBuild();
    if (!build.asset)
        throw Error(QStringLiteral("Translating on the computer is not available for this computer."));
#ifdef Q_OS_ANDROID
    report(QStringLiteral("Downloading ONNX Runtime (about %1 MB)…").arg(build.compressed / (1024 * 1024)));
    const auto library = androidLibrary(build);
    checkSha(library, QLatin1String(build.sha256));
    const auto staging = runtimeDir() + QStringLiteral(".new");
    QDir(staging).removeRecursively();
    save(staging + QLatin1Char('/') + QLatin1String(build.library), library);
    save(staging + QStringLiteral("/scancore-version.txt"), kRuntimeVersion);
    QDir(runtimeDir()).removeRecursively();
    if (!QDir().rename(staging, runtimeDir()))
        throw Error(QStringLiteral("ONNX Runtime could not be installed."));
#else
    report(QStringLiteral("Downloading ONNX Runtime…"));
    const auto archive = fetch(QStringLiteral("https://github.com/microsoft/onnxruntime/releases/download/v%1/%2")
                                   .arg(QLatin1String(kRuntimeVersion), QLatin1String(build.asset)),
                               150LL * 1024 * 1024, QStringLiteral("ONNX Runtime"));
    checkSha(archive, QLatin1String(build.sha256));
    report(QStringLiteral("Unpacking…"));
    const auto staging = runtimeDir() + QStringLiteral(".new");
    QDir(staging).removeRecursively();
    // The libraries and the licences; not the headers or debug files.
    const auto keep = [](const QString &path) {
        return path == QLatin1String("LICENSE") || path == QLatin1String("ThirdPartyNotices.txt") ||
               (path.startsWith(QLatin1String("lib/")) && !path.startsWith(QLatin1String("lib/cmake")) && !path.startsWith(QLatin1String("lib/pkgconfig")) &&
                !path.endsWith(QLatin1String(".pdb")) && !path.endsWith(QLatin1String(".lib")));
    };
    if (QByteArray(build.asset).endsWith(".zip"))
        archive::unzip(archive, staging, keep);
    else
        archive::untarGz(archive, staging, keep);
    save(staging + QStringLiteral("/scancore-version.txt"), kRuntimeVersion);
    QDir(runtimeDir()).removeRecursively();
    if (!QDir().rename(staging, runtimeDir()))
        throw Error(QStringLiteral("ONNX Runtime could not be installed."));
#endif
}

QString hf(const char *repo, const char *revision, const QString &path) {
    return QStringLiteral("https://huggingface.co/%1/resolve/%2/%3").arg(QLatin1String(repo), QLatin1String(revision), path);
}

void installModel(const Model &model, const QString &dir, bool characters) {
    report(QStringLiteral("Downloading the text reader (%1 MB)…").arg(model.size));
    const auto onnx = fetch(hf(model.repo, model.revision, QStringLiteral("inference.onnx")), 100LL * 1024 * 1024, QStringLiteral("The text reader"));
    checkSha(onnx, QLatin1String(model.sha256));
    if (characters) {
        // The characters it reads, from its settings, one a line.
        const auto yaml = fetch(hf(model.repo, model.revision, QStringLiteral("inference.yml")), 8LL * 1024 * 1024, QStringLiteral("The text reader"));
        const auto list = translation::charactersFromYaml(yaml);
        if (list.size() < 10)
            throw Error(QStringLiteral("The text reader's characters could not be read."));
        save(dir + QStringLiteral("/chars.txt"), list.join(QLatin1Char('\n')).toUtf8() + '\n');
    }
    save(dir + QLatin1Char('/') + (characters ? QStringLiteral("rec.onnx") : QStringLiteral("det.onnx")), onnx);
}

void installPair(const Pair &pair) {
    report(QStringLiteral("Downloading the %1 model (%2 MB)…").arg(QLatin1String(pair.pair)).arg(pair.size));
    const auto repo = QStringLiteral("Xenova/opus-mt-") + QLatin1String(pair.pair);
    const auto repoUtf8 = repo.toUtf8();
    // The large files' sums, as the pinned revision lists them.
    QHash<QString, QString> sums;
    const auto listing = Http::getJson(QUrl(QStringLiteral("https://huggingface.co/api/models/%1/revision/%2?blobs=true").arg(repo, QLatin1String(pair.revision))), {}, 30000);
    for (const auto &v : listing.object().value(QStringLiteral("siblings")).toArray()) {
        const auto o = v.toObject();
        const auto sha = o.value(QStringLiteral("lfs")).toObject().value(QStringLiteral("sha256")).toString();
        if (!sha.isEmpty())
            sums.insert(o.value(QStringLiteral("rfilename")).toString(), sha);
    }
    const auto dir = pairDir(pair.pair);
    const QList<QPair<QString, QString>> files{{"config.json", "config.json"},
                                               {"tokenizer.json", "tokenizer.json"},
                                               {"onnx/encoder_model_quantized.onnx", "encoder.onnx"},
                                               {"onnx/decoder_model_quantized.onnx", "decoder.onnx"}};
    for (const auto &[from, to] : files) {
        const auto data = fetch(hf(repoUtf8.constData(), pair.revision, from), 400LL * 1024 * 1024, QStringLiteral("The translation model"));
        if (from.endsWith(QLatin1String(".onnx"))) {
            if (!sums.contains(from))
                throw Error(QStringLiteral("The translation model could not be checked."));
            checkSha(data, sums.value(from));
        }
        // The decoder last: a model is there when its decoder is.
        save(dir + QLatin1Char('/') + to, data);
    }
}

// Models in use, loaded once (a second or so each).
QMutex g_mutex;
QHash<QString, std::shared_ptr<translation::LocalOcr>> g_readers;
QHash<QString, std::shared_ptr<translation::Marian>> g_models;

bool loadRuntime() {
    if (onnx::loaded())
        return true;
    const auto build = runtimeBuild();
    if (!build.library || !runtimeInstalled())
        return false;
    QString error;
    if (!onnx::load(runtimeDir() + QLatin1Char('/') + QLatin1String(build.library), &error))
        throw Error(QStringLiteral("ONNX Runtime could not start: %1").arg(error));
    return true;
}

std::shared_ptr<translation::LocalOcr> readerFor(const QString &script) {
    QMutexLocker lock(&g_mutex);
    if (auto found = g_readers.value(script))
        return found;
    if (!loadRuntime() || !detectorInstalled() || !readerInstalled(script))
        throw Error(QStringLiteral("Download this language in Settings → Reading → Translate on this computer."));
    const auto dir = ocrDir() + QLatin1Char('/') + script;
    auto made = std::make_shared<translation::LocalOcr>(ocrDir() + QStringLiteral("/det.onnx"), dir + QStringLiteral("/rec.onnx"), dir + QStringLiteral("/chars.txt"));
    g_readers.insert(script, made);
    return made;
}

std::shared_ptr<translation::Waifu2x> g_upscaler;

std::shared_ptr<translation::Waifu2x> upscalerNow() {
    QMutexLocker lock(&g_mutex);
    if (g_upscaler)
        return g_upscaler;
    if (!loadRuntime() || !QFileInfo::exists(upscalerFile()))
        throw Error(QStringLiteral("The upscaler is not installed."));
    g_upscaler = std::make_shared<translation::Waifu2x>(upscalerFile());
    return g_upscaler;
}

std::shared_ptr<translation::Marian> modelFor(const Pair *pair) {
    QMutexLocker lock(&g_mutex);
    const auto key = QLatin1String(pair->pair);
    if (auto found = g_models.value(key))
        return found;
    if (!loadRuntime() || !pairInstalled(pair))
        throw Error(QStringLiteral("Download this language in Settings → Reading → Translate on this computer."));
    auto made = std::make_shared<translation::Marian>(pairDir(pair->pair));
    g_models.insert(key, made);
    return made;
}

// The language a text is in, from its script, among those installed.
QString guess(const QString &text, const QString &target) {
    int hangul = 0, kana = 0, han = 0, cyrillic = 0, arabic = 0, thai = 0, devanagari = 0, latin = 0;
    for (const auto c : text) {
        const auto u = c.unicode();
        if ((u >= 0xAC00 && u <= 0xD7AF) || (u >= 0x1100 && u <= 0x11FF) || (u >= 0x3130 && u <= 0x318F)) ++hangul;
        else if (u >= 0x3040 && u <= 0x30FF) ++kana;
        else if (u >= 0x4E00 && u <= 0x9FFF) ++han;
        else if (u >= 0x0400 && u <= 0x04FF) ++cyrillic;
        else if (u >= 0x0600 && u <= 0x06FF) ++arabic;
        else if (u >= 0x0E00 && u <= 0x0E7F) ++thai;
        else if (u >= 0x0900 && u <= 0x097F) ++devanagari;
        else if (c.isLetter() && u < 0x0250) ++latin;
    }
    if (hangul) return QStringLiteral("ko");
    if (kana) return QStringLiteral("ja");
    if (han) return languageInstalled(QStringLiteral("ja"), target) && !languageInstalled(QStringLiteral("zh"), target) ? QStringLiteral("ja") : QStringLiteral("zh");
    if (cyrillic) return languageInstalled(QStringLiteral("uk"), target) && !languageInstalled(QStringLiteral("ru"), target) ? QStringLiteral("uk") : QStringLiteral("ru");
    if (arabic) return QStringLiteral("ar");
    if (thai) return QStringLiteral("th");
    if (devanagari) return QStringLiteral("hi");
    if (latin) {
        // A Latin-script language that is installed (the first), else English.
        for (const auto &p : kPairs) {
            const auto from = QString::fromLatin1(p.pair).section(QLatin1Char('-'), 0, 0);
            if (scriptOf(from) == QLatin1String("latin") && languageInstalled(from, target))
                return from;
        }
        return QStringLiteral("en");
    }
    return {};
}

} // namespace

OnDevice *OnDevice::instance() {
    static auto *self = [] {
        auto *created = new OnDevice;
        QJSEngine::setObjectOwnership(created, QJSEngine::CppOwnership);
        return created;
    }();
    return self;
}

OnDevice::OnDevice(QObject *parent) : QObject(parent) {}

bool OnDevice::available() const { return runtimeBuild().asset != nullptr; }

QVariantList OnDevice::languages(const QString &target) const {
    const auto to = base(target);
    QStringList sources;
    for (const auto &p : kPairs) {
        const auto from = QString::fromLatin1(p.pair).section(QLatin1Char('-'), 0, 0);
        if (!sources.contains(from))
            sources.append(from);
    }
    QVariantList out;
    for (const auto &from : std::as_const(sources)) {
        const auto script = scriptOf(from);
        if (from == to || script.isEmpty() || !routable(from, to))
            continue;
        const auto r = route(from, to);
        int size = 0;
        for (const auto *p : r)
            size += pairInstalled(p) ? 0 : p->size;
        if (!readerInstalled(script))
            size += reader(script)->size;
        if (!detectorInstalled())
            size += kDetector.size;
        if (!runtimeInstalled())
            size += 30;
        const QLocale locale(from);
        out.append(QVariantMap{{"language", from},
                               {"label", QLocale::languageToString(locale.language())},
                               {"size", size},
                               {"installed", languageInstalled(from, to)},
                               {"through", r.size() > 1}});
    }
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        // Comics' own languages first, then by name.
        static const QStringList first{"ko", "ja", "zh"};
        const auto la = a.toMap().value("language").toString(), lb = b.toMap().value("language").toString();
        const auto fa = first.indexOf(la), fb = first.indexOf(lb);
        if ((fa >= 0) != (fb >= 0))
            return fa >= 0;
        if (fa >= 0)
            return fa < fb;
        return a.toMap().value("label").toString() < b.toMap().value("label").toString();
    });
    return out;
}

Task *OnDevice::install(const QString &language, const QString &target) {
    const auto from = base(language), to = base(target);
    auto *task = Task::run([from, to]() -> QVariant {
        const auto script = scriptOf(from);
        if (script.isEmpty() || !routable(from, to))
            throw Error(QStringLiteral("This language cannot be translated on the computer yet."));
        if (!runtimeInstalled())
            installRuntime();
        if (!detectorInstalled())
            installModel(kDetector, ocrDir(), false);
        if (!readerInstalled(script))
            installModel(*reader(script), ocrDir() + QLatin1Char('/') + script, true);
        for (const auto *p : route(from, to))
            if (!pairInstalled(p))
                installPair(*p);
        return true;
    });
    task->then(this, [this](const QVariant &) {
        ++m_revision;
        emit changed();
    });
    return task;
}

void OnDevice::remove(const QString &language, const QString &target) {
    const auto from = base(language), to = base(target);
    // What the other installed languages still need stays.
    QSet<QString> needed;
    QSet<QString> scripts;
    for (const auto &v : languages(to)) {
        const auto other = v.toMap().value("language").toString();
        if (other == from || !v.toMap().value("installed").toBool())
            continue;
        scripts.insert(scriptOf(other));
        for (const auto *p : route(other, to))
            needed.insert(QLatin1String(p->pair));
    }
    {
        QMutexLocker lock(&g_mutex);
        for (const auto *p : route(from, to)) {
            if (needed.contains(QLatin1String(p->pair)))
                continue;
            g_models.remove(QLatin1String(p->pair));
            QDir(pairDir(p->pair)).removeRecursively();
        }
        if (!scripts.contains(scriptOf(from))) {
            g_readers.remove(scriptOf(from));
            QDir(ocrDir() + QLatin1Char('/') + scriptOf(from)).removeRecursively();
        }
    }
    ++m_revision;
    emit changed();
}

bool OnDevice::upscalerInstalled() const { return runtimeInstalled() && QFileInfo::exists(upscalerFile()); }

Task *OnDevice::installUpscaler() {
    auto *task = Task::run([]() -> QVariant {
        if (!runtimeInstalled())
            installRuntime();
        if (!QFileInfo::exists(upscalerFile())) {
            report(QStringLiteral("Downloading the upscaler (%1 MB)…").arg(kUpscaler.size));
            const auto data = fetch(hf(kUpscaler.repo, kUpscaler.revision, QLatin1String(kUpscalerPath)), 50LL * 1024 * 1024,
                                    QStringLiteral("The upscaler"));
            checkSha(data, QLatin1String(kUpscaler.sha256));
            save(upscalerFile(), data);
        }
        // Started once now: a runtime this device cannot load says so here,
        // not later in every download.
        upscalerNow();
        return true;
    });
    task->then(this, [this](const QVariant &) {
        ++m_revision;
        emit changed();
    });
    return task;
}

QImage OnDevice::upscale(const QImage &image) const { return upscalerNow()->upscale(image); }

bool OnDevice::any() const {
    for (const auto &r : kReaders)
        if (readerInstalled(QLatin1String(r.name)))
            return runtimeInstalled();
    return false;
}

bool OnDevice::canRead(const QString &language) const {
    if (!runtimeInstalled() || !detectorInstalled())
        return false;
    if (language.isEmpty())
        return any();
    return readerInstalled(scriptOf(base(language)));
}

QList<translation::TextLine> OnDevice::read(const QImage &image, const QString &language) const {
    QString script = scriptOf(base(language));
    if (language.isEmpty() || !readerInstalled(script)) {
        // Not said, or not installed: the first script that is.
        script.clear();
        for (const auto &r : kReaders)
            if (readerInstalled(QLatin1String(r.name))) {
                script = QLatin1String(r.name);
                break;
            }
    }
    if (script.isEmpty())
        throw Error(QStringLiteral("Download a language in Settings → Reading → Translate on this computer."));
    return readerFor(script)->lines(image);
}

bool OnDevice::canTranslate(const QString &from, const QString &to) const {
    if (from.isEmpty())
        return any();
    return languageInstalled(base(from), base(to));
}

QStringList OnDevice::translate(const QStringList &texts, const QString &from, const QString &to) const {
    const auto target = base(to);
    QStringList out;
    for (const auto &text : texts) {
        const auto source = from.isEmpty() ? guess(text, target) : base(from);
        auto now = text;
        if (!source.isEmpty() && source != target) {
            const auto r = route(source, target);
            if (r.contains(nullptr))
                throw Error(QStringLiteral("This language cannot be translated on the computer yet."));
            for (const auto *p : r)
                now = modelFor(p)->translate(now);
        }
        out.append(now);
    }
    return out;
}

} // namespace sc
