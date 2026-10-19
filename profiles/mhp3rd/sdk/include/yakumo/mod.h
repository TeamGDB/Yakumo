/*
 * Yakumo native mod API.
 *
 * A native mod is a shared library in its mod folder, at
 * native/<platform>/<name>.<dll|dylib|so>, that exports yakumo_mod_main().
 * Yakumo calls it once, when the game starts with the mod on, and passes the
 * host interface below. Everything else goes through services: versioned
 * tables of functions a mod looks up by name. A service a mod cannot run
 * without is required (yakumo_mod_main() fails without it); any other is
 * optional, and the mod keeps working on a Yakumo that lacks it.
 *
 * The interface is plain C, so a mod can be built with any compiler and
 * language that can call C. The C++ layer in yakumo/mod.hpp sits on top.
 *
 * Rules:
 *  - Callbacks run on the thread that runs the game, between guest
 *    instructions: never while guest code is half-way through one. They must
 *    not block, and they must not keep pointers into guest memory past their
 *    return.
 *  - Guest addresses are 32-bit PSP addresses (0x08800000 and up). Reads and
 *    writes are checked: an address outside guest memory fails, it never
 *    touches host memory.
 *  - Strings are UTF-8 and NUL-terminated.
 *  - Nothing here is thread-safe; call it from callbacks or from
 *    yakumo_mod_main() only.
 */
#ifndef YAKUMO_MOD_H
#define YAKUMO_MOD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define YAKUMO_MOD_EXPORT __declspec(dllexport)
#else
#define YAKUMO_MOD_EXPORT __attribute__((visibility("default")))
#endif

/* The interface version this header describes. Services carry versions of
 * their own. A mod states the version it was built for with
 * YAKUMO_MOD_API_VERSION(), once, in one source file; a host refuses a mod
 * built for a newer interface than its own (host->api_version). */
#define YAKUMO_API_VERSION 1u
#define YAKUMO_MOD_API_VERSION_NAME "yakumo_mod_api_version"
#ifdef __cplusplus
#define YAKUMO_MOD_API_VERSION() \
    extern "C" { \
    YAKUMO_MOD_EXPORT extern const uint32_t yakumo_mod_api_version; \
    YAKUMO_MOD_EXPORT const uint32_t yakumo_mod_api_version = YAKUMO_API_VERSION; \
    } \
    static_assert(true, "")
#else
#define YAKUMO_MOD_API_VERSION() YAKUMO_MOD_EXPORT const uint32_t yakumo_mod_api_version = YAKUMO_API_VERSION
#endif

typedef struct ymod_mod ymod_mod; /* this mod, as the host knows it */

enum ymod_log_level { YMOD_LOG_INFO = 0, YMOD_LOG_WARNING = 1, YMOD_LOG_ERROR = 2 };

typedef struct ymod_host {
    uint32_t api_version; /* YAKUMO_API_VERSION of the host */
    uint32_t size;        /* sizeof(ymod_host) in the host: fields past it are absent */
    /* The service called `name` at `version` or a newer compatible one, or
     * NULL when the host has none. Each service struct starts with its own
     * version and size. */
    const void *(*service)(ymod_mod *mod, const char *name, uint32_t version);
    /* Into Yakumo's log and the mod's page, prefixed with the mod's name. */
    void (*log)(ymod_mod *mod, int level, const char *message);
    /* The mod's folder, for its own files. */
    const char *(*folder)(ymod_mod *mod);
} ymod_host;

/* Exported by every native mod. Returns 0 when the mod is ready, anything
 * else to refuse to run (it is then shown as failed and not called again). */
typedef int (*ymod_main_fn)(const ymod_host *host, ymod_mod *mod);
#define YAKUMO_MOD_MAIN_NAME "yakumo_mod_main"

