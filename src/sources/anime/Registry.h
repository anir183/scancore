#pragma once

#include "extensions/AnimeSource.h"

#include <QList>

#include <memory>

namespace sc::anime {

// The anime sources of installed Aniyomi extensions, and the reader's own
// Jellyfin servers. Lookups return shared
// pointers so a rebuild can swap the set while requests run.
class Registry {
public:
    static void rebuild();
    static QList<std::shared_ptr<ext::AnimeSource>> all();
    static std::shared_ptr<ext::AnimeSource> byKey(const QString &key);
    // The source a wrapped link ("aniyomi:<id>:<link>") names.
    static std::shared_ptr<ext::AnimeSource> forUrl(const QString &url);
};

} // namespace sc::anime
