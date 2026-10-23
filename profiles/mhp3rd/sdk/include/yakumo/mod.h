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
 * else to refuse to run: it is then shown as failed and nothing of it is
 * called again. Its hooks, events, panels, overlays and published services
 * go; what it added to the game before (kinds, quests, weapons, armor,
 * areas, table rows, replaced files, memory it allocated) stays for the
 * session, as the game's tables cannot shrink, and its kinds' monsters
 * behave as their base's. */
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
     * `address` is the guest address the code runs at. 0 when the hook
     * cannot be installed: the code there is not the known function there,
     * or cannot be compiled to run under the hook. A hook in an overlay that
     * is not loaded now is installed when it loads. */
    ymod_hook (*function)(
        ymod_mod *mod, const char *overlay, uint32_t address, int kind, ymod_hook_fn callback, void *user);
    /* 0 also when the JIT is off (PSPRECOMP_NO_JIT=1): the interpreter does
     * not run instruction hooks. */
    ymod_hook (*instruction)(ymod_mod *mod, const char *overlay, uint32_t address, ymod_hook_fn callback, void *user);
    /* Removes a hook this mod added; another mod's stays. */
    void (*remove)(ymod_mod *mod, ymod_hook hook);
} ymod_hooks;

/* --------------------------------------------------------------------------
 * "yakumo.calls" 1: calling a guest function from a callback. The call runs
 * when the callback returns, on the same guest thread, as guest code: it may
 * block. `done` then gets its v0. Only from a REPLACE hook's callback, and
 * once in it: the call's result returns to the hooked function's caller. A
 * call from anywhere else, or a second one, is refused (-1).
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
     * it, until the game ends (call it again at each start). Returns 0, or -1
     * with the reason on the mod's page: for a file the disc does not have,
     * or a change the running game cannot take (a size the archive keeps
     * changes, the file outgrows its blocks, or another change waits for a
     * restart; docs/MODDING.md). */
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

/* --------------------------------------------------------------------------
 * "yakumo.tables" 1: game tables mods can make longer, such as the kinds of
 * large monster and the model sets (docs/MODDING.md, "Game tables"). A table
 * extended is moved to memory of its own, with the game's rows copied in and
 * the game's code pointed at it; rows past the game's start at zero, and a
 * mod writes its own there through "yakumo.memory". A table inside a code
 * overlay keeps its rows while the overlay comes and goes.
 */
#define YMOD_TABLES "yakumo.tables"
typedef struct ymod_table {
    uint32_t size; /* set to sizeof(ymod_table) before passing one */
    const char *name;
    const char *overlay; /* NULL: the executable; otherwise the code overlay it lies in */
    uint32_t address;    /* where it is now */
    uint32_t row;        /* bytes per row */
    uint32_t rows;       /* rows it has now */
    uint32_t game_rows;  /* rows the game has */
} ymod_table;
typedef struct ymod_tables {
    uint32_t version;
    uint32_t size;
    /* 0 and the table's entry in `out`, or -1 for a name not known. */
    int (*find)(const char *name, ymod_table *out);
    /* Every table, from index 0 until get() fails. */
    int (*get)(uint32_t index, ymod_table *out);
    /* Gives the table at least `rows` rows: where they are now, or 0 when the
     * table cannot grow that far or the game's code is not as traced. */
    uint32_t (*extend)(ymod_mod *mod, const char *name, uint32_t rows);
} ymod_tables;

/* --------------------------------------------------------------------------
 * "yakumo.monsters" 1: large monsters, by name. The game's kinds have ids
 * ("rathian", "tigrex", "gold_rathian": docs/MONSTERS.md lists them), and a
 * mod adds kinds of its own: an id, the game's kind it starts from (its
 * behaviour, size and carves until the mod gives its own) and its model files
 * in the formats docs/MONSTERS.md describes. Yakumo gives the kind its number
 * and its place in the game's tables, and serves the files. Call add() from
 * yakumo_mod_main.
 */