/* --------------------------------------------------------------------------
 * The guest CPU, as callbacks see it. Registers are the Allegrex's: gpr[4..7]
 * are a0..a3, gpr[2..3] v0..v1, gpr[29] sp, gpr[31] ra. Writing pc in a hook
 * sends the guest there.
 */
typedef struct ymod_cpu {
    uint32_t gpr[32];
    uint32_t hi;
    uint32_t lo;
    uint32_t pc;
    float fpr[32];
    uint32_t fcr31;
} ymod_cpu;

/* --------------------------------------------------------------------------
 * "yakumo.memory" 1: guest memory.
 */
#define YMOD_MEMORY "yakumo.memory"
typedef struct ymod_memory {
    uint32_t version;
    uint32_t size;
    /* Return 0 on success, -1 when [address, address + length) is not guest
     * memory. */
    int (*read)(uint32_t address, void *out, uint32_t length);
    int (*write)(uint32_t address, const void *data, uint32_t length);
    /* Guest memory reserved for this mod from the game's allocator, aligned
     * to `alignment` (a power of two, at least 16): its address, or 0. */
    uint32_t (*allocate)(ymod_mod *mod, uint32_t size, uint32_t alignment);
    void (*release)(ymod_mod *mod, uint32_t address);
    /* Tells the JIT that guest code in [address, address + length) changed,
     * after writing instructions. */
    void (*code_changed)(uint32_t address, uint32_t length);
} ymod_memory;

/* --------------------------------------------------------------------------
 * "yakumo.hooks" 1: running host code at the game's own functions.
 *
 * A function hook takes over a guest function's entry address:
 *  - BEFORE runs the callback, then the game's function, with the registers
 *    as the callback left them.
 *  - AFTER runs the game's function, then the callback when it returns, with
 *    its return value in v0/v1; the callback may change them.
 *  - REPLACE runs the callback instead. The callback sets v0/v1 and returns
 *    YMOD_RETURN to return to the caller, or YMOD_ORIGINAL to run the game's
 *    function after all.
 * An instruction hook runs the callback just before the instruction at any
 * address runs, inside native code compiled by the JIT; it cannot redirect.
 *
 * Addresses inside code overlays are given with the overlay: a hook there is
 * installed whenever that overlay is loaded, and absent otherwise.
 */
#define YMOD_HOOKS "yakumo.hooks"
enum ymod_hook_kind { YMOD_HOOK_BEFORE = 0, YMOD_HOOK_AFTER = 1, YMOD_HOOK_REPLACE = 2 };
enum ymod_hook_result { YMOD_CONTINUE = 0, YMOD_RETURN = 1, YMOD_ORIGINAL = 2 };
typedef int (*ymod_hook_fn)(ymod_cpu *cpu, void *user);
typedef uint32_t ymod_hook; /* 0: failed */
typedef struct ymod_hooks {
    uint32_t version;
    uint32_t size;
    /* `overlay`: NULL for the executable, or the overlay's name ("game_task").
     * `address` is the guest address the code runs at. */
    ymod_hook (*function)(
        ymod_mod *mod, const char *overlay, uint32_t address, int kind, ymod_hook_fn callback, void *user);
    ymod_hook (*instruction)(ymod_mod *mod, const char *overlay, uint32_t address, ymod_hook_fn callback, void *user);
    void (*remove)(ymod_mod *mod, ymod_hook hook);
} ymod_hooks;

/* --------------------------------------------------------------------------
 * "yakumo.calls" 1: calling a guest function from a callback. The call runs
 * when the callback returns, on the same guest thread, as guest code: it may
 * block. `done` then gets its v0. Only from a hook callback.
 */
#define YMOD_CALLS "yakumo.calls"
typedef void (*ymod_call_done_fn)(ymod_cpu *cpu, uint32_t result, void *user);
typedef struct ymod_calls {
    uint32_t version;
    uint32_t size;
    int (*call)(ymod_cpu *cpu, uint32_t address, const uint32_t arguments[4], ymod_call_done_fn done, void *user);
} ymod_calls;

