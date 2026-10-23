// Checks for mod support that need no game data: reading mod folders in the
// community's format, the choices and what they add up to (priority,
// conflicts, packs), importing, the DATA.BIN obfuscation, a synthetic archive
// served with replaced, grown and patched files, and the patch format.
//
//   mhp3rd_mods_tests
//   mhp3rd_mods_tests --check-disc <image.iso>
//
// The second form reads the real DATA.BIN directory from a disc image (read
// only) and checks that it parses and encodes back to the same bytes, and that
// a layout with a grown entry re-keys the entries after it consistently.
#include "mods/file_overlay.hpp"
#include "mods/mhp3rd_code_mods.hpp"
#include "mods/mhp3rd_data_bin.hpp"
#include "mods/mhp3rd_mod_format.hpp"
#include "mods/mod_import.hpp"
#include "mods/mod_ini.hpp"
#include "mods/mod_library.hpp"
#include "mods/mod_session.hpp"

#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace mhp3rd::mods;
namespace fs = std::filesystem;
namespace p3 = mhp3rd::mods::p3rd;
using Bytes = std::vector<std::uint8_t>;

namespace {

int failures = 0;

void check(bool condition, const std::string &what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition) ++failures;
}

void write(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

void write(const fs::path &path, const Bytes &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void put32(Bytes &bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
}

void append32(Bytes &bytes, std::uint32_t value) {
    bytes.resize(bytes.size() + 4u);
    put32(bytes, bytes.size() - 4u, value);
}

Bytes pattern(std::size_t size, std::uint32_t seed) {
    std::mt19937 random(seed);
    Bytes bytes(size);
    for (std::uint8_t &b : bytes) b = static_cast<std::uint8_t>(random());
    return bytes;
}

struct Scratch {
    fs::path root;
    Scratch() {
        root = fs::temp_directory_path() /
            ("yakumo_mods_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(root, ec); // only this test's own scratch folder
    }
};

void test_ini() {
    const IniFile ini = IniFile::parse("\xEF\xBB\xBF; comment\r\n[MOD INFO]\r\nName=\"A mod\"\r\nFiles=\"a;b; c;;\"\r\n"
                                       "ModList=\"x;y\r\nVersion=HD\r\n# another\r\n[sub]\r\nType = Patch \r\n");
    check(ini.get("mod info", "name") == "A mod", "ini: quoted value, section and key without case");
    check(split_list(ini.get("MOD INFO", "Files")) == std::vector<std::string>{"a", "b", "c"},
        "ini: list split and trimmed");
    check(ini.get("MOD INFO", "ModList") == "x;y", "ini: a quote left open runs to the end of the line");
    check(ini.get("MOD INFO", "Version") == "HD", "ini: unquoted value");
    check(ini.get("sub", "Type") == "Patch", "ini: spaces around = are trimmed");
    check(ini.find("MOD INFO", "missing") == nullptr, "ini: missing key");
}

// A mod folder with a mod.ini in the manager's format.
void make_mod(const fs::path &folder, const std::string &info, const std::vector<std::string> &files = {}) {
    write(folder / "mod.ini", "[MOD INFO]\r\n" + info);
    for (const std::string &file : files) write(folder / file, pattern(64, 7));
}

void test_format(const fs::path &root) {
    const p3::ModFolderFormat format(6043u);
    const fs::path mods = root / "format";
    make_mod(mods / "file_hd",
        "Name=\"File HD\"\nAuthor=\"Someone\"\nFiles=\"a.pac;B.PAC\"\nTarget=\"0601;05d2\"\n"
        "Type=\"File\"\nVersion=\"HD\"\nDescription=\"Line one.\\Line two.\"\n",
        {"a.pac", "b.pac"});
    std::optional<Mod> mod = format.read(mods / "file_hd");
    check(mod && mod->unusable.empty() && mod->changes.size() == 2u, "format: a file mod for the HD version");
    check(mod && mod->changes[1].file == 0x5D2 && mod->changes[1].kind == FileChange::Kind::Replace,
        "format: targets are hex file ids; file names match without case");
    check(mod && mod->description == "Line one.\nLine two." && mod->author == "Someone", "format: description lines");

    make_mod(mods / "psp", "Name=\"PSP\"\nFiles=\"a.pac\"\nTarget=\"0601\"\nType=\"File\"\n", {"a.pac"});
    mod = format.read(mods / "psp");
    check(mod && !mod->unusable.empty(), "format: no Version means the PSP version, which cannot be used");

    make_mod(mods / "both",
        "Name=\"Both\"\nFiles=\"a.pac\"\nTarget=\"0601\"\nFilesHD=\"h.pac\"\nTargetHD=\"0602\"\n"
        "Type=\"Patch\"\nVersion=\"BOTH\"\n",
        {"a.pac", "h.pac"});
    mod = format.read(mods / "both");
    check(mod && mod->unusable.empty() && mod->changes.size() == 1u && mod->changes[0].file == 0x602 &&
            mod->changes[0].kind == FileChange::Kind::Patch && mod->changes[0].source.filename() == "h.pac",
        "format: BOTH uses FilesHD and TargetHD");

    make_mod(
        mods / "count", "Files=\"a.pac;b.pac\"\nTarget=\"0601\"\nType=\"File\"\nVersion=\"HD\"\n", {"a.pac", "b.pac"});
    check(!format.read(mods / "count")->unusable.empty(), "format: files and targets must pair up");
    make_mod(mods / "missing", "Files=\"gone.pac\"\nTarget=\"0601\"\nType=\"File\"\nVersion=\"HD\"\n");
    check(!format.read(mods / "missing")->unusable.empty(), "format: a missing file makes the mod unusable");
    make_mod(mods / "range", "Files=\"a.pac\"\nTarget=\"FFFF\"\nType=\"File\"\nVersion=\"HD\"\n", {"a.pac"});
    check(!format.read(mods / "range")->unusable.empty(), "format: a target past the archive is refused");
    make_mod(
        mods / "code", "Name=\"Code\"\nFiles=\"c.bin;D.BIN\"\nType=\"Code\"\nVersion=\"HD\"\n", {"c.bin", "d.bin"});
    mod = format.read(mods / "code");
    check(mod && mod->unusable.empty() && mod->changes.empty() && mod->code.size() == 2u &&
            iequals(to_utf8(mod->code[1].filename()), "d.bin"),
        "format: a code mod's files, in order, found without case");
    make_mod(mods / "code_both", "Files=\"psp.bin\"\nFilesHD=\"hd.bin\"\nType=\"Code\"\nVersion=\"BOTH\"\n",
        {"psp.bin", "hd.bin"});
    mod = format.read(mods / "code_both");
    check(mod && mod->code.size() == 1u && mod->code[0].filename() == "hd.bin",
        "format: a code mod for both uses FilesHD");
    make_mod(mods / "code_psp", "Files=\"c.bin\"\nType=\"Code\"\n", {"c.bin"});
    check(!format.read(mods / "code_psp")->unusable.empty(), "format: a code mod without Version is for the PSP");
    make_mod(mods / "code_missing", "Files=\"gone.bin\"\nType=\"Code\"\nVersion=\"HD\"\n");
    check(!format.read(mods / "code_missing")->unusable.empty(), "format: a code mod's missing file is reported");
    make_mod(mods / "code_empty", "Type=\"Code\"\nVersion=\"HD\"\n");
    check(!format.read(mods / "code_empty")->unusable.empty(), "format: a code mod without files cannot be used");
    make_mod(mods / "pack", "Name=\"Pack\"\nType=\"Pack\"\nModList=\"file_hd;both\n");
    mod = format.read(mods / "pack");
    check(mod && mod->members == std::vector<std::string>{"file_hd", "both"}, "format: a pack lists its mods");
    make_mod(mods / "pack_depends", "Name=\"Pack\"\nType=\"Pack\"\nDepends=\"file_hd;both\"\n");
    mod = format.read(mods / "pack_depends");
    check(mod && mod->unusable.empty() && mod->members == std::vector<std::string>{"file_hd", "both"},
        "format: a pack that names its mods only in Depends");
    make_mod(mods / "added_file",
        "Files=\"a.pac\"\nTarget=\"1508Q\"\nType=\"File\"\nVersion=\"HD\"\nDepends=\"monster_expansion\"\n", {"a.pac"});
    mod = format.read(mods / "added_file");
    check(mod && mod->unusable.find("another mod adds (it needs monster_expansion)") != std::string::npos,
        "format: a target another mod adds says so");
    make_mod(mods / "pseudo",
        "Name=\"Pseudo\"\nType=\"PseudoPack\"\nVersion=\"HD\"\nSubModList=\"one;two;three\"\n"
        "[one]\nFiles=\"a.pac\"\nTarget=\"0010\"\nType=\"File\"\n[two]\nFiles=\"p.bin\"\n"
        "Target=\"0011\"\nType=\"Patch\"\n[three]\nFiles=\"c.bin\"\nType=\"Code\"\n[four]\nType=\"EquipGS\"\n",
        {"a.pac", "p.bin", "c.bin"});
    mod = format.read(mods / "pseudo");
    // SubModList names three; [four] is not listed and so not read.
    check(mod && mod->unusable.empty() && mod->changes.size() == 2u && mod->code.size() == 1u && mod->notes.empty(),
        "format: a pseudo pack takes its file, patch and code parts");
    make_mod(mods / "pseudo_equip",
        "Type=\"PseudoPack\"\nVersion=\"HD\"\nSubModList=\"one;two\"\n[one]\nFiles=\"c.bin\"\nType=\"Code\"\n"
        "[two]\nType=\"EquipGS\"\nFiles=\"c.bin\"\n",
        {"c.bin"});
    mod = format.read(mods / "pseudo_equip");
    check(mod && mod->unusable.empty() && mod->code.size() == 1u && mod->notes.size() == 1u,
        "format: a pseudo pack reports the parts it cannot take");
    make_mod(mods / "set", "Name=\"Set\"\nType=\"EquipSET\"\nFiles=\"h.pac;null;b.pac;null;l.pac\"\n",
        {"h.pac", "b.pac", "l.pac"});
    mod = format.read(mods / "set");
    check(
        mod && mod->slots.size() == 3u && mod->slots[1].label == "Body", "format: an armour set has a slot per piece");
    check(mod && mod->slots.size() == 3u && mod->slots[0].part == "HEAD" && mod->slots[1].part == "BODY" &&
            mod->slots[2].part == "LEGS",
        "format: a set's slots say which piece each stands in for, past the null ones");
    make_mod(mods / "weapon", "Name=\"GS\"\nType=\"EquipGS\"\nFiles=\"w.pac\"\nAnimation=\"w.json\"\n", {"w.pac"});
    mod = format.read(mods / "weapon");
    check(mod && mod->slots.size() == 1u && mod->type == "Great Sword" && !mod->notes.empty(),
        "format: a weapon model, its animations reported");
    check(mod && mod->slots.size() == 1u && mod->slots[0].part == "GS", "format: a weapon slot names its class");
    write(mods / "withpreview" / "PREVIEW.PNG", "x");
    make_mod(mods / "withpreview", "Name=\"P\"\nType=\"Pack\"\nModList=\"a\"\n");
    check(!format.read(mods / "withpreview")->preview.empty(), "format: the preview is found without case");

    // The author in a section of its own, as published armour sets give it,
    // and a dependency on the loader's file replacer, which Yakumo is.
    write(mods / "author_section" / "mod.ini",
        "[MOD INFO]\r\nName=\"Set\"\r\nFiles=\"h.pac;null;null;null;null\"\r\nType=\"EquipSET\"\r\nVersion=\"BOTH\"\r\n"
        "Depends=\"file_replacer\"\r\n\r\n[Author]\r\nName=\"Someone\"\r\n");
    write(mods / "author_section" / "h.pac", pattern(16, 3));
    mod = format.read(mods / "author_section");
    check(mod && mod->unusable.empty() && mod->author == "Someone", "format: the author from an [Author] section");
    check(mod && mod->depends == std::vector<std::string>{"file_replacer"} && format.built_in("file_replacer") &&
            format.built_in(" FILE_REPLACER") && !format.built_in("wpn_anim_expansion"),
        "format: the loader's file replacer is built in, other dependencies are not");
    make_mod(mods / "priority", "Name=\"P\"\nType=\"Pack\"\nModList=\"a\"\nPriority=\"0\"\n");
    mod = format.read(mods / "priority");
    check(mod && mod->priority == 0 && mod->notes.empty(), "format: Priority 0 to 5 is read");
    make_mod(mods / "bad_priority", "Name=\"P\"\nType=\"Pack\"\nModList=\"a\"\nPriority=\"9\"\n");
    mod = format.read(mods / "bad_priority");
    check(mod && mod->priority == 3 && mod->notes.size() == 1u, "format: a Priority out of range keeps 3, with a note");
    make_mod(mods / "no_priority", "Name=\"P\"\nType=\"Pack\"\nModList=\"a\"\n");
    check(format.read(mods / "no_priority")->priority == 3, "format: no Priority means 3");

    // mhp3reload's own files folder: files named by id, patches with P.
    write(mods / "raw" / "files" / "0601", pattern(16, 1));
    write(mods / "raw" / "files" / "0602P", pattern(16, 2));
    write(mods / "raw" / "files" / "readme.txt", "x");
    mod = format.read(mods / "raw");
    check(mod && mod->changes.size() == 2u && mod->changes[1].kind == FileChange::Kind::Patch,
        "format: a folder of files named by file id");
    fs::create_directories(mods / "empty");
    check(!format.read(mods / "empty"), "format: a folder with neither is not a mod");
    check(format.file_name(0x601) == "0601" && format.parse_file("0x5d2") == 0x5D2u, "format: file id spelling");
}

void test_library(const fs::path &root) {
    const p3::ModFolderFormat format(6043u);
    const fs::path mods = root / "library";
    make_mod(mods / "a", "Name=\"A\"\nFiles=\"x.pac;y.pac\"\nTarget=\"0001;0002\"\nType=\"File\"\nVersion=\"HD\"\n",
        {"x.pac", "y.pac"});
    make_mod(mods / "b", "Name=\"B\"\nFiles=\"x.pac\"\nTarget=\"0001\"\nType=\"File\"\nVersion=\"HD\"\n", {"x.pac"});
    make_mod(mods / "c",
        "Name=\"C\"\nFiles=\"p.bin\"\nTarget=\"0001\"\nType=\"Patch\"\nVersion=\"HD\"\n"
        "Depends=\"d\"\n",
        {"p.bin"});
    make_mod(mods / "d", "Name=\"D\"\nFiles=\"p.bin\"\nTarget=\"0003\"\nType=\"Patch\"\nVersion=\"HD\"\n", {"p.bin"});
    make_mod(mods / "pack", "Name=\"Pack\"\nType=\"Pack\"\nModList=\"a;b\"\n");
    // For the PSP version only: listed, but it cannot be turned on.
    make_mod(mods / "code", "Name=\"Code\"\nFiles=\"c.bin\"\nType=\"Code\"\nVersion=\"NOHD\"\n", {"c.bin"});

    ModLibrary library(format);
    library.load_choices(root / "none.ini");
    library.scan(mods);
    check(library.mods().size() == 6u, "library: every mod folder is read");
    check(library.resolve().empty(), "library: nothing is on at first");

    library.set_enabled("pack", true);
    check(library.enabled("a") && library.enabled("b"), "library: a pack turns its mods on");
    Resolution r = library.resolve();
    const std::string top = library.mods().front().id;
    // The mods seen first, in name order, get the lower ranks: b is above a.
    check(r.replacements.at(1).mod == "b" && r.replacements.at(2).mod == "a", "library: the higher mod wins a file");
    check(r.conflicts.size() == 1u && r.conflicts[0].file == 1u && r.conflicts[0].winner == "b" &&
            r.conflicts[0].overridden == std::vector<std::string>{"a"},
        "library: the conflict is reported");
    library.move("a", +1);
    r = library.resolve();
    check(r.replacements.at(1).mod == "a", "library: raising a mod makes it win");

    library.set_enabled("c", true);
    check(library.enabled("d"), "library: turning a mod on turns on what it depends on");
    r = library.resolve();
    check(r.patches.at(1).size() == 1u && r.patches.at(3).size() == 1u, "library: patches are collected");
    const auto conflict =
        std::find_if(r.conflicts.begin(), r.conflicts.end(), [](const auto &c) { return c.file == 1u; });
    check(conflict != r.conflicts.end() && conflict->patched_by == std::vector<std::string>{"c"},
        "library: a patch on another mod's replacement is shown");

    library.set_enabled("code", true);
    check(!library.enabled("code"), "library: a mod that cannot be used stays off");
    library.set_enabled("pack", false);
    check(!library.enabled("a") && !library.enabled("b"), "library: a pack turns its mods off");

    std::string error;
    library.set_enabled("b", true);
    check(library.save_choices(root / "mods.ini", error), "library: choices are saved");
    ModLibrary again(format);
    again.load_choices(root / "mods.ini");
    again.scan(mods);
    check(again.enabled("b") && !again.enabled("a") && again.resolve().same_files(library.resolve()),
        "library: saved choices come back");
    again.set_master(false);
    check(again.resolve().empty(), "library: the master switch turns every mod off");

    // A setting saved out of its range (by hand, or by an older version of
    // the mod) reads clamped both when declared and afterwards.
    again.set_setting("b", "speed", 50);
    again.set_setting("b", "mode", -3);
    const std::int64_t declared_speed =
        again.declare_setting("b", {"speed", "Speed", ModSetting::Kind::Number, 1, 10, 5, {}});
    const std::int64_t declared_mode =
        again.declare_setting("b", {"mode", "Mode", ModSetting::Kind::Choice, 0, 0, 1, {"Off", "Low", "High"}});
    check(declared_speed == 10 && again.declared_value("b", "speed") == 10 && declared_mode == 0 &&
            again.declared_value("b", "mode") == 0 && again.setting("b", "speed") == 50,
        "library: a setting reads clamped to its range when declared and when read later");
    check(again.declared_value("b", "undeclared", 7) == 7, "library: a key never declared reads its fallback");
    (void)top;

    // What only the start takes: a native library or a script turned on or
    // off needs a restart, as code does; a file does not.
    Resolution started;
    Resolution files_only = started;
    files_only.replacements[7u] = {"a", "x.bin", {}};
    Resolution with_native = started;
    with_native.native.push_back({"n", "lib.so", {}});
    Resolution with_script = started;
    with_script.scripts.push_back({"s", "main.luau", {}});
    Resolution with_code = started;
    with_code.code.push_back({"c", "code.bin", {}});
    check(files_only.same_start(started) && !with_native.same_start(started) && !with_script.same_start(started) &&
            !with_code.same_start(started),
        "library: a native library, a script or code differ in what only the start takes; a file does not");

    // The session applies at once when the game can take it.
    bool accept = true;
    int activations = 0;
    ModSession session(format, {mods, root / "session.ini", nullptr},
        {[&](const Resolution &) { return accept; }, [&](const Resolution &) { ++activations; }});
    session.start();
    session.library().set_enabled("b", true);
    session.commit();
    check(!session.restart_pending() && activations == 2, "session: a change the game can take applies now");
    accept = false;
    session.library().set_enabled("a", true);
    session.commit();
    check(session.restart_pending() && activations == 2, "session: one it cannot take waits for the restart");
    session.library().set_enabled("a", false);
    session.commit();
    check(!session.restart_pending(), "session: undoing it clears the restart");
}

void test_import(const fs::path &root) {
    const p3::ModFolderFormat format(6043u);
    const fs::path downloads = root / "downloads";
    const fs::path mods = root / "import_mods";
    make_mod(downloads / "single", "Name=\"One\"\nType=\"Pack\"\nModList=\"x\"\n");
    make_mod(downloads / "bundle" / "wrapper" / "m1", "Name=\"M1\"\nType=\"Pack\"\nModList=\"x\"\n");
    make_mod(downloads / "bundle" / "wrapper" / "m2", "Name=\"M2\"\nType=\"Pack\"\nModList=\"x\"\n");
    ImportCheck c = check_import(downloads / "single", format, mods);
    check(c.mods.size() == 1u && c.mods[0].id == "single", "import: a mod folder itself");
    c = check_import(downloads / "bundle", format, mods);
    check(c.mods.size() == 2u, "import: mods one folder further down");
    ImportResult result = import_mods(c, mods);
    check(result.imported.size() == 2u && fs::exists(mods / "m1" / "mod.ini"), "import: copied into the mods folder");
    write(mods / "m1" / "mine.txt", "kept");
    c = check_import(downloads / "bundle", format, mods);
    check(c.mods[0].replaces, "import: an installed mod of the same name is noticed");
    result = import_mods(c, mods);
    check(result.backups.size() == 2u && fs::exists(result.backups[0] / "mine.txt"),
        "import: the replaced mod moves to .backup, nothing is deleted");
    c = check_import(mods / "m1", format, mods);
    check(c.mods.empty() && !c.problem.empty(), "import: not from inside the mods folder");
    c = check_import(root / "nothing", format, mods);
    check(c.mods.empty(), "import: a folder that is not there");
}

void test_cipher() {
    const Bytes plain = pattern(10007, 3);
    Bytes data = plain;
    p3::encrypt(data, 0x12345u, 0u);
    check(data != plain, "cipher: encrypting changes the bytes");
    Bytes pieces = plain;
    // Any split into pieces, even unaligned, gives the same bytes.
    std::size_t at = 0u;
    for (const std::size_t size : {3u, 1u, 4096u, 5u, 131u}) {
        p3::encrypt(std::span(pieces).subspan(at, size), 0x12345u, at);
        at += size;
    }
    p3::encrypt(std::span(pieces).subspan(at), 0x12345u, at);
    check(pieces == data, "cipher: a keystream started at any byte matches");
    Bytes back = data;
    p3::decrypt(back, 0x12345u, 0u);
    check(back == plain, "cipher: decrypting restores the bytes");
    Bytes moved = data;
    p3::transcode(moved, 0x12345u, 0x12400u, 0u);
    Bytes expected = plain;
    p3::encrypt(expected, 0x12400u, 0u);
    check(moved == expected, "cipher: re-keying equals decrypting and encrypting again");
    Bytes zero = plain;
    p3::encrypt(zero, 0u, 0u);
    Bytes seeded = plain;
    p3::encrypt(seeded, 0x10000u, 0u); // low half 0: its default seed
    check(zero != seeded, "cipher: each half has its own default seed");
    const std::uint8_t psmf[] = {'P', 'S', 'M', 'F', '0'};
    check(p3::verbatim_magic(psmf) && !p3::verbatim_magic(std::span(psmf, 3)), "cipher: verbatim entries by magic");
}

// A small archive: a directory of 2 blocks and five entries. Entry 1 has an
// exact size; entry 3 is stored verbatim.
struct Archive {
    std::vector<Bytes> plain;
    p3::Directory directory;
    Bytes bytes;
};

Archive make_archive() {
    Archive a;
    const std::vector<std::size_t> sizes = {2048, 3000, 6144, 2048, 1000};
    a.directory.directory_blocks = 2u;
    std::uint32_t block = 2u;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        a.plain.push_back(pattern(sizes[i], static_cast<std::uint32_t>(10 + i)));
        a.directory.blocks.push_back(block);
        block += static_cast<std::uint32_t>((sizes[i] + p3::kBlock - 1u) / p3::kBlock);
    }
    std::memcpy(a.plain[3].data(), "PSMF", 4u);
    a.directory.blocks.push_back(block);
    a.directory.sizes = {{1u, 3000u}, {4u, 1000u}};
    // The trailer: whatever is left of the two directory blocks.
    const std::size_t tables = (a.directory.blocks.size() + 4u) * 4u;
    a.directory.trailer = pattern(2u * p3::kBlock - tables, 99);
    a.bytes = a.directory.encode();
    a.bytes.resize(static_cast<std::size_t>(block) * p3::kBlock, 0u);
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        Bytes stored = a.plain[i];
        if (i != 3u) p3::encrypt(stored, a.directory.blocks[i], 0u);
        std::copy(stored.begin(), stored.end(), a.bytes.begin() + a.directory.blocks[i] * p3::kBlock);
    }
    return a;
}

// What the game would get for entry `i` reading `view` through `directory`.
Bytes game_reads(p3::ArchiveView &view, const p3::Directory &directory, std::uint32_t i) {
    Bytes out(static_cast<std::size_t>(directory.size(i)));
    view.read(static_cast<std::uint64_t>(directory.blocks[i]) * p3::kBlock, out);
    if (!p3::verbatim_magic(out)) p3::decrypt(out, directory.blocks[i], 0u);
    return out;
}

void test_archive(const fs::path &root) {
    const Archive a = make_archive();
    const std::optional<p3::Directory> parsed =
        p3::Directory::parse(std::span(a.bytes).first(2u * p3::kBlock), a.bytes.size());
    check(parsed && parsed->blocks == a.directory.blocks && parsed->sizes == a.directory.sizes,
        "archive: the directory parses");
    check(parsed && parsed->encode() == Bytes(a.bytes.begin(), a.bytes.begin() + 2 * p3::kBlock),
        "archive: and encodes back to the same bytes");
    const p3::Directory &disc = *parsed;
    const auto raw = [&a](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
        if (offset >= a.bytes.size()) return 0u;
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), a.bytes.size() - offset));
        std::copy_n(a.bytes.begin() + static_cast<std::ptrdiff_t>(offset), count, out.begin());
        return count;
    };
    p3::ArchiveView view(disc, raw);
    check(game_reads(view, disc, 2) == a.plain[2], "archive: without mods the view is the disc");

    // Replacements: entry 1 grows past its blocks, entry 4 shrinks.
    const Bytes grown = pattern(5000, 50);
    const Bytes small = pattern(700, 51);
    write(root / "archive" / "grown", grown);
    write(root / "archive" / "small", small);
    Bytes patch;
    append32(patch, 0x10u);
    append32(patch, 4u);
    append32(patch, 0xAABBCCDDu);
    append32(patch, 0xFFFFFFFFu);
    append32(patch, 0u);
    write(root / "archive" / "patch", patch);
    Resolution r;
    r.replacements[1] = {"m", root / "archive" / "grown"};
    r.replacements[4] = {"m", root / "archive" / "small"};
    r.patches[2] = {{"m", root / "archive" / "patch"}};
    FileOverlay overlay(
        {[&view](FileId file) { return view.original(file); }, [&disc](FileId file) { return disc.size(file); },
            [](FileId, Bytes &bytes, const Resolution::Source &patch) {
                return p3::apply_patch(bytes, p3::read_blocks(patch));
            }});
    overlay.set(r);
    const auto layout = std::make_shared<const p3::Layout>(p3::Layout::build(disc, overlay.sizes()));
    const p3::Directory &d = layout->directory;
    check(d.blocks[2] == disc.blocks[2] + 1u && d.blocks.back() == disc.blocks.back() + 1u,
        "archive: a grown entry moves the ones after it");
    check(d.size(1) == 5000u && d.size(4) == 700u, "archive: the size table takes the new sizes");
    view.set(layout, &overlay);
    check(view.size() == disc.archive_bytes() + p3::kBlock, "archive: the archive grows");
    Bytes dir(2u * p3::kBlock);
    view.read(0u, dir);
    const std::optional<p3::Directory> served = p3::Directory::parse(dir, view.size());
    check(served && served->blocks == d.blocks && served->sizes == d.sizes, "archive: the game gets the new directory");
    check(game_reads(view, d, 1) == grown, "archive: a grown replacement reads back whole");
    check(game_reads(view, d, 4) == small, "archive: a smaller one too");
    check(game_reads(view, d, 0) == a.plain[0], "archive: an entry before the change is untouched");
    Bytes patched = a.plain[2];
    put32(patched, 0x10u, 0xAABBCCDDu);
    check(game_reads(view, d, 2) == patched, "archive: a moved entry is re-keyed, and patched");
    check(game_reads(view, d, 3) == a.plain[3], "archive: a moved verbatim entry is served as stored");
    // The game reads in pieces; so does this.
    Bytes piecewise(static_cast<std::size_t>(d.size(1)));
    for (std::size_t at = 0; at < piecewise.size(); at += 1024u)
        view.read(static_cast<std::uint64_t>(d.blocks[1]) * p3::kBlock + at,
            std::span(piecewise).subspan(at, std::min<std::size_t>(1024u, piecewise.size() - at)));
    p3::decrypt(piecewise, d.blocks[1], 0u);
    check(piecewise == grown, "archive: reading in pieces gives the same bytes");

    // A replacement that is not a whole number of blocks where the disc has
    // no exact size is padded.
    const p3::Layout padded = p3::Layout::build(disc, {{2u, 100u}});
    check(padded.padded.contains(2u) && padded.directory.size(2) == 3u * p3::kBlock && !padded.moved(disc),
        "archive: a size the table cannot hold is padded to the blocks");
}