#define YMOD_MONSTERS "yakumo.monsters"
typedef struct ymod_monster {
    uint32_t size;         /* set to sizeof(ymod_monster) before passing one */
    const char *id;        /* the mod's name for the kind: lower case, "fatalis" */
    const char *based_on;  /* a kind of the game by id: "tigrex" */
    const char *model;     /* paths in the mod's folder: the model pack (skeleton, model, textures), */
    const char *geometry;  /* the model's GE data */
    const char *motions;   /* and the motions */
    /* A kind of the game by id whose effects the kind takes (its fire, its
     * roar...), when the mod's model pack has none of its own; NULL: the
     * base's. Effects are entries 3 and on of a model pack (docs/MONSTERS.md). */
    const char *effects;
    /* A path in the mod's folder: the kind's picture in a quest's description,
     * a PNG of 42 by 42 pixels with at most 16 colours; NULL: the base's. A
     * picture that cannot be used is reported and the base's shown. */
    const char *picture;
} ymod_monster;
/* The parts of a monster's behaviour a mod can give a kind it added. Each is a
 * function the game calls on the monster; the base's runs where the mod gives
 * none. More parts are named as they are traced (docs/MONSTERS.md). */
enum ymod_monster_part {
    YMOD_MONSTER_THINK = 0, /* each frame on a quest: decide what to do */
    /* then: carry out the action decided. When a mod does THINK itself
     * (YMOD_RETURN) and gives no ACT, the base's ACT is left out in that
     * frame, as it would carry out the base's own decision. */
    YMOD_MONSTER_ACT = 1,
    /* the sounds and effects of the motion playing, from the base's tables by
     * motion slot; left out as ACT is when the mod does THINK itself, since
     * the base's slots are not the mod's motions */
    YMOD_MONSTER_EFFECTS = 2
};
/* A point in the game's world: its units, y up. */
typedef struct ymod_vec3 {
    float x, y, z;
} ymod_vec3;
/* A sphere an attack hits with, on one of the kind's joints (numbered as in
 * its skeleton): its radius and centre from the joint, in the joint's frame. */
typedef struct ymod_hit_sphere {
    int32_t joint;
    float radius;
    float x, y, z;
} ymod_hit_sphere;
/* `monster` is the monster's object. Return YMOD_RETURN when it has done the
 * part itself, YMOD_CONTINUE to have the base's run as well (after it). */
typedef int (*ymod_monster_fn)(ymod_cpu *cpu, uint32_t monster, void *user);
/* A capsule of a monster's body, where hunters' hits land (docs/MONSTERS.md,
 * "A monster's body capsules"): from `from` in joint `joint`'s frame to `to`
 * in joint `joint_to`'s, `radius` round, or a sphere at `from` when joint_to
 * is -1. `part` is the body part and `zone` the hit zone word, as the base
 * kind's own capsules give them. */
typedef struct ymod_body_capsule {
    int32_t joint, joint_to;
    int32_t part;
    uint32_t zone;
    float radius;
    ymod_vec3 from, to;
} ymod_body_capsule;
/* A footstep (docs/MONSTERS.md, "A monster's footsteps"): in motion `motion`
 * at its frame `frame`, joint `joint` comes down, raising the dust of a step
 * of size `size` (the game's kinds of step, 0 to 9) on the ground the monster
 * stands on. */
