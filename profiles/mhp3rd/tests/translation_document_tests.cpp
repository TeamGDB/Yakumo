// The Android document adapter over a synthetic provider backed by a local fd.
#include "platform/android_documents.hpp"
#include "platform/android_jni.hpp"
#include "text/language.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
std::filesystem::path document;
bool cancelled = false;
void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
}
namespace mhp3rd::android {
std::optional<std::string> pick_document() {
    return cancelled ? std::nullopt : std::optional<std::string>{"content://test/document"};
}
std::optional<std::string> pick_folder() {
    return std::nullopt;
}
std::string tree_root(const std::string &) {
    return {};
}
std::optional<std::vector<Entry>> list_folder(const std::string &) {
    return std::nullopt;
}
std::optional<std::string> create(const std::string &, const std::string &, bool) {
    return std::nullopt;
}
int open_document(const std::string &, const char *) {
    return ::open(document.c_str(), O_RDONLY);
}
}
int main() {
    namespace fs = std::filesystem;
    using mhp3rd::android::pick_translation_to_import;
    const auto dir = fs::temp_directory_path() /
        ("yakumo-translation-document-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{dir};
    try {
        const auto staging = dir / "transfer";
        cancelled = true;
        require(!pick_translation_to_import(staging) && !fs::exists(staging), "cancel creates no staging folder");
        cancelled = false;
        auto failed = pick_translation_to_import(staging);
        require(
            failed && !failed->error.empty() && failed->staged.empty(), "unreadable provider document fails cleanly");
        document = dir / "provider.lang";
        {
            std::ofstream out(document);
            out << "language = test\n2:1 = Text\n";
        }
        auto picked = pick_translation_to_import(staging);
        require(picked && picked->error.empty() && fs::is_regular_file(picked->staged), "a document is staged locally");
        std::ifstream in(picked->staged, std::ios::binary);
        const std::string contents(std::istreambuf_iterator<char>{in}, {});
        require(contents == "language = test\n2:1 = Text\n", "provider bytes are preserved");
        auto imported = mhp3rd::text::import_translation_file(picked->staged, dir / "translations");
        require(
            imported.error.empty() && imported.code == "test", "staged documents use the regular validated importer");
        fs::remove_all(picked->staged.parent_path());
        {
            std::ofstream out(document, std::ios::binary);
            out.seekp(mhp3rd::text::kMaxTranslationBytes);
            out << 'x';
        }
        auto oversized = pick_translation_to_import(staging);
        require(oversized && !oversized->error.empty() && oversized->staged.empty(),
            "provider copies respect the byte budget");
        require(fs::is_empty(staging), "failed copies leave no staged files");
        {
            std::ofstream out(dir / "blocked");
            out << "file";
        }
        auto blocked = pick_translation_to_import(dir / "blocked");
        require(blocked && !blocked->error.empty(), "unwritable staging is reported");
        std::cout << "translation document tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
