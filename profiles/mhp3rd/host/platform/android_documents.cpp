#include "platform/android_documents.hpp"

#include "platform/android_jni.hpp"
#include "text/language.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <limits>
#include <fstream>
#include <future>
#include <set>
#include <vector>

namespace mhp3rd::android {
namespace {

namespace fs = std::filesystem;

// A readable name for a picked tree: the last part of its document id
// ("primary:Download/saves" gives "Download/saves").
std::string describe_tree(const std::string &uri) {
    std::string decoded;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            decoded += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            decoded += uri[i];
        }
    }
    const std::size_t colon = decoded.rfind(':');
    return colon == std::string::npos ? decoded : decoded.substr(colon + 1);
}

bool copy_document_to_file(const std::string &uri, const fs::path &target, std::string &error,
    std::size_t limit = std::numeric_limits<std::size_t>::max(), const FolderImportProgress &progress = {}) {
    const int fd = open_document(uri, "r");
    if (fd < 0) {
        error = "cannot read " + target.filename().string();
        return false;
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    std::array<char, 1 << 16> buffer{};
    bool ok = static_cast<bool>(out) && (!progress || progress(0));
    std::size_t copied = 0;
    while (ok) {
        const ssize_t got = ::read(fd, buffer.data(), buffer.size());
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            ok = got == 0;
            break;
        }
        if (static_cast<std::size_t>(got) > limit - copied) {
            error = "the chosen files exceed the import size limit";
            ok = false;
            break;
        }
        copied += static_cast<std::size_t>(got);
        out.write(buffer.data(), got);
        ok = static_cast<bool>(out);
        if (ok && progress && !progress(copied)) {
            error = "Import cancelled.";
            ok = false;
        }
    }
    ::close(fd);
    out.close();
    ok = ok && static_cast<bool>(out);
    if (!ok && error.empty()) error = "cannot copy " + target.filename().string();
    return ok;
}

bool copy_file_to_document(const fs::path &source, const std::string &folder_uri, std::string &error) {
    const std::optional<std::string> created = create(folder_uri, source.filename().string(), false);
    if (!created) {
        error = "cannot create " + source.filename().string() + " in the chosen folder";
        return false;
    }
    const int fd = open_document(*created, "w");
    if (fd < 0) {
        error = "cannot write " + source.filename().string();
        return false;
    }
    std::ifstream in(source, std::ios::binary);
    std::array<char, 1 << 16> buffer{};
    bool ok = static_cast<bool>(in);
    while (ok && in) {
        in.read(buffer.data(), buffer.size());
        std::streamsize left = in.gcount();
        const char *at = buffer.data();
        while (left > 0) {
            const ssize_t put = ::write(fd, at, static_cast<std::size_t>(left));
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) {
                ok = false;
                break;
            }
            at += put;
            left -= put;
        }
    }
    ok = ::close(fd) == 0 && ok;
    if (!ok) error = "cannot write " + source.filename().string();
    return ok;
}

// Copies the files of a folder document (not its subfolders) into `target`.
bool copy_save_folder(const std::string &uri, const fs::path &target, std::string &error) {
    const auto entries = list_folder(uri);
    if (!entries) {
        error = "cannot read the chosen folder";
        return false;
    }
    std::error_code ec;
    fs::create_directories(target, ec);
    for (const Entry &entry : *entries)
        if (!entry.directory && !copy_document_to_file(entry.uri, target / entry.name, error)) return false;
    return true;
}

bool holds_param_sfo(const std::vector<Entry> &entries) {
    for (const Entry &entry : entries)
        if (!entry.directory && entry.name == "PARAM.SFO") return true;
    return false;
}