void test_patches(const fs::path &root) {
    // A code overlay: MWo3, id, load address, code and data sizes.
    Bytes overlay(64u + 0x100u + 0x40u, 0u);
    std::memcpy(overlay.data(), "MWo3", 4u);
    put32(overlay, 8u, 0x0A055E80u);
    put32(overlay, 12u, 0x100u);
    put32(overlay, 16u, 0x40u);
    Bytes patch;
    const auto block = [&patch](std::uint32_t address, const Bytes &payload, bool run = false) {
        append32(patch, address);
        append32(patch, static_cast<std::uint32_t>(payload.size()) | (run ? 0x80000000u : 0u));
        patch.insert(patch.end(), payload.begin(), payload.end());
    };
    block(0x0A055E80u + 64u + 0x100u + 4u, {1, 2, 3, 4}); // data section
    block(0x4A055E80u + 64u, {9, 9, 9, 9});               // code, through the uncached mirror
    block(0x08801000u, {5, 6});                           // elsewhere in memory
    block(0x08900000u, {7}, true);                        // code to run
    append32(patch, 0xFFFFFFFFu);
    append32(patch, 0u);
    block(0x0A055E80u + 64u, {0xEE}); // after the end marker: ignored
    write(root / "patches" / "p", patch);
    PatchOutcome outcome = p3::apply_patch(overlay, root / "patches" / "p");
    check(overlay[64u + 0x100u + 4u] == 1u && overlay[64u + 0x103u + 4u] == 4u, "patch: an overlay's data by address");
    // The overlay keeps the code its recompiled corpus was made from; the
    // change to its code is written to memory once that corpus is installed.
    check(overlay[64u] == 0u && outcome.after_load.size() == 3u && outcome.after_load[0].address == 0x0A055E80u + 64u &&
            outcome.after_load[0].bytes == Bytes{9, 9, 9, 9},
        "patch: its code, through a mirror address, is written to memory after the load");
    check(outcome.applied == 1u && outcome.after_load[1].address == 0x08801000u,
        "patch: memory outside the overlay is written after it loads");
    check(outcome.run == std::vector<std::uint32_t>{0x08900000u} && outcome.after_load[2].address == 0x08900000u,
        "patch: code to run is written after the load, then called");
    check(outcome.problems.empty(), "patch: nothing is reported for an overlay patch that applies");

    Bytes data = pattern(256, 5);
    patch.clear();
    block(0x20u, {0xAB, 0xCD});
    block(0x08801000u, {1});
    block(0x0BFF3800u, {2}, true);
    block(0x1000u, {1});
    block(0x40u, {3}, true);
    write(root / "patches" / "q", patch); // no end marker, as the manager installs it
    outcome = p3::apply_patch(data, root / "patches" / "q");
    check(data[0x20] == 0xABu && data[0x21] == 0xCDu && outcome.applied == 1u, "patch: an offset into a data file");
    // The loader's patches are code loaded with a file: memory blocks go in
    // once the game has read it.
    check(outcome.after_load.size() == 2u && outcome.after_load[0].address == 0x08801000u &&
            outcome.run == std::vector<std::uint32_t>{0x0BFF3800u},
        "patch: a data file's patch writes memory and runs code after the file is read");
    check(outcome.problems.size() == 2u, "patch: a block past the end, and code to run inside the file, are refused");
}

