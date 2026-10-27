#pragma once

#include "Package.h"

#include <QByteArray>
#include <QUrl>

namespace sc::ext {

// One downloaded repository document, whichever of the formats it is:
//  - Mihon/Aniyomi legacy repositories: index.min.json (a list of
//    extensions) next to repo.json ({meta: {name, signingKeyFingerprint}});
//  - Mihon/Aniyomi extension stores: one JSON or protobuf message with the
//    store's name, signing key and either its extensions or a link to them;
//  - LNReader repositories: a JSON list of plugins;
//  - CloudStream repositories: repo.json ({name, pluginLists: [...]}) and
//    the plugins.json lists it links to;
//  - Echo extension lists: [{id, name, iconUrl, updateUrl}].
struct IndexDocument {
    enum class Type { Unknown, LegacyIndex, LegacyRepo, Store, ExtensionList, PluginList, CloudStreamRepo };
    Type type = Type::Unknown;
    // Name, key and links, when the document has them.
    std::optional<Repository> repo;
    // A document to read next: repo.json for a legacy index, index_v2 or a
    // store's extension list.
    QString follow;
    QList<Package> packages;
    bool hasPackages = false;
};

// `from` resolves relative links; `hint` is the kind a link scheme gave
// (aniyomi://…), else it is told by the extensions' lib versions.
IndexDocument parseIndex(QByteArray body, const QUrl &from, std::optional<Kind> hint = std::nullopt);

// Mihon's lib versions are 1.x, Aniyomi's 12 and above.
Kind kindForLib(const QString &lib);

} // namespace sc::ext
