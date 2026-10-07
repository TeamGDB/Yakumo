#pragma once

// Saves through Android's document picker (#17). A folder the player picks
// is a tree of content:// documents, not a path, so the save screen works on
// local copies: an import copies the save folders it can find in the picked
// folder into a staging folder and checks them there as on any platform; an
// export or a backup is written to a staging folder and then copied into the
// picked one. Only in the Android app (MHP3RD_ANDROID_APP).

#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace mhp3rd::android {

struct PickedImport {
    std::filesystem::path staged; // the local copy to look for saves in
    std::string error;            // empty: the copy worked
};
// Asks for a folder, then copies the save folders (those holding a PARAM.SFO)
// found in it into <staging>/<its name>/SAVEDATA, where find_saves() looks:
// the folder itself, its SAVEDATA and PSP folders and two levels of other
// folders, so a memory stick's root, PPSSPP's PSP folder or an export all
// work. `staging` is emptied first. Nothing when the player cancels.
[[nodiscard]] std::optional<PickedImport> pick_saves_to_import(const std::filesystem::path &staging);

// A picked .lang document is copied to a new private staging directory with
// the translation input budget. The caller imports it, then removes its parent.
[[nodiscard]] std::optional<PickedImport> pick_translation_to_import(const std::filesystem::path &staging);

// Copy only a user-selected unpacked folder into private staging. The regular
// texture/mod importer validates it before installing anything. Names, cycles,
// depth, entry count and aggregate bytes are bounded; failures remove the copy.
struct FolderImportLimits {
    std::uint64_t bytes{8ULL << 30};
    std::size_t entries{100000};
    unsigned depth{32};
};
using FolderImportProgress = std::function<bool(std::uint64_t)>;
[[nodiscard]] std::optional<PickedImport> pick_folder_to_import(const std::filesystem::path &staging,
    const FolderImportProgress &progress = {}, const FolderImportLimits &limits = {});

// Picker and provider I/O run away from the drawing thread. Cancelling waits
// for an outstanding picker/provider call to return, then discards staging.
class FolderImport {
public:
    FolderImport();
    ~FolderImport();
    FolderImport(const FolderImport &) = delete;
    FolderImport &operator=(const FolderImport &) = delete;
    void start(const std::filesystem::path &staging);
    void cancel();
    [[nodiscard]] bool ready() const;
    [[nodiscard]] std::uint64_t bytes() const;
    [[nodiscard]] std::optional<PickedImport> take();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct PickedExport {
    std::string where; // the picked folder, for the player
    std::string error; // empty: the copy worked
};
// Asks for a folder and copies `local` into it as a folder of the same name,
// with everything in it. Nothing when the player cancels.
[[nodiscard]] std::optional<PickedExport> pick_folder_and_copy(const std::filesystem::path &local);

} // namespace mhp3rd::android
