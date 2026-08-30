#if defined(DEBUG)

#include "test/input_script.h"

#include "port/sdl/sdl_app.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/ui/frame_trace.h"

#include <SDL3/SDL_stdinc.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_SCRIPT_MAX_DIRECTIVES 16384
#define INPUT_SCRIPT_LABEL_MAX_LEN 64
#define INPUT_SCRIPT_LINE_MAX_LEN 256
#define INPUT_SCRIPT_QUIT_GRACE_FRAMES 120
#define INPUT_SCRIPT_EXIT_CODE_FAILED_TO_START 3

typedef enum InputScriptDirectiveType {
    INPUT_SCRIPT_DIRECTIVE_HOLD,
    INPUT_SCRIPT_DIRECTIVE_LABEL,
    INPUT_SCRIPT_DIRECTIVE_TELEPORT,
    INPUT_SCRIPT_DIRECTIVE_GUARD,
    INPUT_SCRIPT_DIRECTIVE_QUIT,
} InputScriptDirectiveType;

/* Step H3 (docs/plan-frame-data-harness.md section 1.5) dummy guard/
 * stance modes for the G directive. See input_script_apply_guard_mode()
 * for what each one pokes. */
typedef enum InputScriptGuardMode {
    INPUT_SCRIPT_GUARD_NONE,   /* "none": no guard, standing */
    INPUT_SCRIPT_GUARD_STAND,  /* "stand": guard-all, standing */
    INPUT_SCRIPT_GUARD_CROUCH, /* "crouch": guard-all, crouching */
} InputScriptGuardMode;

typedef struct InputScriptDirective {
    InputScriptDirectiveType type;
    union {
        struct {
            u16 p1_word;
            u16 p2_word;
            int frames;
        } hold;
        struct {
            char text[INPUT_SCRIPT_LABEL_MAX_LEN];
        } label;
        struct {
            int p1_x;
            int p2_x;
        } teleport;
        struct {
            int mode;
        } guard;
    } data;
} InputScriptDirective;

static InputScriptDirective g_directives[INPUT_SCRIPT_MAX_DIRECTIVES];
static int g_directive_count = 0;
static int g_directive_index = 0;

static bool g_loaded = false;

static u16 g_hold_p1_word = 0;
static u16 g_hold_p2_word = 0;
static int g_hold_frames_remaining = 0;

static bool g_quit_seen = false;
static int g_quit_grace_remaining = 0;
static bool g_wants_exit = false;
static bool g_exit_requested = false;

static bool g_guard_default_applied = false;

/* Task #136 (R1). The dummy's blocking state, re-asserted every tick by
 * InputScript_Tick(). See input_script_apply_guard_mode() for why the
 * training ALL-GUARD DIP alone is not enough under arcade balance. */
static s8 g_dummy_auto_guard = 0;

/* Fatal parse/setup error: print to stderr and terminate the process
 * immediately (never returns). Used both for file/line-level errors
 * during InputScript_Load() and for the "wrong mode" check in
 * InputScript_RequireTrainingModePreset() - both mean the script could
 * never run, and the caller must not be allowed to silently continue.
 *
 * This calls exit() rather than routing through main.c's cleanup()/
 * ConsoleMode_Exit() shutdown path, matching existing init-time-failure
 * precedent elsewhere in the codebase (main.c's ConsoleMode_Enter()
 * failure at "Failed to acquire Linux console" also calls a bare
 * exit(1); args.c's Args_Fail() does the same for CLI parse errors).
 * cleanup() itself only does AFS_Finish()/SDLApp_Quit(), which the OS
 * reclaims on process exit regardless. The VT-restore concern doesn't
 * apply either: ConsoleMode_Enter() (port/linux/console_mode.c) only
 * does anything on PORT_MISTER+Linux builds - this harness's current
 * scope is host/macOS only, where ConsoleMode_Enter/Exit are no-ops -
 * and even on a Linux build, ConsoleMode_Enter() registers its own
 * ConsoleMode_Exit() via atexit() on success, so a plain exit() here
 * still restores KD_TEXT before the process dies. */