/* --------------------------------------------------------------------------
 * "yakumo.events" 1: being told when things happen.
 */
#define YMOD_EVENTS "yakumo.events"
enum ymod_event {
    YMOD_EVENT_FRAME = 0,           /* between two game frames */
    YMOD_EVENT_FILE_LOADED = 1,     /* the game read a whole DATA.BIN file: value = file id */
    YMOD_EVENT_OVERLAY_LOADED = 2,  /* a code overlay was copied to its slot: value = load address */
    YMOD_EVENT_GAME_STARTED = 3,    /* the game's first frame */
    YMOD_EVENT_OVERLAY_UNLOADED = 4, /* a code overlay left its slot: value = its load address */
    YMOD_EVENT_QUEST_STARTED = 5,   /* the quest overlay loaded: value = the quest's id */
    YMOD_EVENT_QUEST_ENDED = 6,     /* back from a quest, however it ended: value = the quest's id */
};
typedef void (*ymod_event_fn)(int event, uint32_t value, void *user);
typedef struct ymod_events {
    uint32_t version;
    uint32_t size;
    int (*subscribe)(ymod_mod *mod, int event, ymod_event_fn callback, void *user);
    /* Stops what subscribe() started with the same event, callback and user. */
    void (*unsubscribe)(ymod_mod *mod, int event, ymod_event_fn callback, void *user);
} ymod_events;

/* --------------------------------------------------------------------------
 * "yakumo.files" 1: the game's files. A file is a DATA.BIN entry by id.
 */
#define YMOD_FILES "yakumo.files"
typedef struct ymod_files {
    uint32_t version;
    uint32_t size;
    /* Replaces a file with bytes (copied), from the next time the game loads
     * it. A larger file applies at the next start. Returns 0 on success. */
    int (*replace)(ymod_mod *mod, uint32_t file, const void *data, uint32_t length);
    /* Reads the game's own bytes of a file into `out`, up to `capacity`.
     * Returns the file's size, or -1. */
    int64_t (*read_original)(uint32_t file, void *out, uint32_t capacity);
} ymod_files;

/* --------------------------------------------------------------------------
 * "yakumo.settings" 1: the mod's own settings, kept between runs and shown on
 * its page in the Mods menu.
 */
#define YMOD_SETTINGS "yakumo.settings"
typedef struct ymod_settings {
    uint32_t version;
    uint32_t size;
    /* Declares a setting shown on the mod's page: a switch, a whole number
     * between minimum and maximum, or a choice between `choices`, separated
     * by ';'. Returns its current value. */
    int64_t (*declare_switch)(ymod_mod *mod, const char *key, const char *label, int fallback);
    int64_t (*declare_number)(
        ymod_mod *mod, const char *key, const char *label, int64_t minimum, int64_t maximum, int64_t fallback);
    int64_t (*declare_choice)(ymod_mod *mod, const char *key, const char *label, const char *choices, int64_t fallback);
    int64_t (*get)(ymod_mod *mod, const char *key);
} ymod_settings;

/* --------------------------------------------------------------------------
 * "yakumo.services" 1: services between mods. A mod publishes a table of its
 * own under a name and version; mods that depend on it import it.
 */
#define YMOD_SERVICES "yakumo.services"
typedef struct ymod_services {
    uint32_t version;
    uint32_t size;
    int (*publish)(ymod_mod *mod, const char *name, uint32_t version, const void *table);
    const void *(*import)(ymod_mod *mod, const char *name, uint32_t version);
} ymod_services;

/* --------------------------------------------------------------------------
 * "yakumo.symbols" 1: the game's known functions and data by name, from the
 * table in sdk/symbols.txt ("armor.model_of_part", "hunter.zenny").
 */