typedef struct ymod_footstep {
    int32_t motion;
    float frame;
    int32_t joint;
    int32_t size;
} ymod_footstep;
typedef struct ymod_monsters {
    uint32_t version;
    uint32_t size;
    /* Adds a kind: its number, or -1 (the reason is in the log). */
    int32_t (*add)(ymod_mod *mod, const ymod_monster *monster);
    /* A kind's number by id, the game's or a mod's; -1 for an id nothing has. */
    int32_t (*kind)(const char *id);
    /* A kind's id, or NULL for a number nothing has. */
    const char *(*id_of)(int32_t kind);
    /* Gives a kind this mod added its own part of behaviour: `fn` runs each
     * time the game asks one of its monsters for that part. 0, or -1 (the
     * reason is in the log). */
    int (*behave)(ymod_mod *mod, int32_t kind, int part, ymod_monster_fn fn, void *user);
    /* What a part of behaviour does, called from its fn on the monster it was
     * given. Actions are queued and the game carries them out, in order, as
     * soon as fn returns (before the base's part, when fn lets that run). */
    /* Plays `motion` (the slot in the kind's motions file) on `monster`,
     * blending from what it plays now over `blend` frames. 0, or -1 outside a
     * part of behaviour. */
    int (*play_motion)(uint32_t monster, int32_t motion, int32_t blend);
    /* Whether the motion `monster` plays has reached its last frame (a looping
     * motion never has): 1 or 0, or -1 when its model cannot be read. */
    int (*motion_finished)(uint32_t monster);
    /* The frame the motion playing has reached: 0, or -1 as above. */
    int (*motion_frame)(uint32_t monster, float *frame);
    /* Whether the motion passes `frame` in this frame's step, the way the
     * game's behaviours time what they do: 1 or 0, or -1 as above. */
    int (*at_frame)(uint32_t monster, float frame);
    /* The monster's body (docs/MONSTERS.md, "A monster's body"). Facings are
     * in degrees, 0 towards +z and growing towards +x. Each returns 0, or -1
     * for an address that is not guest memory. During THINK the position's y
     * is -20000: the game sets the ground height later in the frame. */
    int (*position)(uint32_t monster, ymod_vec3 *out);
    int (*facing)(uint32_t monster, float *degrees);
    int (*set_facing)(uint32_t monster, float degrees);
    /* Where the monster is going: the hunter it is after, kept up to date by
     * the game. */
    int (*target)(uint32_t monster, ymod_vec3 *out);
    /* Has the game bring the target up to date from the hunter the monster
     * is after (it does so on its own only now and then); queued as
     * play_motion is. Call it each frame the target matters. */
    int (*aim)(uint32_t monster);
    /* Turns towards the target by at most `degrees`: 1 when it faces the
     * target now, 0 when not yet, -1 as above. */
    int (*turn_to_target)(uint32_t monster, float degrees);
    /* Moves it by (x, z) in its own frame: z ahead, x to the side, as in its
     * motions. A motion turns a monster but does not carry it: its behaviour
     * moves it, a little each frame. */
    int (*move)(uint32_t monster, float x, float z);
    /* Attacks, queued as play_motion is: starts attack `attack` (a row of the
     * kind's attack table, 0 to 63: the base's, or one given with attack_row)
     * in one of the monster's two attack slots, 0 or 1, or ends the attack in
     * a slot. 0, or -1 outside a part of behaviour or for a slot or row past
     * those. Past the base's own rows, a row the kind was not given is what
     * follows them in memory. */
    int (*attack)(uint32_t monster, int32_t slot, int32_t attack);
    int (*end_attack)(uint32_t monster, int32_t slot);
    /* Gives a kind this mod added its own spheres (1 to 16) for one of the
     * base kind's attack shapes, by number: what the base's attack rows that
     * name the shape hit with, on the kind's own joints instead of the
     * base's (docs/MONSTERS.md, "A monster's attacks"). Call at start; it
     * takes effect in monsters with a part of behaviour. 0, or -1 (the
     * reason is in the log). */
    int (*attack_shape)(ymod_mod *mod, int32_t kind, int32_t shape, const ymod_hit_sphere *spheres, int32_t count);
    /* Starts one of the kind's effects (in its model pack, docs/MONSTERS.md),
     * by number, on one of its joints, `offset` from it (NULL: on the joint)
     * and at `scale` (1: as made); queued as play_motion is. 0, or -1 outside
     * a part of behaviour, for a number the kind's effects do not have, or
     * when its model set is not loaded. */
    int (*effect)(uint32_t monster, int32_t effect, int32_t joint, const ymod_vec3 *offset, float scale);
    /* Its health now and at most (either may be NULL); now is 0 once it has
     * fallen. 0, or -1 for an address that is not guest memory. */
    int (*health)(uint32_t monster, int32_t *now, int32_t *most);
    /* Gives a kind this mod added its own body capsules (1 to 64), in place
     * of the base kind's, which are on the base's joints: where hunters'
     * hits land. Call at start; it takes effect in monsters with a part of
     * behaviour. 0, or -1 (the reason is in the log). */
    int (*body_shapes)(ymod_mod *mod, int32_t kind, const ymod_body_capsule *capsules, int32_t count);
    /* Gives a kind this mod added an attack row of its own: row `row` (1 to
     * 63) of its attack table becomes a copy of the base kind's row `like`
     * (what it does: its strength, its kind of hit) that hits with shape
     * `shape` (0 to 31), the base's or one given with attack_shape. Rows and
     * shapes past the base's own may be given, so an attack can have a shape
     * no other uses. Call at start; it takes effect in monsters with a part
     * of behaviour. 0, or -1 (the reason is in the log). */
    int (*attack_row)(ymod_mod *mod, int32_t kind, int32_t row, int32_t like, int32_t shape);
    /* Gives a kind this mod added its footsteps (1 to 256, at most 255 in one
     * motion): after each frame
     * its THINK did alone (YMOD_RETURN), the game plays those of the motion
     * playing, as a monster's own effects part plays the base's.
     * Call at start. 0, or -1 (the reason is in the log). */
    int (*footsteps)(ymod_mod *mod, int32_t kind, const ymod_footstep *steps, int32_t count);
} ymod_monsters;

