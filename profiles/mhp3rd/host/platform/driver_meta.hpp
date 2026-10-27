#pragma once

// Reading a GPU driver package's meta.json (libadrenotools' tools/ADPKG.md:
// a flat object with schemaVersion, name, description, ..., libraryName).
// Nothing here depends on Android, so it is tested on every platform.

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace mhp3rd::android {

// The string value of `key` in a flat JSON object, or empty when it is
// missing or not a string. Not a JSON parser: meta.json has no nesting, so
// the first `"key"` followed by a colon and a quote is the field.
[[nodiscard]] inline std::string json_string_field(const std::string &text, const std::string &key) {
    const std::string quoted = "\"" + key + "\"";
    for (std::size_t at = text.find(quoted); at != std::string::npos; at = text.find(quoted, at + 1u)) {
        std::size_t value = text.find_first_not_of(" \t\r\n", at + quoted.size());
        if (value == std::string::npos || text[value] != ':') continue;
        value = text.find_first_not_of(" \t\r\n", value + 1u);
        if (value == std::string::npos || text[value] != '"') return {};
        std::string result;
        for (++value; value < text.size() && text[value] != '"'; ++value) {
            if (text[value] == '\\' && value + 1u < text.size()) ++value; // keep the escaped character
            result += text[value];
        }
        return result;
    }
    return {};
}

// The main driver among the .so files a package holds: the one meta.json's
// "libraryName" names; failing that the only one, or, among several, the
// first whose name holds "vulkan"; failing that the first. Empty when there
// is none.
[[nodiscard]] inline std::string choose_main_library(
    const std::vector<std::string> &libraries, const std::string &library_name) {
    if (libraries.empty()) return {};
    if (std::find(libraries.begin(), libraries.end(), library_name) != libraries.end()) return library_name;
    if (libraries.size() > 1u)
        for (const std::string &name : libraries)
            if (name.find("vulkan") != std::string::npos) return name;
    return libraries.front();
}

} // namespace mhp3rd::android
