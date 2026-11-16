#pragma once
#include "dav.h"

namespace gauge::saves {
class Migration {
public:
    static Migration inspect(const fs::path& legacy, const fs::path& root, const Catalog& current);
    const Json& summary() const { return summary_; }
    Json apply(const fs::path& root, Catalog& current) const;
private:
    struct Entry {
        fs::path source, base, destination;
        Json record;
        bool ready{};
    };
    Catalog catalog_;
    std::vector<fs::path> sources_;
    std::optional<DavConfig> cloud_;
    std::vector<Entry> entries_;
    Json summary_;
};
}
