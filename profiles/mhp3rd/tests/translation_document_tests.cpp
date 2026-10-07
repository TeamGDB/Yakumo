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
#include <map>
#include <thread>
#include <stdexcept>

namespace {
std::filesystem::path document;
bool cancelled = false;
std::optional<std::string> selected_tree;
std::map<std::string, std::vector<mhp3rd::android::Entry>> folders;
std::map<std::string, std::filesystem::path> files;
std::optional<std::string> folder_name = "my-pack";
void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
}
namespace mhp3rd::android {
std::optional<std::string> pick_document() {
    return cancelled ? std::nullopt : std::optional<std::string>{"content://test/document"};
}
std::optional<std::string> pick_folder() {
    return selected_tree;
}
std::string tree_root(const std::string &) {
    return "root";
}
std::optional<std::string> document_name(const std::string &) {
    return folder_name;
}
std::optional<std::vector<Entry>> list_folder(const std::string &uri) {
    const auto found = folders.find(uri);
    return found == folders.end() ? std::nullopt : std::optional{found->second};
}
std::optional<std::string> create(const std::string &, const std::string &, bool) {
    return std::nullopt;
}
int open_document(const std::string &uri, const char *) {
    const auto found = files.find(uri);
    return ::open((found == files.end() ? document : found->second).c_str(), O_RDONLY);
}
}

namespace {
void folder_tests(const std::filesystem::path &dir) {
    namespace fs = std::filesystem;
    using namespace mhp3rd::android;
    const auto staging = dir / "folders";
    require(!pick_folder_to_import(staging) && !fs::exists(staging), "cancelled picker creates nothing");
    selected_tree = "content://test/tree/primary%3ADownload%2Fmy-pack";
    auto denied = pick_folder_to_import(staging);
    require(denied && denied->staged.empty() && !denied->error.empty(), "denied tree is reported and removed");
    const auto provider = dir / "provider-image";
    std::ofstream(provider, std::ios::binary) << "public synthetic bytes";
    files["image"] = provider;
    folders["root"] = {{true, "日本語", "nested"}, {false, "textures.ini", "image"}};
    folders["nested"] = {{false, "тест.png", "image"}};
    std::vector<std::uint64_t> progress;
    auto picked = pick_folder_to_import(staging, [&](std::uint64_t bytes) {
        progress.push_back(bytes);
        return true;
    });
    require(picked && picked->error.empty() && picked->staged.filename() == "my-pack", "picked name is retained");
    std::ifstream in(picked->staged / "日本語" / "тест.png", std::ios::binary);
    require(
        std::string(std::istreambuf_iterator<char>{in}, {}) == "public synthetic bytes", "nested Unicode bytes copied");
    require(progress.back() == 2 * fs::file_size(provider), "progress reports aggregate bytes");
    fs::remove_all(picked->staged.parent_path());

    const auto failed = [&](const FolderImportLimits &limits = {}) {
        const auto result = pick_folder_to_import(staging, {}, limits);
        require(result && !result->error.empty() && result->staged.empty(), "invalid provider tree fails");
        require(fs::is_empty(staging), "failure removes only staging");
    };
    for (const std::string &name :
        std::vector<std::string>{"../escape", "/absolute", "..", ".", "a\\b", "a:b", "", std::string("a\0b", 3)}) {
        folders["root"] = {{false, name, "image"}};
        failed();
    }
    require(!fs::exists(dir / "escape"), "provider names cannot escape staging");
    folders["root"] = {{false, "duplicate", "image"}, {false, "duplicate", "image"}};
    failed();
    folders["root"] = {{true, "cycle", "root"}};
    failed();
    folders["root"] = {{true, "denied", "missing"}};
    failed();
    folders["root"] = {{false, "missing", "unreadable"}};
    files["unreadable"] = dir / "missing";
    failed();
    folders["root"] = {{false, "invalid", ""}};
    failed();
    folders["root"] = {{true, "nested", "nested"}};
    failed({1000, 10, 0});
    failed({1000, 1, 32});
    folders["root"] = {{false, "image", "image"}};
    const auto size = fs::file_size(provider);
    failed({size - 1, 10, 32});
    auto exact = pick_folder_to_import(staging, {}, {size, 1, 0});
    require(exact && exact->error.empty(), "exact byte, count and depth limits succeed");
    fs::remove_all(exact->staged.parent_path());
    auto cancelled_copy = pick_folder_to_import(staging, [](std::uint64_t bytes) { return bytes == 0; });
    require(
        cancelled_copy && !cancelled_copy->error.empty() && fs::is_empty(staging), "copy cancellation cleans staging");
    auto cancelled_start = pick_folder_to_import(staging, [](std::uint64_t) { return false; });
    require(cancelled_start && !cancelled_start->error.empty() && fs::is_empty(staging), "cancel before I/O is safe");
    std::ofstream(dir / "blocked-folder") << "unchanged";
    auto blocked = pick_folder_to_import(dir / "blocked-folder");
    require(blocked && !blocked->error.empty(), "unwritable staging reports an error");
    folder_name = "..";
    failed();
    folder_name.reset();
    failed();
    folder_name = "my-pack";
    selected_tree = "content://test/tree/primary%3ADownload%2Fmy-pack";
    folders["root"] = {{false, "image", "image"}};
    FolderImport job;
    require(!job.ready() && job.bytes() == 0, "worker starts idle");
    job.start(staging);
    bool refused = false;
    try {
        job.start(staging);
    } catch (const std::logic_error &) {
        refused = true;
    }
    require(refused, "running worker cannot be replaced");
    const auto wait = [](FolderImport &work) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!work.ready() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        require(work.ready(), "folder worker completes within a bound");
    };
    wait(job);
    auto async_result = job.take();
    require(async_result && async_result->error.empty() && job.bytes() == size, "worker stages provider bytes");
    fs::remove_all(async_result->staged.parent_path());
    job.start(staging);
    job.cancel();
    wait(job);
    auto cancelled_result = job.take();
    // A very small provider can finish before cancel; ownership still transfers
    // to the caller, which removes it just as the UI does on a late cancel.
    if (cancelled_result && !cancelled_result->staged.empty()) fs::remove_all(cancelled_result->staged.parent_path());
    require(fs::is_empty(staging), "late cancellation leaves no temporary copy");
    {
        FolderImport owned;
        owned.start(staging);
        wait(owned);
    }
    require(fs::is_empty(staging), "worker destruction cleans unclaimed staging");
    selected_tree.reset();
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
        folder_tests(dir);
        std::cout << "translation document tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