// Looks for save folders (those holding a PARAM.SFO) under a folder document
// and copies each into `savedata`/<name>: in the folder itself, its SAVEDATA
// and PSP folders, and one level of other folders, so a memory stick's root,
// an export ("MHP3rd saves <time>/PSP/SAVEDATA") or a folder holding one all
// work. The first save of a name wins.
bool find_and_copy(
    const std::string &uri, const std::string &name, const fs::path &savedata, int depth, std::string &error) {
    const auto entries = list_folder(uri);
    if (!entries) return true;
    if (holds_param_sfo(*entries)) {
        std::error_code ec;
        if (fs::exists(savedata / name, ec)) return true;
        return copy_save_folder(uri, savedata / name, error);
    }
    if (depth >= 5) return true;
    for (const Entry &entry : *entries) {
        if (!entry.directory) continue;
        const bool obvious = entry.name == "PSP" || entry.name == "SAVEDATA" || name == "SAVEDATA";
        if ((obvious || depth < 2) && !find_and_copy(entry.uri, entry.name, savedata, depth + 1, error)) return false;
    }
    return true;
}

bool copy_tree(const fs::path &local, const std::string &folder_uri, std::string &error) {
    const std::optional<std::string> made = create(folder_uri, local.filename().string(), true);
    if (!made) {
        error = "cannot create " + local.filename().string() + " in the chosen folder";
        return false;
    }
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(local, ec)) {
        if (entry.is_directory(ec)) {
            if (!copy_tree(entry.path(), *made, error)) return false;
        } else if (!copy_file_to_document(entry.path(), *made, error)) {
            return false;
        }
    }
    if (ec) error = "cannot read " + local.string();
    return !ec;
}

bool safe_name(const std::string &name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string::npos &&
        name.find('\0') == std::string::npos;
}

struct FolderCopy {
    const FolderImportLimits &limits;
    const FolderImportProgress &progress;
    std::uint64_t bytes{};
    std::size_t entries{};
    std::set<std::string> visited;
    std::string error;

    bool active() {
        if (!progress || progress(bytes)) return true;
        error = "Import cancelled.";
        return false;
    }

    bool copy(const std::string &uri, const fs::path &target, unsigned depth) {
        if (!active()) return false;
        if (depth > limits.depth || !visited.insert(uri).second) {
            error = "The chosen folder is too deeply nested or contains a document cycle.";
            return false;
        }
        const auto children = list_folder(uri);
        if (!children) {
            error = "Android would not let Yakumo read that folder.";
            return false;
        }
        for (const Entry &entry : *children) {
            if (!active()) return false;
            if (++entries > limits.entries || !safe_name(entry.name) || entry.uri.empty()) {
                error = "The chosen folder contains too many entries or an unsafe file name.";
                return false;
            }
            const fs::path path = target / entry.name;
            // A fresh, private staging directory is required. Refuse duplicate
            // names rather than letting a provider overwrite an earlier entry.
            if (fs::exists(path)) {
                error = "The chosen folder contains duplicate file names.";
                return false;
            }
            if (entry.directory) {
                fs::create_directory(path);
                if (!copy(entry.uri, path, depth + 1)) return false;
            } else {
                const std::uint64_t before = bytes;
                const auto remaining = static_cast<std::size_t>(
                    std::min<std::uint64_t>(limits.bytes - bytes, std::numeric_limits<std::size_t>::max()));
                if (!copy_document_to_file(entry.uri, path, error, remaining, [&](std::uint64_t done) {
                        bytes = before + done;
                        return active();
                    }))
                    return false;
            }
        }
        return true;
    }
};

} // namespace

std::optional<PickedImport> pick_folder_to_import(
    const fs::path &staging, const FolderImportProgress &progress, const FolderImportLimits &limits) {
    const auto tree = pick_folder();
    if (!tree) return std::nullopt;
    PickedImport result;
    fs::path owned;
    try {
        if (progress && !progress(0)) {
            result.error = "Import cancelled.";
            return result;
        }
        fs::create_directories(staging);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const auto candidate = staging / ("folder-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (fs::create_directory(candidate)) {
                owned = candidate;
                break;
            }
        }
        if (owned.empty()) throw std::runtime_error("Cannot create a transfer folder.");
        const std::string root_uri = tree_root(*tree);
        const auto display_name = document_name(root_uri);
        if (!display_name) throw std::runtime_error("Android would not provide the selected folder's name.");
        const std::string &name = *display_name;
        if (!safe_name(name)) throw std::runtime_error("The selected folder has an unsafe name.");
        result.staged = owned / name;
        fs::create_directory(result.staged);
        FolderCopy copy{limits, progress, 0, 0, {}, {}};
        if (!copy.copy(root_uri, result.staged, 0)) result.error = copy.error;
    } catch (const std::exception &error) {
        result.error = "Copying the selected folder failed: " + std::string(error.what());
    }
    if (!result.error.empty()) {
        std::error_code ec;
        if (!owned.empty()) fs::remove_all(owned, ec);
        result.staged.clear();
    }
    return result;
}

