// Reading a GPU driver package's meta.json and choosing its main library; no
// Android and no game data needed.
#include "platform/driver_meta.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace mhp3rd::android;

int failures{};

void check(bool condition, const char *message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void test_reads_a_flat_package_description() {
    const std::string meta = R"({
  "schemaVersion": 1,
  "name": "Mesa Turnip v26.3.0",
  "description": "A \"quoted\" word and the name of nothing",
  "libraryName": "vulkan.adreno.so"
})";
    check(json_string_field(meta, "name") == "Mesa Turnip v26.3.0", "name is read");
    check(json_string_field(meta, "libraryName") == "vulkan.adreno.so", "libraryName is read, not mistaken for name");
    check(json_string_field(meta, "description") == "A \"quoted\" word and the name of nothing",
        "an escaped quote is kept as the quote");
}

void test_missing_or_non_string_fields_are_empty() {
    check(json_string_field("{}", "name").empty(), "a missing field is empty");
    check(json_string_field("", "name").empty(), "no text at all is empty");
    check(json_string_field(R"({"name": 3})", "name").empty(), "a number is not a string");
    check(json_string_field(R"({"name")", "name").empty(), "a key with no value is empty");
    check(json_string_field(R"({"name": "cut)", "name") == "cut", "an unterminated string yields what it has");
}

void test_a_key_inside_a_value_is_not_the_field() {
    const std::string meta = R"({"description": "the \"name\" is below", "name": "Real"})";
    check(json_string_field(meta, "name") == "Real", "a quoted word inside a value does not count as the key");
}

void test_whitespace_around_the_colon() {
    check(json_string_field("{\"name\"\n  :\t\"Spaced\"}", "name") == "Spaced", "whitespace around the colon is fine");
}

void test_chooses_the_main_library() {
    const std::vector<std::string> several{"libgsl.so", "libvulkan_freedreno.so", "other.so"};
    check(choose_main_library(several, "other.so") == "other.so", "libraryName wins over the heuristic");
    check(choose_main_library(several, "") == "libvulkan_freedreno.so", "else the one naming vulkan");
    check(choose_main_library(several, "missing.so") == "libvulkan_freedreno.so",
        "a libraryName the package does not hold is ignored");
    check(choose_main_library({"a.so", "b.so"}, "") == "a.so", "else the first");
    check(choose_main_library({"only.so"}, "") == "only.so", "a single library is the main one");
    check(choose_main_library({"lib_vulkan_one.so", "vulkan_two.so"}, "") == "lib_vulkan_one.so",
        "among several naming vulkan, the first");
    check(choose_main_library({}, "x.so").empty(), "no library, no main one");
}
} // namespace

int main() {
    test_reads_a_flat_package_description();
    test_missing_or_non_string_fields_are_empty();
    test_a_key_inside_a_value_is_not_the_field();
    test_whitespace_around_the_colon();
    test_chooses_the_main_library();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "driver meta tests passed\n";
    return 0;
}
