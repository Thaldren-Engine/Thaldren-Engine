// The screenshot key's file: the next free number in a folder (docs/RE_MEMORY.md, "Client",
// "Screenshots").

#include "client/front.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "render/presenter.h"

namespace client {

std::string save_numbered_picture(const std::string& dir, const std::string& prefix, const render::Image8& frame,
                                  const render::Palette& palette) {
    // The folder is made when it is missing; the number is one more than the largest found after
    // the prefix in the folder's "<prefix>*.pcx" names (the C runtime's atoi of what follows the
    // prefix; none: 1), written with four digits.
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!dir.empty()) fs::create_directories(fs::path(dir), ec);
    int highest = 0;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string want = lower(prefix);
    for (fs::directory_iterator it(dir.empty() ? fs::path(".") : fs::path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = lower(it->path().filename().string());
        if (name.size() < want.size() + 4 || name.compare(0, want.size(), want) != 0 || name.compare(name.size() - 4, 4, ".pcx") != 0)
            continue;
        const int n = std::atoi(name.c_str() + want.size());
        if (n > highest) highest = n;
    }
    char num[16];
    std::snprintf(num, sizeof num, "%04i", highest + 1);
    std::string path = dir;
    if (!path.empty() && path.back() != '\\' && path.back() != '/') path += '/';
    path += prefix + num + ".pcx";
    return render::write_pcx(path, frame, palette) ? path : std::string();
}

}  // namespace client