struct FolderImport::Impl {
    std::atomic_bool cancelled{};
    std::atomic<std::uint64_t> bytes{};
    std::future<std::optional<PickedImport>> work;
};

FolderImport::FolderImport() : impl_(std::make_unique<Impl>()) {}

FolderImport::~FolderImport() {
    cancel();
    if (impl_->work.valid()) {
        const auto result = impl_->work.get();
        if (result && !result->staged.empty()) {
            std::error_code ec;
            fs::remove_all(result->staged.parent_path(), ec);
        }
    }
}

void FolderImport::start(const fs::path &staging) {
    if (impl_->work.valid()) throw std::logic_error("A folder import is already running.");
    impl_->cancelled = false;
    impl_->bytes = 0;
    impl_->work = std::async(std::launch::async, [this, staging] {
        return pick_folder_to_import(staging, [this](std::uint64_t bytes) {
            impl_->bytes = bytes;
            return !impl_->cancelled;
        });
    });
}

void FolderImport::cancel() {
    impl_->cancelled = true;
}

bool FolderImport::ready() const {
    return impl_->work.valid() && impl_->work.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

std::uint64_t FolderImport::bytes() const {
    return impl_->bytes;
}

std::optional<PickedImport> FolderImport::take() {
    return impl_->work.get();
}

std::optional<PickedImport> pick_translation_to_import(const fs::path &staging) {
    const auto document = pick_document();
    if (!document) return std::nullopt;
    PickedImport result;
    std::error_code ec;
    fs::create_directories(staging, ec);
    if (ec) {
        result.error = "cannot create translation transfer folder";
        return result;
    }
    fs::path folder;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        auto candidate = staging / ("translation-" + std::to_string(stamp) + "-" + std::to_string(attempt));
        if (fs::create_directory(candidate, ec)) {
            folder = std::move(candidate);
            break;
        }
        if (ec) break;
    }
    if (folder.empty()) {
        result.error = "cannot stage translation document";
        return result;
    }
    result.staged = folder / "selected.lang";
    if (!copy_document_to_file(*document, result.staged, result.error, text::kMaxTranslationBytes)) {
        fs::remove_all(folder, ec);
        result.staged.clear();
    }
    return result;
}

std::optional<PickedImport> pick_saves_to_import(const fs::path &staging) {
    const std::optional<std::string> tree = pick_folder();
    if (!tree) return std::nullopt;
    PickedImport picked;
    std::error_code ec;
    fs::remove_all(staging, ec);
    const std::string where = describe_tree(*tree);
    // The staged copy carries the picked folder's name, which the review
    // screen shows; the saves go into its SAVEDATA, where find_saves() looks.
    const fs::path root = staging / where.substr(where.rfind('/') + 1);
    picked.staged = root;
    fs::create_directories(root / "SAVEDATA", ec);
    const std::string root_uri = tree_root(*tree);
    if (!list_folder(root_uri)) {
        picked.error = "Android would not let Yakumo read that folder.";
        return picked;
    }
    std::string error;
    const std::string root_name = where.substr(where.rfind('/') + 1);
    if (!find_and_copy(root_uri, root_name, root / "SAVEDATA", 0, error))
        picked.error = "Copying from the chosen folder failed: " + error + ".";
    return picked;
}

std::optional<PickedExport> pick_folder_and_copy(const fs::path &local) {
    const std::optional<std::string> tree = pick_folder();
    if (!tree) return std::nullopt;
    PickedExport result;
    result.where = describe_tree(*tree);
    std::string error;
    if (!copy_tree(local, tree_root(*tree), error)) result.error = error;
    return result;
}

} // namespace mhp3rd::android
