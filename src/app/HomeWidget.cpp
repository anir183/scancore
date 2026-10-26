#include "HomeWidget.h"

#include "Library.h"
#include "core/Database.h"
#include "core/ImageCache.h"
#include "core/Task.h"
#include "sources/comics/Registry.h"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

namespace sc {

namespace {

constexpr int kRows = 3;

// A cover small enough for the widget, saved beside widget.json.
QString coverFile(const QVariantMap &entry, const QString &dir, int index) {
    const auto url = entry.value(QStringLiteral("cover")).toString();
    if (url.isEmpty())
        return {};
    QByteArray bytes;
    try {
        bytes = ImageCache::fetch(url, QString());
    } catch (const Error &) {
    }
    if (bytes.isEmpty()) {
        // Some sites want their own page as the referer.
        if (const auto source = comics::Registry::byKey(entry.value(QStringLiteral("source")).toString())) {
            try {
                bytes = ImageCache::fetch(url, source->imageReferer());
            } catch (const Error &) {
            }
        }
    }
    QImage image;
    if (bytes.isEmpty() || !image.loadFromData(bytes))
        return {};
    image = image.scaled(120, 170, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const auto path = dir + QStringLiteral("/cover-%1.jpg").arg(index);
    return image.save(path, "JPG", 85) ? path : QString();
}

QString detailOf(const QVariantMap &entry) {
    const bool anime = entry.value(QStringLiteral("media")).toString() == QLatin1String("anime");
    const auto number = entry.value(QStringLiteral("unitNumber")).toString();
    QString detail;
    if (!number.isEmpty() && number != QLatin1String("0"))
        detail = (anime ? QCoreApplication::translate("HomeWidget", "Episode %1") : QCoreApplication::translate("HomeWidget", "Chapter %1")).arg(number);
    else
        detail = entry.value(QStringLiteral("unitTitle")).toString();
    const int percent = qRound(entry.value(QStringLiteral("percent")).toDouble() * (entry.value(QStringLiteral("percent")).toDouble() <= 1.0 ? 100 : 1));
    if (percent > 0 && percent < 100)
        detail += QStringLiteral(" · %1%").arg(percent);
    return detail;
}

} // namespace

HomeWidget *HomeWidget::instance() {
    static auto *widget = new HomeWidget(QCoreApplication::instance());
    return widget;
}

HomeWidget::HomeWidget(QObject *parent) : QObject(parent) {
    m_later.setSingleShot(true);
    m_later.setInterval(3000);
    connect(&m_later, &QTimer::timeout, this, &HomeWidget::write);
}

void HomeWidget::start() {
#ifdef Q_OS_ANDROID
    connect(Library::instance(), &Library::changed, &m_later, qOverload<>(&QTimer::start));
    // Leaving the app: what was read last is on the widget at once.
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive && m_later.isActive()) {
            m_later.stop();
            write();
        }
    });
    m_later.start();
#endif
}

void HomeWidget::write() {
    if (m_writing) {
        m_again = true;
        return;
    }
    m_writing = true;
    const QJsonObject labels{{"title", tr("Continue")},
                             {"empty", tr("Nothing yet: what you read and watch shows here.")},
                             {"open", tr("Open SCANCORE")}};
    const int updates = Library::instance()->unreadUpdates();
    const auto updatesText = updates == 1 ? tr("1 new chapter") : updates > 1 ? tr("%1 new chapters").arg(updates) : QString();
    auto *task = Library::instance()->continueItems(QString(), kRows);
    // Never handed to QML: deleted here once it answers.
    connect(task, &Task::finished, this, [this, task, labels, updatesText](const QVariant &result) {
        task->deleteLater();
        const auto list = result.toList();
        runInBackground([this, list, labels, updatesText] {
            const auto dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/widget");
            QDir().mkpath(dir);
            QJsonArray rows;
            for (int i = 0; i < list.size() && i < kRows; ++i) {
                const auto entry = list.at(i).toMap();
                rows.append(QJsonObject{{"id", entry.value(QStringLiteral("id")).toString()},
                                        {"title", entry.value(QStringLiteral("title")).toString()},
                                        {"detail", detailOf(entry)},
                                        {"media", entry.value(QStringLiteral("media")).toString()},
                                        {"cover", coverFile(entry, dir, i)}});
            }
            QJsonObject root = labels;
            root.insert(QStringLiteral("updates"), updatesText);
            root.insert(QStringLiteral("rows"), rows);
            QSaveFile file(dir + QStringLiteral("/widget.json"));
            if (file.open(QIODevice::WriteOnly)) {
                file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
                file.commit();
            }
            QMetaObject::invokeMethod(this, [this] {
#ifdef Q_OS_ANDROID
                QJniObject::callStaticMethod<void>("com/mediumdev/scancore/HomeWidget", "refresh", "(Landroid/content/Context;)V",
                                                   QNativeInterface::QAndroidApplication::context().object());
#endif
                m_writing = false;
                if (m_again) {
                    m_again = false;
                    m_later.start();
                }
            });
        });
    });
    connect(task, &Task::failed, this, [this, task] {
        task->deleteLater();
        m_writing = false;
    });
}

} // namespace sc