void test_yakumo_format(const fs::path &root) {
    // Bytes and edits as a mod.ini writes them.
    std::string error;
    check(parse_bytes("27 BD FF F0", error) == Bytes{0x27, 0xBD, 0xFF, 0xF0} &&
            parse_bytes(" 27BDFFF0 ", error) == Bytes{0x27, 0xBD, 0xFF, 0xF0},
        "edits: hex bytes in order, spaced or not");
    check(parse_bytes("0x24020001 0x1234 0xAB", error) == Bytes{0x01, 0x00, 0x02, 0x24, 0x34, 0x12, 0xAB},
        "edits: 0x numbers are words, halfwords and bytes, low byte first");
    for (const char *bad : {"0x123", "G1", "", "0x", "ABC", "0x1234567890"}) {
        error.clear();
        check(!parse_bytes(bad, error) && !error.empty(), std::string("edits: \"") + bad + "\" is refused");
    }
    std::optional<Edit> edit = parse_edit(" 0x08A00010 ", "00 00 -> 0x1234 ; a comment", error);
    check(edit && edit->address == 0x08A00010u && edit->expect == Bytes{0, 0} && edit->bytes == Bytes{0x34, 0x12},
        "edits: address, expected bytes and bytes, with a comment");
    edit = parse_edit("120", "01 # comment", error);
    check(edit && edit->address == 0x120u && edit->expect.empty() && edit->bytes == Bytes{1},
        "edits: an offset without 0x, and nothing expected");
    check(!parse_edit("zz", "01", error) && !parse_edit("123456789", "01", error) &&
            !parse_edit("10", "00 -> 01 02", error) && !parse_edit("10", "-> 01", error) &&
            !parse_edit("10", "01 ->", error),
        "edits: a bad address, a length mismatch and a missing side are refused");

    // A mod in Yakumo's own format.
    const p3::ModFolderFormat format(6043u);
    const fs::path mods = root / "yakumo";
    write(mods / "full" / "mod.ini",
        "[MOD INFO]\nName=\"Full\"\nType=\"Yakumo\"\nModVersion=\"1.2\"\nPriority=\"1\"\n"
        "[Files]\n0601 = armour.pac\n05D2 = sub/HELM.pac\n"
        "[Patches]\n0700 = fix.bin\n"
        "[Patch 0702]\n; a comment line\n0x10 = 01 02\n0x20 = 00 -> FF\n"
        "[Patch 0703]\n"
        "[Code]\nFiles=\"hook.bin;more.bin\"\n0x08B00000 = image.bin\n"
        "[Memory]\n0x08A00000 = 0x03E00008\n0x08A00004 = 00000000 -> 0x24020001\n");
    for (const char *name : {"armour.pac", "sub/helm.pac", "fix.bin", "hook.bin", "more.bin"})
        write(mods / "full" / name, pattern(16, 4));
    write(mods / "full" / "image.bin", Bytes{0x08, 0x00, 0xE0, 0x03, 0, 0, 0, 0});
    std::optional<Mod> mod = format.read(mods / "full");
    check(mod && mod->unusable.empty() && mod->type == "Yakumo mod" && mod->priority == 1 &&
            mod->version.find("1.2") != std::string::npos,
        "yakumo: a mod in Yakumo's format, with its own version");
    check(mod && mod->changes.size() == 4u && mod->changes[0].kind == FileChange::Kind::Replace &&
            mod->changes[1].file == 0x5D2u && mod->changes[2].kind == FileChange::Kind::Patch &&
            !mod->changes[2].source.empty() && mod->changes[3].file == 0x702u && mod->changes[3].source.empty() &&
            mod->changes[3].edits.size() == 2u && mod->changes[3].edits[1].expect == Bytes{0},
        "yakumo: files, patch files and a patch written as edits; an empty patch section adds nothing");
    check(mod && mod->code.size() == 2u && mod->memory.size() == 3u && mod->memory[0].address == 0x08B00000u &&
            mod->memory[0].bytes.size() == 8u && mod->memory[0].expect.empty() &&
            mod->memory[1].bytes == Bytes{8, 0, 0xE0, 3},
        "yakumo: code files, a raw image at its address, then memory edits");

    const auto refused = [&](const std::string &name, const std::string &ini, const std::string &why) {
        write(mods / name / "mod.ini", "[MOD INFO]\nType=\"Yakumo\"\n" + ini);
        const std::optional<Mod> read = format.read(mods / name);
        check(read && read->unusable.find(why) != std::string::npos, "yakumo: " + name + " is refused: " + why);
    };
    refused("newer", "Format=\"2\"\n[Memory]\n08A00000=00\n", "newer Yakumo");
    refused("psp", "Version=\"NOHD\"\n[Memory]\n08A00000=00\n", "PSP version");
    refused("nothing", "Name=\"N\"\n", "changes nothing");
    refused("bad_edit", "[Memory]\n08A00000 = 0x123\n", "[Memory] 08A00000");
    refused("bad_patch_edit", "[Patch 0601]\n10 = zz\n", "[Patch 0601] 10");
    refused("bad_id", "[Files]\nnope = a.pac\n", "not a file id");
    refused("past_end", "[Patch FFFF]\n10 = 00\n", "not a file of this game");
    refused("missing", "[Code]\nFiles=\"gone.bin\"\n", "missing from the folder");
    refused("missing_patch", "[Patches]\n0601 = gone.bin\n", "missing from the folder");
    refused("image_address", "[Code]\nsomewhere = image.bin\n", "[Code] somewhere");
    refused("image_missing", "[Code]\n08B00000 = gone.bin\n", "missing from the folder");
    // Nothing outside the mod's folder, however it is named.
    write(mods / "secret.bin", pattern(16, 4));
    refused("dot_dot", "[Code]\nFiles=\"../secret.bin\"\n", "missing from the folder");
    refused("dot_dot_inside", "[Code]\nFiles=\"sub/../../secret.bin\"\n", "missing from the folder");
    refused("absolute", "[Code]\nFiles=\"" + to_utf8(mods / "secret.bin") + "\"\n", "missing from the folder");
    refused("drive", "[Code]\nFiles=\"C:secret.bin\"\n", "missing from the folder");
#if !defined(_WIN32)
    write(mods / "link_out" / "placeholder", pattern(4, 1));
    std::error_code link_error;
    fs::create_symlink(mods / "secret.bin", mods / "link_out" / "linked.bin", link_error);
    if (!link_error) refused("link_out", "[Code]\nFiles=\"linked.bin\"\n", "missing from the folder");
#endif
    write(mods / "image_empty" / "empty.bin", std::string());
    refused("image_empty", "[Code]\n08B00000 = empty.bin\n", "empty, unreadable or larger");

    // Native code for the running platform, and a script.
    const std::string library_name = std::string("example_mod") + native_extension();
    write(mods / "native" / "mod.ini",
        "[MOD INFO]\nName=Armor Model Log\nType=Yakumo\nVersion=HD\nFormat=1\n[Native]\nLibrary=example_mod\n");
    write(mods / "native" / "native" / native_platform() / library_name, pattern(16, 1));
    write(mods / "native" / "native" / "other-platform" / library_name, pattern(16, 2));
    mod = format.read(mods / "native");
    check(mod && mod->unusable.empty() && mod->native.filename() == library_name &&
            mod->native.parent_path().filename() == native_platform() &&
            std::any_of(mod->notes.begin(), mod->notes.end(),
                [](const std::string &note) { return note.find("people you trust") != std::string::npos; }),
        "yakumo: native code for this platform, with a warning");
    write(mods / "script" / "mod.ini", "[MOD INFO]\nType=Yakumo\n[Script]\nFile=main.luau\n");
    write(mods / "script" / "main.luau", std::string("yakumo.log('hi')\n"));
    mod = format.read(mods / "script");
    check(mod && mod->unusable.empty() && mod->script.filename() == "main.luau", "yakumo: a script");
    // New files, under ids past the disc's.
    write(mods / "adds" / "mod.ini", "[MOD INFO]\nType=Yakumo\n[Add]\n2000 = new/model.pac\n179B = first.bin\n");
    write(mods / "adds" / "new" / "model.pac", pattern(40, 3));
    write(mods / "adds" / "first.bin", pattern(8, 4));
    mod = format.read(mods / "adds");
    check(mod && mod->unusable.empty() && mod->changes.size() == 2u && mod->changes[0].kind == FileChange::Kind::Add &&
            mod->changes[0].file == 0x2000u && mod->changes[1].file == 0x179Bu,
        "yakumo: new files under ids from the disc's count up");
    refused("add_existing", "[Add]\n0601 = a.pac\n", "[Add] 0601: a new file needs an id from 179B");
    refused("add_none", "[Add]\nFFFF = a.pac\n", "[Add] FFFF");
    refused("add_missing", "[Add]\n2000 = gone.pac\n", "missing from the folder");
    write(mods / "adds_too" / "mod.ini", "[MOD INFO]\nType=Yakumo\n[Add]\n2000 = other.pac\n");
    write(mods / "adds_too" / "other.pac", pattern(8, 5));
    write(mods / "elsewhere" / "native" / "other-platform" / library_name, pattern(16, 2));
    refused("elsewhere", "[Native]\nLibrary=example_mod\n", "no native code for this platform");
    refused("script_missing", "[Script]\nFile=gone.luau\n", "missing from the folder");

    // Quests: the payload as the game holds it, its record naming the same id.
    const auto quest_payload = [](std::uint16_t id) {
        Bytes payload(0x100u, 0u);
        payload[0] = 0x10u;
        payload[0x10u + 0x1Cu] = static_cast<std::uint8_t>(id);
        payload[0x10u + 0x1Du] = static_cast<std::uint8_t>(id >> 8u);
        const std::string name = "A quest";
        std::copy(name.begin(), name.end(), payload.begin() + 0x10 + 0x48);
        return payload;
    };
    write(mods / "quests" / "mod.ini", "[MOD INFO]\nType=Yakumo\n[Quests]\n62001 = q1.bin\n65535 = q2.bin\n");
    write(mods / "quests" / "q1.bin", quest_payload(62001u));
    write(mods / "quests" / "q2.bin", quest_payload(65535u));
    mod = format.read(mods / "quests");
    check(mod && mod->unusable.empty() && mod->changes.size() == 2u &&
            mod->changes[0].kind == FileChange::Kind::Quest && mod->changes[0].file == 0x6001u &&
            mod->changes[1].file == 0x6DCFu,
        "yakumo: quests from 62000 up, served as files from 6000 up");
    check(p3::quest_file(62000u) == 0x6000u && !p3::quest_file(61999u) && !p3::quest_file(60001u) &&
            p3::quest_of_file(0x6DCFu) == 65535u && !p3::quest_of_file(0x6DD0u) && !p3::quest_of_file(0x5FFFu),
        "yakumo: quest ids and their files map both ways");
    write(mods / "quest_other" / "q.bin", quest_payload(62002u));
    refused("quest_other", "[Quests]\n62001 = q.bin\n", "is quest 62002");
    write(mods / "quest_low" / "q.bin", quest_payload(204u));
    refused("quest_low", "[Quests]\n204 = q.bin\n", "[Quests] 204: a new quest needs an id from 62000");
    refused("quest_word", "[Quests]\nnew = q.bin\n", "[Quests] new");
    write(mods / "quest_junk" / "q.bin", Bytes{1u, 2u, 3u});
    refused("quest_junk", "[Quests]\n62003 = q.bin\n", "not a quest the game can read");
    write(mods / "quest_huge" / "q.bin", Bytes(30000u, 0u));
    refused("quest_huge", "[Quests]\n62004 = q.bin\n", "not a quest the game can read");
    refused("quest_missing", "[Quests]\n62005 = gone.bin\n", "missing from the folder");
    refused("add_in_quests", "[Add]\n6100 = a.pac\n", "kept for the quests mods add");

    // Resolved: the mod's memory edits come after its code files.
    ModLibrary library(format);
    library.scan(mods);
    library.set_enabled("full", true);
    library.set_enabled("native", true);
    library.set_enabled("script", true);
    library.set_enabled("adds", true);
    library.set_enabled("adds_too", true);
    library.set_enabled("quests", true);
    const Resolution resolution = library.resolve();
    check(resolution.quests.size() == 2u && resolution.quests.at(0x6001u).path.filename() == "q1.bin" &&
            resolution.added.size() == 2u,
        "yakumo: quests reach the resolution apart from added files");
    const auto added_conflict = std::find_if(resolution.conflicts.begin(), resolution.conflicts.end(),
        [](const Resolution::Conflict &c) { return c.file == 0x2000u; });
    check(resolution.added.size() == 2u && resolution.added.contains(0x179Bu) &&
            added_conflict != resolution.conflicts.end() && added_conflict->overridden.size() == 1u &&
            resolution.added.at(0x2000u).mod == added_conflict->winner,
        "yakumo: added files reach the resolution; two mods adding one id conflict, the higher rank wins");
    check(!resolution.empty(), "yakumo: a resolution with only added files is not empty");
    check(resolution.native.size() == 1u && resolution.native[0].path.filename() == library_name &&
            resolution.scripts.size() == 1u && resolution.scripts[0].path.filename() == "main.luau",
        "yakumo: native code and scripts of mods that are on reach the resolution");
    check(resolution.code.size() == 3u && resolution.code[2].path.empty() && resolution.code[2].edits.size() == 3u &&
            resolution.patches.at(0x702u).front().edits.size() == 2u,
        "yakumo: memory edits follow the code files, and patch edits reach the resolution");

    // Expected bytes in a file: written where they match, skipped otherwise.
    Bytes data(64u, 0u);
    data[0x20] = 0x11u;
    p3::Blocks blocks;
    blocks.blocks.push_back({0x10u, {1, 2}, false, {0, 0}});
    blocks.blocks.push_back({0x20u, {0xFF}, false, {0x00}});
    PatchOutcome outcome = p3::apply_patch(data, blocks);
    check(data[0x10] == 1u && data[0x11] == 2u && data[0x20] == 0x11u && outcome.applied == 1u &&
            outcome.problems.size() == 1u && outcome.problems[0].find("expected 00, found 11") != std::string::npos,
        "yakumo: an edit over other bytes than it expects is skipped and reported");
    Bytes overlay(64u + 0x10u, 0u);
    std::memcpy(overlay.data(), "MWo3", 4u);
    put32(overlay, 8u, 0x0A000000u);
    p3::Blocks outside;
    outside.blocks.push_back({0x08A00000u, {1}, false, {7}});
    outcome = p3::apply_patch(overlay, outside);
    check(outcome.after_load.size() == 1u && outcome.after_load[0].expect == Bytes{7},
        "yakumo: an edit to memory after an overlay loads keeps what it expects");

    // Expected bytes in memory at start.
    psprecomp::Runtime runtime(64u * 1024u * 1024u);
    runtime.memory().store32(0x08A00000u, 0x11111111u);
    Resolution::Source edits{"m", {}, {}};
    edits.edits.push_back({0x08A00000u, {0xAA}, {0x11}});
    edits.edits.push_back({0x08A00001u, {0xBB}, {0x22}});
    const p3::CodeReport report = p3::write_code(runtime, {{"Edits", edits}});
    check(runtime.memory().load8(0x08A00000u) == 0xAAu && runtime.memory().load8(0x08A00001u) == 0x11u &&
            report.blocks == 1u && report.problems.size() == 1u &&
            report.problems[0].find("expected 22, found 11") != std::string::npos,
        "yakumo: a memory edit over other bytes than it expects is skipped and reported");
}

