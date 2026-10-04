#include "ui/translation_screen.hpp"

#include "text/language.hpp"
#include "text/translation.hpp"

#include "install/user_data.hpp"
#include "ui/file_browser.hpp"
#include "ui/layer.hpp"
#include "ui/save_screen.hpp"
#include "ui/widgets.hpp"
#if defined(MHP3RD_ANDROID_APP)
#include "platform/android_documents.hpp"
#endif

#include "imgui.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <system_error>
#include <utility>

namespace mhp3rd::ui {
namespace {

namespace fs = std::filesystem;

enum class Stage { Closed, Choose, Result };

struct State {
    Stage stage{Stage::Closed};
    std::unique_ptr<FileBrowser> browser;
    fs::path last_folder;
    text::TranslationImport result;
    bool focus_row{};
};

State &state() {
    static State s;
    return s;
}

TextLanguages g_languages;
bool g_languages_loaded = false;

float px(float value) {
    return std::round(value * Layer::get().scale());
}

std::string utf8(const fs::path &path) {
    return install::path_to_utf8(path);
}

void indented(const std::string &text, ImU32 color = colors::kTextDim) {
    ImGui::Indent(px(16.0f));
    paragraph(text, color);
    ImGui::Unindent(px(16.0f));
}

fs::path translations_folder() {
    return install::user_data_directory() / "translations";
}

void go(Stage stage) {
    State &s = state();
    s.stage = stage;
    ImGui::SetScrollY(0.0f);
}

void close() {
    State &s = state();
    s.stage = Stage::Closed;
    s.browser.reset();
    s.focus_row = true;
}

void start_import(const fs::path &chosen);

void open_browser() {
    State &s = state();
#if defined(MHP3RD_ANDROID_APP)
    const auto picked = android::pick_translation_to_import(install::user_data_directory() / "transfer");
    if (!picked) {
        close();
        return;
    }
    if (!picked->error.empty()) {
        s.result = {};
        s.result.error = picked->error;
        go(Stage::Result);
        return;
    }
    start_import(picked->staged);
    std::error_code ec;
    fs::remove_all(picked->staged.parent_path(), ec);
#else
    FileBrowser::Options options;
    options.extensions = {".lang"};
    options.filter_name = ".lang";
    options.listed_name = ".lang files";
    options.empty_note = "No .lang files here.";
    fs::path start = s.last_folder;
    if (start.empty()) {
        start = FileBrowser::home() / "Downloads";
        std::error_code ec;
        if (!fs::is_directory(start, ec)) start = FileBrowser::home();
    }
    s.browser = std::make_unique<FileBrowser>(start, std::move(options));
    go(Stage::Choose);
#endif
}

void start_import(const fs::path &chosen) {
    State &s = state();
    s.result = text::import_translation_file(chosen, translations_folder());
    if (s.result.error.empty()) {
        refresh_text_languages();
        std::cout << "[text] imported " << utf8(s.result.saved) << " (" << s.result.code << ")" << std::endl;
    } else {
        std::cout << "[text] import failed: " << s.result.error << std::endl;
    }
    go(Stage::Result);
}

bool browse(bool back) {
    State &s = state();
    indented("Import: choose a translation file (.lang) you downloaded. It is copied into Yakumo's translations "
             "folder, then appears under Game text language above.");
#if !defined(MHP3RD_ANDROID_APP)
    indented("You can also drop the file on the window.", colors::kTextDim);
#endif
    // A file dropped on the window is imported at once.
    if (auto dropped = Layer::get().take_dropped_file()) {
        s.last_folder = s.browser->folder();
        s.browser.reset();
        start_import(*dropped);
        return true;
    }
    const FileBrowser::Result result = s.browser->frame(back);
    if (result == FileBrowser::Result::Browsing) return true;
    s.last_folder = s.browser->folder();
    const fs::path chosen = s.browser->chosen();
    s.browser.reset();
    if (result == FileBrowser::Result::Cancelled) {
        close();
        return false;
    }
    start_import(chosen);
    return true;
}

void result_screen(bool back) {
    State &s = state();
    if (back) {
        close();
        return;
    }
    section("Import translation");
    if (s.result.error.empty()) {
        info_row("Imported", s.result.name.empty() ? s.result.code : s.result.name + " (" + s.result.code + ")");
        indented("Choose it under Game text language above, then restart the game to load it.");
    } else {
        paragraph("Not imported: " + s.result.error, colors::kDanger);
    }
    if (button_row("Done", {false, {}, "Back to the Text section."})) close();
}

} // namespace

const TextLanguages &text_languages() {
    if (!g_languages_loaded) {
        g_languages = {};
        g_languages.codes.emplace_back("original");
        g_languages.names.emplace_back("Original");
        for (const text::Language &language : text::languages()) {
            g_languages.codes.push_back(language.code);
            g_languages.names.push_back(language.name);
        }
        g_languages_loaded = true;
    }
    return g_languages;
}

void refresh_text_languages() {
    g_languages_loaded = false;
}

void translation_rows() {
    State &s = state();
    if (s.focus_row) {
        focus_next_row();
        s.focus_row = false;
    }
    if (button_row("Import translation…",
            {false, {},
                "Copy a translation file (.lang) you downloaded into Yakumo. It appears under Game text "
                "language; a restart loads it."}))
        open_browser();
#if !defined(MHP3RD_ANDROID_APP)
    if (button_row("Open the translations folder", {false, {}, "Show where imported translations are kept."}))
        open_folder(translations_folder());
#endif
}

bool translation_screen_open() {
    return state().stage != Stage::Closed;
}

bool translation_screen(bool back) {
    State &s = state();
    switch (s.stage) {
    case Stage::Closed:
        return false;
    case Stage::Choose:
        return browse(back);
    case Stage::Result:
        result_screen(back);
        break;
    }
    return true;
}

} // namespace mhp3rd::ui