static void fail_to_start(const char* fmt, ...) {
    fprintf(stderr, "[input_script] ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(INPUT_SCRIPT_EXIT_CODE_FAILED_TO_START);
}

/* Pokes the training dummy's guard mode (Training[].contents[0][0][1] -
 * consumed per-frame by effect_E3_move's switch, effe3.c:178-260, which
 * derives the DIP guard/parry flags on spmv_ng_flag) and stance/ACTION
 * setting (Training[].contents[0][0][0], latched into control_pl_rno at
 * menu->gameplay transition and re-read every frame by Control_Player_Tr(),
 * menu.c:3798-3827, to force the dummy's input word).
 *
 * Writes both Training[0] (the live copy) and Training[2] (the copy
 * Setup_NTr_Data, menu.c:4451, restores into Training[0] on a round
 * reset) so the setting survives a reset, and pokes control_pl_rno
 * directly for immediate effect this frame (it was only latched once,
 * at gameplay entry - re-deriving it from Training[0] wouldn't happen
 * again on its own).
 *
 * Guard slot values (effe3.c:178-260): 0=AUTO, 1=NO GUARD, 2=ALL GUARD,
 * 3=PARRYING. Stance/control_pl_rno values: 0=STAND, 1=CROUCH (also
 * 2=JUMP, 99=UNFORCED, unused here - see DummyAction, workuser.h:55-60).
 *
 * Does NOT touch spmv_ng_flag/Guard_Type directly - both are re-derived
 * every frame from the contents slot (effe3.c:154-235; com_pl.c:589-591)
 * so poking them here would just get overwritten next frame. */
static void input_script_apply_guard_mode(int mode) {
    s8 guard_slot;
    s8 stance_slot;

    switch ((InputScriptGuardMode)mode) {
    case INPUT_SCRIPT_GUARD_STAND:
        guard_slot = 2; /* ALL GUARD */
        stance_slot = DUMMY_ACTION_STAND;
        break;

    case INPUT_SCRIPT_GUARD_CROUCH:
        guard_slot = 2; /* ALL GUARD */
        stance_slot = DUMMY_ACTION_CROUCH;
        break;

    case INPUT_SCRIPT_GUARD_NONE:
    default:
        guard_slot = 1; /* NO GUARD */
        stance_slot = DUMMY_ACTION_STAND;
        break;
    }

    Training[0].contents[0][0][0] = stance_slot;
    Training[0].contents[0][0][1] = guard_slot;
    Training[2].contents[0][0][0] = stance_slot;
    Training[2].contents[0][0][1] = guard_slot;
    control_pl_rno = (u8)stance_slot;

    /* Task #136 (R1): guard slot 2 (ALL GUARD) is not sufficient to make the
     * dummy block under ARCADE balance, and that is the whole reason
     * corpus-q-arcade.yaml originally shipped with all 22 of corpus-q.yaml's
     * BLOCK entries omitted.
     *
     * Guard slot 2 clears DIP_AUTO_GUARD_DISABLED and DIP_GUARD_DISABLED
     * (effe3.c:211-216, sw2_case_2). The PS2 ground-defense path reads the
     * former back as its `ags` term and lets it stand in for holding back:
     *
     *   hitcheck.c:1365-1366  ags = (ds->spmv_ng_flag & DIP_AUTO_GUARD_DISABLED) == 0;
     *   hitcheck.c:1496       if (!ds->auto_guard && !ags && (...)) { lever checks }
     *   hitcheck.c:1508/1516  case 8 / case 16: ... && ags == 0
     *
     * defense_ground_cps3() has no `ags` term at all - it never reads
     * DIP_AUTO_GUARD_DISABLED - so its equivalent gate is just
     *
     *   hitcheck.c:1309       if (!ds->auto_guard) { lever checks }
     *
     * and Control_Player_Tr() forces the dummy's input word to neutral for
     * DUMMY_ACTION_STAND (menu.c:3798-3827), so `saishin_lvdir & gddir` is
     * false and defense_ground_cps3() returns 2 (= took the hit). Measured:
     * corpus-smoke.yaml's close-lp-block-vs-stand reads outcome=BLOCK under
     * --test-balance ps2 and outcome=HIT under --test-balance arcade with
     * every numeric field unchanged.
     *
     * `auto_guard` is the field CPS3's own defense path already provides for
     * exactly this - "guard without holding back". Nothing in a normal round
     * sets it (player_mv_0000 zeroes it at round init, plmain.c:123; only the
     * bonus-stage init sets it, plmain2.c:101) because CPS3 has no training
     * mode to set it from. So the harness sets it, the same way it already
     * pokes control_pl_rno and Training[] rather than going through a menu.
     *
     * High/low correctness is NOT papered over: defense_ground_cps3's
     * `switch (as->wu.att.guard & 0x18)` still requires the crouch bit for a
     * low (case 8, hitcheck.c:1320-1321) and its absence for an overhead
     * (case 16, hitcheck.c:1328-1329), which is what `dummy: crouch` vs
     * `dummy: stand` supplies. That is a REAL engine difference from PS2,
     * whose `ags` escapes at :1508/:1516 let a standing ALL-GUARD dummy block
     * a low - so an arcade corpus must spell the stance out where its PS2
     * twin did not have to. PS2 itself is unaffected by this write: its gate
     * is `!ds->auto_guard && !ags && ...`, already false via ags.
     *
     * Applied to plw[1] (P2 = the dummy) - the harness never switches sides,
     * the same fixed convention input_script_apply_teleport() uses. */
    g_dummy_auto_guard = (mode == INPUT_SCRIPT_GUARD_NONE) ? 0 : 1;
    plw[1].auto_guard = g_dummy_auto_guard;
}

/* Task #136 (R2). Restores both players to full vitality; called once per
 * corpus entry, at the entry's `L` directive.
 *
 * The PS2 balance path gets this for free: check_omop_vital() walks
 * vital_new back up to 160 every frame the dummy is idle
 * (plmain.c:1134-1244, omop_vital_ix cases 2/3/4 at :1197-1243), and 90
 * frames of inter_entry_wait is more than enough to undo one normal's
 * damage. That call is arcade-skipped by construction -
 * `if (!ArcadeBalance_IsEnabled()) { check_omop_vital(wk); }`,
 * plmain.c:335-337 - because it implements the port's EXTRA OPTIONS vitality
 * setting (sysdir.c:126-127), which CPS3 has no menu for. So under arcade the
 * dummy's health only ever goes DOWN, and a long corpus grinds it to zero
 * partway through.
 *
 * That is not cosmetic. same_dm_stop() (hitcheck.c:1023-1039) overrides the
 * defender's hitstop with -att.hs_me whenever
 * `(ds->vital_new - ds->dm_vital) < -2`, i.e. whenever the defender is nearly
 * dead. Measured on corpus-q-arcade.yaml's q-crmp-hit-capture-a with a
 * temporary probe inside same_dm_stop:
 *
 *   arcade  vnew=0    dmvital=10  delta=-10  hsme=9  hsyou=-10  fires=1
 *   ps2     vnew=160  dmvital=13  delta=147  hsme=9  hsyou=-10  fires=0
 *
 * att.dipsw, att.hs_me and att.hs_you are IDENTICAL across the two engines
 * there - the only differing input is the dummy's vitality. With the rule
 * firing, dm_stop becomes -9 instead of -10, the dummy leaves hitstun one
 * frame early (def_idle_F 5818 vs 5819 against an identical atk_idle_F 5820),
 * and cr.MP reads adv=-2 instead of the oracle's -1. Those were the two
 * ARCADE-VS-PORT-DIVERGENCE xfails: a dead dummy, not a balance difference.
 *
 * Deliberately NOT fixed by re-enabling check_omop_vital under arcade - that
 * call is a shipping-behaviour decision and the harness has no business
 * changing which engine ships. Restoring vitality from the test harness is
 * the same class of poke as control_pl_rno and Training[] above.
 *
 * Unconditional (not arcade-gated) so both engines run the identical harness;
 * under PS2 it is a no-op in steady state, since check_omop_vital has already
 * restored 160 by the time the next `L` runs - verified by re-running all 73
 * entries of corpus-q.yaml under --test-balance ps2 with this in place, zero
 * golden drift. 160 is check_omop_vital's own ceiling (plmain.c:1212/1238). */
static void input_script_restore_vitality(void) {
    plw[0].wu.vital_new = 160;
    plw[1].wu.vital_new = 160;
}

/* Teleport both players to absolute X positions, writing the same WORK
 * field frame_data_overlay.c's MOVE_START annotation reads for
 * atk_x/def_x (plw[i].wu.xyz[0].disp.pos). */
static void input_script_apply_teleport(int p1_x, int p2_x) {
    plw[0].wu.xyz[0].disp.pos = (s16)p1_x;
    plw[1].wu.xyz[0].disp.pos = (s16)p2_x;
}

static void strip_comment_and_trim(char* line) {
    /* Everything from the first '#' to end of line is a comment,
     * whether the line is comment-only or a directive with a trailing
     * comment (e.g. "W 0100 0000 2    # punch"). */
    char* hash = strchr(line, '#');
    if (hash != NULL) {
        *hash = '\0';
    }

    size_t n = strlen(line);
    while (n > 0) {
        const char c = line[n - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            line[n - 1] = '\0';
            n -= 1;
        } else {
            break;
        }
    }
}

static char* skip_leading_space(char* s) {
    while (*s == ' ' || *s == '\t') {
        s += 1;
    }
    return s;
}

/* True if p[0] is a recognized directive letter immediately followed by
 * whitespace or end-of-string, e.g. rejects "Wabc" (would otherwise let
 * sscanf's %x eat "abc" as a hex word) and "Label foo" (would otherwise
 * let the L directive's arg parsing eat "abel foo" as the label text). */
static bool directive_letter_is_delimited(const char* p) {
    return p[1] == '\0' || p[1] == ' ' || p[1] == '\t';
}

static void add_directive(const char* path, int line_no, const InputScriptDirective* directive) {
    if (g_directive_count >= INPUT_SCRIPT_MAX_DIRECTIVES) {
        fail_to_start("%s:%d: script exceeds max directive count (%d)", path, line_no, INPUT_SCRIPT_MAX_DIRECTIVES);
    }

    g_directives[g_directive_count] = *directive;
    g_directive_count += 1;
}

/* Parse one already-trimmed, non-empty directive line (comment/blank
 * lines are filtered out by the caller). Fails loudly on anything it
 * doesn't recognize rather than skipping it. */
static void parse_directive_line(const char* path, int line_no, char* p) {
    InputScriptDirective directive;
    SDL_zero(directive);

    switch (p[0]) {
    case 'W': {
        if (!directive_letter_is_delimited(p)) {
            fail_to_start("%s:%d: malformed W directive: \"%s\"", path, line_no, p);
        }
        unsigned int p1_word = 0;
        unsigned int p2_word = 0;
        int frames = 0;
        int consumed = -1;
        sscanf(p + 1, " %x %x %d %n", &p1_word, &p2_word, &frames, &consumed);
        if (consumed < 0 || p[1 + consumed] != '\0') {
            fail_to_start("%s:%d: malformed W directive: \"%s\"", path, line_no, p);
        }
        /* %x also accepts a leading '-' (parsed as an unsigned negation),
         * so this range check doubles as the negative-value rejection: a
         * negative or otherwise out-of-range input wraps to a value well
         * above 0xFFFF here, before it would otherwise be silently
         * truncated by the (u16) cast below. */
        if (p1_word > 0xFFFF || p2_word > 0xFFFF) {
            fail_to_start("%s:%d: W directive word value out of range (must be 0-ffff): \"%s\"", path, line_no, p);
        }
        if (frames < 0) {
            fail_to_start("%s:%d: W directive has a negative frame count: \"%s\"", path, line_no, p);
        }
        directive.type = INPUT_SCRIPT_DIRECTIVE_HOLD;
        directive.data.hold.p1_word = (u16)p1_word;
        directive.data.hold.p2_word = (u16)p2_word;
        directive.data.hold.frames = frames;
        break;
    }

    case 'L': {
        if (!directive_letter_is_delimited(p)) {
            fail_to_start("%s:%d: malformed L directive: \"%s\"", path, line_no, p);
        }
        char* label = skip_leading_space(p + 1);
        if (*label == '\0') {
            fail_to_start("%s:%d: L directive is missing a label", path, line_no);
        }
        if (strlen(label) >= sizeof(directive.data.label.text)) {
            fail_to_start("%s:%d: label exceeds %d characters: \"%s\"", path, line_no,
                          (int)sizeof(directive.data.label.text) - 1, label);
        }
        directive.type = INPUT_SCRIPT_DIRECTIVE_LABEL;
        SDL_strlcpy(directive.data.label.text, label, sizeof(directive.data.label.text));
        break;
    }

    case 'P': {
        if (!directive_letter_is_delimited(p)) {
            fail_to_start("%s:%d: malformed P directive: \"%s\"", path, line_no, p);
        }
        int p1_x = 0;
        int p2_x = 0;
        int consumed = -1;
        sscanf(p + 1, " %d %d %n", &p1_x, &p2_x, &consumed);
        if (consumed < 0 || p[1 + consumed] != '\0') {
            fail_to_start("%s:%d: malformed P directive: \"%s\"", path, line_no, p);
        }
        directive.type = INPUT_SCRIPT_DIRECTIVE_TELEPORT;
        directive.data.teleport.p1_x = p1_x;
        directive.data.teleport.p2_x = p2_x;
        break;
    }

    case 'G': {
        if (!directive_letter_is_delimited(p)) {
            fail_to_start("%s:%d: malformed G directive: \"%s\"", path, line_no, p);
        }
        char* arg = skip_leading_space(p + 1);
        int mode;
        if (SDL_strcmp(arg, "none") == 0) {
            mode = INPUT_SCRIPT_GUARD_NONE;
        } else if (SDL_strcmp(arg, "stand") == 0) {
            mode = INPUT_SCRIPT_GUARD_STAND;
        } else if (SDL_strcmp(arg, "crouch") == 0) {
            mode = INPUT_SCRIPT_GUARD_CROUCH;
        } else {
            fail_to_start("%s:%d: G directive mode must be none, stand, or crouch: \"%s\"", path, line_no, p);
            return; /* unreachable - fail_to_start() exits - keeps analyzers happy */
        }
        directive.type = INPUT_SCRIPT_DIRECTIVE_GUARD;
        directive.data.guard.mode = mode;
        break;
    }

    case 'Q': {
        char* rest = skip_leading_space(p + 1);
        if (*rest != '\0') {
            fail_to_start("%s:%d: Q directive takes no arguments: \"%s\"", path, line_no, p);
        }
        directive.type = INPUT_SCRIPT_DIRECTIVE_QUIT;
        break;
    }

    default:
        fail_to_start("%s:%d: unrecognized directive: \"%s\"", path, line_no, p);
        return; /* unreachable - fail_to_start() exits - keeps analyzers happy */
    }

    add_directive(path, line_no, &directive);
}

void InputScript_Load(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        fail_to_start("could not open \"%s\": %s", path, strerror(errno));
    }

    char raw_line[INPUT_SCRIPT_LINE_MAX_LEN];
    int line_no = 0;

    while (fgets(raw_line, sizeof(raw_line), f) != NULL) {
        line_no += 1;

        const size_t raw_len = strlen(raw_line);
        if (raw_len == sizeof(raw_line) - 1 && raw_line[raw_len - 1] != '\n') {
            /* The buffer filled up without a trailing newline. That's only
             * legitimate if the line's terminating newline (or EOF) falls
             * exactly on the next byte; fgets() can't tell us that on its
             * own since it never reads past a full buffer. Peek one more
             * character to disambiguate rather than silently treating the
             * remainder as a fresh directive line. Note this peek can only
             * ever consume the true newline/EOF that ends *this* line (or
             * prove there's more of this line still unread) - it can't
             * accidentally swallow the start of the next line. */
            const int next_c = fgetc(f);
            if (next_c != EOF && next_c != '\n') {
                fail_to_start("%s:%d: line too long (max %d characters)", path, line_no,
                              (int)sizeof(raw_line) - 1);
            }
        }

        strip_comment_and_trim(raw_line);
        char* p = skip_leading_space(raw_line);

        if (*p == '\0') {
            continue;
        }

        parse_directive_line(path, line_no, p);
    }

    fclose(f);

    g_directive_index = 0;
    g_loaded = true;
}