/* --------------------------------------------------------------------------
 * "yakumo.quests" 1: quests a mod describes, built by Yakumo into the game's
 * quest format (docs/QUESTS.md) and served at the counter, among the event
 * quests, and to the developer tools. A hunt for now: its map, its large
 * monsters and where they start, the objective, time, fee and reward, and its
 * texts. Call add() from yakumo_mod_main, after adding the monsters it names.
 */
#define YMOD_QUESTS "yakumo.quests"
typedef struct ymod_quest_monster {
    const char *monster; /* a kind by id, the game's or a mod's: "fatalis" */
    int32_t area;        /* the area of the quest's map it starts in; 0 in a quest played in an added area
                            (ymod_quest.area): that area's */
    ymod_vec3 at;        /* and where in it */
    int32_t health;      /* the game's health level: higher is more (0x1E: a Tigrex-based kind's 5400) */
    int32_t attack;      /* the game's attack level (0x13: a low rank hunt; 0x30 hits three times as hard) */
    int32_t size;        /* percent of the kind's size; 0: 100 */
} ymod_quest_monster;
typedef struct ymod_quest_texts {
    /* Shown in every language for now; NUL-terminated UTF-8, "\n" for a new line. */
    const char *name, *objective, *failure, *description, *monsters, *client;
} ymod_quest_texts;
typedef struct ymod_quest_reward {
    const char *item; /* the item, by its name as the game shows it in English: "Wyvern Gem" */
    uint32_t count;   /* how many, 1 to 99 */
    uint32_t chance;  /* percent: a group's chances add up to 100 */
    uint32_t line;    /* the reward group it is drawn from: 0 the main reward, drawn several times; 1 and 2
                         two more, as the game's quests have (a commendation, say) */
} ymod_quest_reward;
typedef struct ymod_quest_supply {
    const char *item; /* by its name as the game shows it in English: "First-aid Med" */
    uint32_t count;   /* how many, 1 to 99 */
} ymod_quest_supply;
typedef struct ymod_quest {
    uint32_t size; /* set to sizeof(ymod_quest) before passing one */
    uint32_t id;   /* 62000 to 65535 */
    int32_t map;   /* the game's map number: 8 is the arena; not read when `area` names an added area */
    int32_t stars;
    uint32_t minutes, fee, reward;
    const char *target; /* the objective: hunt `count` of this kind, by id */
    uint32_t count;
    const ymod_quest_monster *monsters; /* 1 to 5 large monsters; the first are the main ones */
    uint32_t monster_count;
    ymod_quest_texts texts;
    const char *area; /* NULL, or an area of the mod's (yakumo.maps, added first) the quest is played in; `map`
                         is then that area's map, whatever it says, and a monster whose area is 0 starts in its
                         area of that map */
    const ymod_quest_reward *rewards; /* its rewards' items, by group; the quest is built again with them once
                                         the game's text is loaded, so they are named as the game names them */
    uint32_t reward_count;
    const ymod_quest_supply *supplies; /* what the supply box holds at the start (up to 32), named as rewards are */
    uint32_t supply_count;
} ymod_quest;
/* --------------------------------------------------------------------------
 * "yakumo.equipment" 1: weapons mods add. A weapon starts from one of the
 * game's of its class (its attack, element, sharpness, slots, rarity and
 * upgrades) and has its own model, name, description and price
 * (docs/EQUIPMENT_MODS.md), and armor the same way. Call add_weapon() and
 * add_armor() from yakumo_mod_main.
 */
