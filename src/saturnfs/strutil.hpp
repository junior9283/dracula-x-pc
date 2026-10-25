#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace saturnfs {

inline std::string to_lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

}  // namespace saturnfs