int check_disc(const char *image) {
    std::ifstream in(image, std::ios::binary);
    if (!in) {
        std::printf("cannot open %s\n", image);
        return 1;
    }
    // Find /PSP_GAME/USRDIR/DATA.BIN the simple way: the tool in
    // profiles/mhp3rd/tools/databin.py does the same walk.
    const auto sector = [&in](std::uint64_t lba, std::size_t count) {
        Bytes bytes(count * 2048u);
        in.seekg(static_cast<std::streamoff>(lba * 2048u));
        in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return bytes;
    };
    const auto load = [](const Bytes &b, std::size_t at) {
        return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8u |
            static_cast<std::uint32_t>(b[at + 2]) << 16u | static_cast<std::uint32_t>(b[at + 3]) << 24u;
    };
    const Bytes primary = sector(16, 1);
    std::uint32_t lba = load(primary, 156 + 2);
    std::uint32_t size = load(primary, 156 + 10);
    for (const char *want : {"PSP_GAME", "USRDIR", "DATA.BIN"}) {
        const Bytes dir = sector(lba, (size + 2047u) / 2048u);
        bool found = false;
        for (std::size_t at = 0; at < dir.size() && !found;) {
            const std::uint8_t length = dir[at];
            if (length == 0u) {
                at = (at / 2048u + 1u) * 2048u;
                continue;
            }
            const std::string name(reinterpret_cast<const char *>(&dir[at + 33]), dir[at + 32]);
            if (name.substr(0, name.find(';')) == want) {
                lba = load(dir, at + 2);
                size = load(dir, at + 10);
                found = true;
            }
            at += length;
        }
        if (!found) {
            std::printf("no %s in the image\n", want);
            return 1;
        }
    }
    const Bytes head = sector(lba, 17);
    const std::optional<p3::Directory> directory = p3::Directory::parse(head, size);
    check(directory.has_value(), "disc: DATA.BIN's directory parses");
    if (!directory) return 1;
    std::printf("     %zu entries, %zu exact sizes, %u directory blocks, %zu trailer bytes\n", directory->entries(),
        directory->sizes.size(), directory->directory_blocks, directory->trailer.size());
    const Bytes encoded = directory->encode();
    check(encoded == Bytes(head.begin(), head.begin() + static_cast<std::ptrdiff_t>(encoded.size())),
        "disc: the directory encodes back to the bytes on the disc");
    const p3::Layout grown = p3::Layout::build(*directory, {{0x0FEEu, directory->size(0x0FEE) + 300u * 1024u}});
    check(grown.moved(*directory) && grown.directory.blocks[0x0FEF] == directory->blocks[0x0FEF] + 150u,
        "disc: growing an entry moves the ones after it");
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "--check-disc") return check_disc(argv[2]);
    const Scratch scratch;
    test_ini();
    test_format(scratch.root);
    test_library(scratch.root);
    test_import(scratch.root);
    test_cipher();
    test_archive(scratch.root);
    test_patches(scratch.root);
    test_yakumo_format(scratch.root);
    std::printf("%s\n", failures == 0 ? "all mod checks passed" : "mod checks FAILED");
    return failures == 0 ? 0 : 1;
}