#define YMOD_SYMBOLS "yakumo.symbols"
enum ymod_symbol_kind { YMOD_SYMBOL_FUNCTION = 0, YMOD_SYMBOL_DATA = 1 };
/* Set `size` to sizeof(ymod_symbol) before passing one: the host fills in
 * only the fields that fit, so a later host with more fields never writes
 * past an older mod's struct. */
typedef struct ymod_symbol {
    uint32_t size;
    const char *name;
    const char *overlay; /* NULL: the executable; otherwise the code overlay ("game_task") */
    const char *about;   /* what it is, in a line */
    uint32_t address;
    uint32_t kind;
} ymod_symbol;
typedef struct ymod_symbols {
    uint32_t version;
    uint32_t size;
    /* The address of `name`, and the rest of its entry in `out` when not
     * NULL; 0 when the name is unknown, or when the game's code there is not
     * the code the table was made from (another version of the game). The
     * strings stay valid while the game runs. */
    uint32_t (*find)(const char *name, ymod_symbol *out);
    /* The entry whose address is exactly `address`: 0 when there is one. */
    int (*name_of)(uint32_t address, ymod_symbol *out);
    /* Every entry, from index 0 until get() fails. */
    int (*get)(uint32_t index, ymod_symbol *out);
    /* The code overlay loaded now whose image covers `address`: its name
     * (without ".ovl") into `out`, and its load address as the result; 0
     * when none covers it. */
    uint32_t (*overlay_at)(uint32_t address, char *out, uint32_t capacity);
} ymod_symbols;

/* --------------------------------------------------------------------------
 * "yakumo.ui" 1: drawing in Yakumo's interface. A panel sits on the mod's
 * page in the Mods menu, under its settings; an overlay is drawn over the
 * running game, every frame, in a small window of its own with the mod's
 * title. Both are drawn by a callback, every frame they show, through the
 * widgets below, which exist only during that call.
 *
 * Widgets that take input (button, checkbox, sliders, text input) work on
 * panels; an overlay takes no input, so there they only show. The shapes
 * draw on the game's picture, in coordinates from 0 to 1 from its top left,
 * and only in overlays. Colours are 0xRRGGBBAA.
 */
#define YMOD_UI "yakumo.ui"
typedef struct ymod_widgets {
    uint32_t version;
    uint32_t size;
    void (*text)(const char *text);
    void (*text_colored)(uint32_t rgba, const char *text);
    void (*separator)(void);
    void (*same_line)(void);
    /* A heading that opens and closes: 1 while open. */
    int (*section)(const char *label);
    /* 1 in the frame it is pressed. */
    int (*button)(const char *label);
    /* These return 1 in the frame the player changes the value. */
    int (*checkbox)(const char *label, int *value);
    int (*slider_int)(const char *label, int32_t *value, int32_t minimum, int32_t maximum);
    int (*slider_float)(const char *label, float *value, float minimum, float maximum);
    int (*input_text)(const char *label, char *buffer, uint32_t capacity);
    void (*line)(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness);
    void (*rect)(float x0, float y0, float x1, float y1, uint32_t rgba, int filled);
    /* `radius` as a part of the picture's height. */
    void (*circle)(float x, float y, float radius, uint32_t rgba, int filled);
    void (*text_at)(float x, float y, uint32_t rgba, const char *text);
} ymod_widgets;
typedef void (*ymod_draw_fn)(const ymod_widgets *ui, void *user);
typedef struct ymod_ui {
    uint32_t version;
    uint32_t size;
    /* Return 0 on success. */
    int (*panel)(ymod_mod *mod, const char *title, ymod_draw_fn draw, void *user);
    int (*overlay)(ymod_mod *mod, const char *title, ymod_draw_fn draw, void *user);
    /* Removes the panels and overlays drawn by `draw` with `user`. */
    void (*remove)(ymod_mod *mod, ymod_draw_fn draw, void *user);
} ymod_ui;

#ifdef __cplusplus
}
#endif

#endif /* YAKUMO_MOD_H */