#define YMOD_EQUIPMENT "yakumo.equipment"
typedef struct ymod_weapon {
    uint32_t size;           /* set to sizeof(ymod_weapon) before passing one */
    const char *id;          /* the mod's name for it */
    const char *kind;        /* its class: "great_sword", "sword_and_shield", "hammer", "lance", "heavy_bowgun",
                                "light_bowgun", "long_sword", "switch_axe", "gunlance", "bow", "dual_blades",
                                "hunting_horn" */
    int32_t based_on;        /* the game's weapon of that class it starts from, by its id there (from 1) */
    const char *model;       /* a path in the mod's folder: its model pack (model, skeleton, textures) */
    const char *name;        /* NUL-terminated UTF-8 */
    const char *description; /* "\n" for a new line */
    uint32_t price;          /* 0: the base's */
    int32_t give;            /* 1: put one in the equipment box when the village loads, if there is none */
} ymod_weapon;
/* An armor piece starts from one of the game's of its part (its defence,
 * resistances, skills, slots and who may wear it) and has its own models (a
 * male and a female one), name, description and price. */
typedef struct ymod_armor {
    uint32_t size;            /* set to sizeof(ymod_armor) before passing one */
    const char *id;           /* the mod's name for it */
    const char *part;         /* "head", "chest", "arms", "waist" or "legs" */
    int32_t based_on;         /* the game's piece of that part it starts from, by its id there (from 1) */
    const char *model_male;   /* a path in the mod's folder: its model file for a male hunter */
    const char *model_female; /* and for a female one; NULL: the male's */
    const char *name;         /* NUL-terminated UTF-8 */
    const char *description;  /* "\n" for a new line */
    uint32_t price;           /* 0: the base's */
    int32_t give;             /* 1: put one in the equipment box when the village loads, if there is none */
} ymod_armor;
typedef struct ymod_equipment {
    uint32_t version;
    uint32_t size;
    /* Adds a weapon: its id in its class, or -1 (the reason is in the log). */
    int32_t (*add_weapon)(ymod_mod *mod, const ymod_weapon *weapon);
    /* Adds an armor piece: its id in its part, or -1 (the reason is in the log). */
    int32_t (*add_armor)(ymod_mod *mod, const ymod_armor *armor);
} ymod_equipment;

/* --------------------------------------------------------------------------
 * "yakumo.maps" 1: areas mods add. An area takes the place of one of the
 * game's (an area of one of its maps) on the mod's quests that name it
 * (ymod_quest.area): its model, GE data, textures and collision (a block for
 * its ground and one for its walls) are served
 * instead of the game's while such a quest is played; the rest of the area
 * (its scene, camera paths, minimap) stays the game's (docs/MAPS.md). Call
 * add_area() from yakumo_mod_main, before the quests that name it.
 */
#define YMOD_MAPS "yakumo.maps"
typedef struct ymod_area {
    uint32_t size;          /* set to sizeof(ymod_area) before passing one */
    const char *id;         /* the mod's name for it */
    int32_t map, area;      /* the game's area it takes the place of: map 8's area 1 is the arena's */
    const char *model;      /* paths in the mod's folder: its model (PMO "102", static), */
    const char *geometry;   /* that model's GE data, */
    const char *textures;   /* its textures (TMH), */
    const char *ground;     /* its ground's collision block ("4.00"): what stands on it, */
    const char *walls;      /* and its walls', ceilings' and steep slopes': what runs into them */
} ymod_area;
typedef struct ymod_maps {
    uint32_t version;
    uint32_t size;
    /* Adds an area: 0, or -1 (the reason is in the log). */
    int32_t (*add_area)(ymod_mod *mod, const ymod_area *area);
} ymod_maps;

typedef struct ymod_quests {
    uint32_t version;
    uint32_t size;
    /* Builds and serves the quest: its id, or -1 (the reason is in the log). */
    int32_t (*add)(ymod_mod *mod, const ymod_quest *quest);
} ymod_quests;

#ifdef __cplusplus
}
#endif

#endif /* YAKUMO_MOD_H */