bool InputScript_IsLoaded(void) {
    return g_loaded;
}

void InputScript_RequireTrainingModePreset(bool scene_preset_uses_training_mode) {
    if (!g_loaded) {
        return;
    }

    if (!scene_preset_uses_training_mode) {
        fail_to_start("--test-input-script requires a training-mode --test-scene-preset "
                      "(e.g. training-yun-ryu-ryu-stage); the script would never run");
    }
}

void InputScript_Tick(u16* p1sw, u16* p2sw) {
    *p1sw = 0;
    *p2sw = 0;

    if (!g_loaded) {
        return;
    }

    if (!g_guard_default_applied) {
        /* Sane harness default the moment training gameplay is actually
         * running (this is the first call once
         * training_mode_gameplay_started() goes true - see
         * TestRunner_Prologue's PHASE_GAME case). Fresh training mode
         * defaults to guard slot 0 (AUTO, Default_Training_Data(),
         * menu.c:5126), which re-derives its behavior every frame from
         * timers (effe3.c:178-260) rather than being a fixed mode - not
         * suitable as a deterministic harness default. A script's own
         * `G` directive overrides this immediately once it runs. */
        g_guard_default_applied = true;
        input_script_apply_guard_mode(INPUT_SCRIPT_GUARD_NONE);
    }

    /* Re-assert the dummy's auto-guard every tick rather than only at the `G`
     * directive: plmain.c:123 zeroes auto_guard in player_mv_0000 (round init)
     * and the PS2 ground/sky defense paths zero it outright whenever
     * Play_Mode != 0 (hitcheck.c:1106, :1371), so a one-shot write is not
     * guaranteed to survive to the next contact. Same reasoning as the
     * control_pl_rno poke in input_script_apply_guard_mode(). */
    plw[1].auto_guard = g_dummy_auto_guard;

    if (g_quit_seen) {
        if (g_quit_grace_remaining > 0) {
            g_quit_grace_remaining -= 1;
        }

        if (g_quit_grace_remaining <= 0 && !g_exit_requested) {
            g_exit_requested = true;
            g_wants_exit = true;
            /* Same shutdown path menu.c's arcade demo-exit and the perf
             * capture completion path use: push SDL_EVENT_QUIT so the
             * main loop's SDLApp_PollEvents() call ends it next tick
             * with shutdown_signal==0, i.e. a clean exit code 0. */
            SDLApp_Exit();
        }

        return;
    }

    if (g_hold_frames_remaining > 0) {
        *p1sw = g_hold_p1_word;
        *p2sw = g_hold_p2_word;
        g_hold_frames_remaining -= 1;
        return;
    }

    while (g_directive_index < g_directive_count) {
        const InputScriptDirective* directive = &g_directives[g_directive_index];
        g_directive_index += 1;

        switch (directive->type) {
        case INPUT_SCRIPT_DIRECTIVE_HOLD:
            if (directive->data.hold.frames <= 0) {
                break;
            }
            g_hold_p1_word = directive->data.hold.p1_word;
            g_hold_p2_word = directive->data.hold.p2_word;
            g_hold_frames_remaining = directive->data.hold.frames - 1;
            *p1sw = g_hold_p1_word;
            *p2sw = g_hold_p2_word;
            return;

        case INPUT_SCRIPT_DIRECTIVE_LABEL:
            input_script_restore_vitality();
            frame_trace_annotate("SCRIPT %s", directive->data.label.text);
            break;

        case INPUT_SCRIPT_DIRECTIVE_TELEPORT:
            input_script_apply_teleport(directive->data.teleport.p1_x, directive->data.teleport.p2_x);
            break;

        case INPUT_SCRIPT_DIRECTIVE_GUARD:
            input_script_apply_guard_mode(directive->data.guard.mode);
            break;

        case INPUT_SCRIPT_DIRECTIVE_QUIT:
            g_quit_seen = true;
            g_quit_grace_remaining = INPUT_SCRIPT_QUIT_GRACE_FRAMES;
            return;
        }
    }

    /* Ran off the end of the script without a Q directive - stop
     * injecting inputs, but don't request an exit; an external wrapper
     * timeout is responsible for reaping a script that forgot Q. */
}

bool InputScript_WantsExit(void) {
    return g_wants_exit;
}

#endif
