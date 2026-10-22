// The program's text translation hook (declared in client/content.h).
#include "client/content.h"

namespace client {

std::function<std::string(const std::string&)>& text_translation() {
    static std::function<std::string(const std::string&)> f;
    return f;
}

std::string tr(const std::string& text) {
    const auto& f = text_translation();
    return f ? f(text) : text;
}

} // namespace client
