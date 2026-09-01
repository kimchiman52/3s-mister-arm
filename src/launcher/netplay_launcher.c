/*
 * netplay_launcher.c — task #157: desktop netplay launcher.
 *
 * A second CMake target (`3s-netplay-launcher`), NOT compiled into the
 * game — CMakeLists.txt filters src/launcher/ out of GAME_SRC the same
 * way it filters netplay_stub.c. Desktop only (Windows/Mac/Linux); the
 * MiSTer OSD already covers hosting/joining on device and is out of
 * scope here.
 *
 * What it does: hosts or joins a direct-P2P game without hand-writing a
 * handoff file. The handoff file is already a control API — the game
 * self-navigates from a cold launch when spawned with
 * `--direct-p2p-handoff <path>` (src/netplay/direct_p2p_handoff.h,
 * netplay_nav.h) — so this launcher only writes that file, spawns the
 * game, and (for hosting) reads the room code back from
 * <pref>/netplay.status, the machine-readable status file
 * direct_p2p.c's netplay_status_publish() maintains. The code in that
 * file is deliberately unredacted: it is local to the user's own pref
 * dir and exists precisely so this launcher does not screen-scrape.
 *
 * Zero new dependencies by construction: rendering is
 * SDL_RenderDebugText (SDL3's built-in 8x8 debug font — independent of
 * the game's proportional font atlas, so bug #156's corrupt '1' glyph
 * cannot reach us), spawning is SDL_CreateProcess (portable — no
 * per-platform fork/exec), clipboard is SDL_SetClipboardText, and the
 * only game code linked in is room_code.c (+ csprng.c it needs) and
 * paths.c (so the pref dir resolves IDENTICALLY to the game's,
 * THIRDSARM_HOME override included).
 *
 * A pasted code is validated with RoomCode_Decode BEFORE spawning, so a
 * typo reports "invalid room code" here instead of costing a 15 s punch
 * at an uninvolved stranger's address — and the decoder's distinct
 * OLD_FORMAT / FUTURE_VERSION outcomes surface as distinct, actionable
 * messages, which is the whole point of the enum return (room_code.h).
 */

#include "netplay/room_code.h"
#include "port/paths.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* Layout constants. Window is fixed 640x480; text is drawn through
 * draw_text() which scales SDL_RenderDebugText's 8px glyphs. */
#define WIN_W 640
#define WIN_H 480
#define GLYPH 8.0f

#define STATUS_POLL_MS 500

typedef enum {
    SCREEN_MENU,
    SCREEN_HOST,
    SCREEN_JOIN,
    SCREEN_LAN,
} Screen;

static SDL_Window* g_win = NULL;
static SDL_Renderer* g_ren = NULL;
static Screen g_screen = SCREEN_MENU;
static bool g_quit = false;

/* Path to the game binary, resolved once at startup. */
static char g_game_path[1024] = { 0 };

/* Spawned game process (at most one). The handle is kept so we can
 * report "game exited"; we never kill it — the process IS the match. */
static SDL_Process* g_child = NULL;
static bool g_child_exited = false;
static int g_child_exit_code = 0;

/* Host screen state. */
static char g_host_code[ROOM_CODE_BUF_LEN] = { 0 }; /* validated, shown big */
static char g_host_status[64] = { 0 };              /* raw line 1 of the file */
static bool g_code_copied = false;
static Uint64 g_next_poll_ms = 0;

/* Join screen state. */
static char g_code_entry[32] = { 0 };

/* LAN screen state. */
static char g_ip_entry[64] = { 0 };
static int g_lan_player = 1;

/* One-line (plus optional second line) message area, per screen. */
static char g_msg[128] = { 0 };
static char g_msg2[128] = { 0 };
static SDL_Color g_msg_color = { 255, 255, 255, 255 };

static const SDL_Color COL_TEXT = { 230, 230, 235, 255 };
static const SDL_Color COL_DIM = { 150, 150, 160, 255 };
static const SDL_Color COL_ACCENT = { 255, 208, 64, 255 };
static const SDL_Color COL_OK = { 96, 220, 128, 255 };
static const SDL_Color COL_ERR = { 255, 96, 96, 255 };

/* ---------------------------------------------------------------------- */
/* Small drawing helpers. */

static void draw_text(float x, float y, float scale, SDL_Color c, const char* s) {
    SDL_SetRenderScale(g_ren, scale, scale);
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    SDL_RenderDebugText(g_ren, x / scale, y / scale, s);
    SDL_SetRenderScale(g_ren, 1.0f, 1.0f);
}

static void draw_text_centered(float cx, float y, float scale, SDL_Color c, const char* s) {
    const float w = (float)strlen(s) * GLYPH * scale;
    draw_text(cx - w / 2.0f, y, scale, c, s);
}

static float g_mouse_x = 0.0f;
static float g_mouse_y = 0.0f;
static bool g_mouse_clicked = false; /* set for one frame on button-up */

typedef struct {
    SDL_FRect rect;
    const char* label;
} Button;

/* Draws the button and returns true when it was clicked this frame. */
static bool button(const Button* b) {
    const bool hover = g_mouse_x >= b->rect.x && g_mouse_x < b->rect.x + b->rect.w &&
                       g_mouse_y >= b->rect.y && g_mouse_y < b->rect.y + b->rect.h;

    if (hover) {
        SDL_SetRenderDrawColor(g_ren, 82, 82, 118, 255);
    } else {
        SDL_SetRenderDrawColor(g_ren, 52, 52, 74, 255);
    }
    SDL_RenderFillRect(g_ren, &b->rect);
    SDL_SetRenderDrawColor(g_ren, 120, 120, 150, 255);
    SDL_RenderRect(g_ren, &b->rect);

    draw_text_centered(b->rect.x + b->rect.w / 2.0f,
                       b->rect.y + (b->rect.h - GLYPH * 2.0f) / 2.0f,
                       2.0f, COL_TEXT, b->label);

    return hover && g_mouse_clicked;
}

static void set_msg(SDL_Color color, const char* line1, const char* line2) {
    SDL_strlcpy(g_msg, line1 != NULL ? line1 : "", sizeof(g_msg));
    SDL_strlcpy(g_msg2, line2 != NULL ? line2 : "", sizeof(g_msg2));
    g_msg_color = color;
}

/* ---------------------------------------------------------------------- */
/* Paths and files. */

static char* status_file_path(void) {
    char* path = NULL;
    SDL_asprintf(&path, "%snetplay.status", Paths_GetPrefPath());
    return path;
}

static char* handoff_file_path(void) {
    char* path = NULL;
    SDL_asprintf(&path, "%slauncher.handoff", Paths_GetPrefPath());
    return path;
}

static bool write_whole_file(const char* path, const char* content) {
    SDL_IOStream* io = SDL_IOFromFile(path, "w");
    if (io == NULL) {
        return false;
    }
    const size_t len = strlen(content);
    const bool ok = SDL_WriteIO(io, content, len) == len;
    SDL_CloseIO(io);
    return ok;
}

/* Overwrite netplay.status with IDLE before a host spawn, so anything
 * the poll loop later reads as HOSTING was necessarily written by the
 * child we just spawned — a code left behind by an earlier crashed run
 * (the one lifecycle hole the game's own teardown clearing cannot
 * cover) can never be re-read. The game clears it at DirectP2P_Init
 * too; either side alone closes the stale-read window, both together
 * make it belt-and-braces. */
static void clear_status_file(void) {
    char* path = status_file_path();
    if (path != NULL) {
        write_whole_file(path, "IDLE\n\n");
        SDL_free(path);
    }
}

/* Reads <pref>/netplay.status into (state, code). Missing file is not
 * an error — state comes back empty. */
static void read_status_file(char* state, size_t state_len, char* code, size_t code_len) {
    state[0] = '\0';
    code[0] = '\0';

    char* path = status_file_path();
    if (path == NULL) {
        return;
    }
    size_t size = 0;
    char* data = (char*)SDL_LoadFile(path, &size);
    SDL_free(path);
    if (data == NULL) {
        return;
    }

    /* Two newline-terminated lines; tolerate a torn/partial write (the
     * writer is not atomic) — a garbled read simply fails RoomCode
     * validation and the next poll re-reads. */
    char* nl = strchr(data, '\n');
    if (nl != NULL) {
        *nl = '\0';
        SDL_strlcpy(state, data, state_len);
        char* second = nl + 1;
        char* nl2 = strchr(second, '\n');
        if (nl2 != NULL) {
            *nl2 = '\0';
        }
        SDL_strlcpy(code, second, code_len);
    }
    SDL_free(data);
}

/* ---------------------------------------------------------------------- */
/* Game process. */

/* Resolve the game binary: --game <path> wins, then THIRDSARM_GAME, then
 * well-known names next to the launcher (SDL_GetBasePath). */
static bool find_game_binary(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--game") == 0) {
            SDL_strlcpy(g_game_path, argv[i + 1], sizeof(g_game_path));
            return true;
        }
    }

    const char* env = SDL_getenv("THIRDSARM_GAME");
    if (env != NULL && env[0] != '\0') {
        SDL_strlcpy(g_game_path, env, sizeof(g_game_path));
        return true;
    }

    const char* base = SDL_GetBasePath();
    if (base == NULL) {
        base = "";
    }
    static const char* const candidates[] = {
        "3S-ARM.app/Contents/MacOS/3S-ARM", /* macOS bundle next to us */
        "3s-arm",                           /* Linux */
        "3s-arm.exe",                       /* Windows */
        "3S-ARM",                           /* macOS non-bundle */
    };
    for (size_t i = 0; i < SDL_arraysize(candidates); i++) {
        char probe[1024];
        SDL_snprintf(probe, sizeof(probe), "%s%s", base, candidates[i]);
        SDL_PathInfo info;
        if (SDL_GetPathInfo(probe, &info) && info.type == SDL_PATHTYPE_FILE) {
            SDL_strlcpy(g_game_path, probe, sizeof(g_game_path));
            return true;
        }
    }
    return false;
}

static bool child_running(void) {
    if (g_child == NULL || g_child_exited) {
        return false;
    }
    int code = 0;
    if (SDL_WaitProcess(g_child, false, &code)) {
        g_child_exited = true;
        g_child_exit_code = code;
        return false;
    }
    return true;
}

/* Spawn the game with the given extra args (NULL-terminated tail is
 * built here). Returns false with a message set on failure. */
static bool spawn_game(const char* const* extra, int nextra) {
    if (child_running()) {
        set_msg(COL_ERR, "The game is already running.", "Close it before starting another.");
        return false;
    }
    if (g_child != NULL) {
        SDL_DestroyProcess(g_child);
        g_child = NULL;
    }
    g_child_exited = false;
    g_child_exit_code = 0;

    const char* args[16];
    int n = 0;
    args[n++] = g_game_path;
    for (int i = 0; i < nextra && n < (int)SDL_arraysize(args) - 1; i++) {
        args[n++] = extra[i];
    }
    args[n] = NULL;

    g_child = SDL_CreateProcess(args, false);
    if (g_child == NULL) {
        set_msg(COL_ERR, "Could not start the game:", SDL_GetError());
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* Mode wiring. */

static bool start_host(void) {
    /* #157 review P-2: the refusal must come BEFORE the clear. Clearing first
     * wiped the status file of a game that was genuinely still hosting, and
     * since the game only republishes on a state transition or a drift
     * re-encode, the launcher then waited forever for a code that had already
     * been published. */
    if (child_running()) {
        set_msg(COL_ERR, "The game is already running.", "Close it before starting another.");
        return false;
    }

    clear_status_file();
    g_host_code[0] = '\0';
    g_host_status[0] = '\0';
    g_code_copied = false;
    g_next_poll_ms = 0;

    char* handoff = handoff_file_path();
    if (handoff == NULL || !write_whole_file(handoff, "mode=host\nport=0\n")) {
        SDL_free(handoff);
        set_msg(COL_ERR, "Could not write the handoff file.", NULL);
        return false;
    }

    const char* extra[] = { "--direct-p2p-handoff", handoff };
    const bool ok = spawn_game(extra, 2);
    SDL_free(handoff);
    if (ok) {
        set_msg(COL_TEXT, "Game starting - waiting for the room code...", NULL);
    }
    return ok;
}

/* Validate + dispatch a join. The decode runs BEFORE any spawn: a bad
 * code must cost an error line here, not a 15 s hole-punch at whoever
 * owns the mistyped address. */
static void start_join(void) {
    if (g_code_entry[0] == '\0') {
        set_msg(COL_ERR, "Enter a room code first.", NULL);
        return;
    }

    uint32_t ip_be = 0;
    uint16_t port = 0;
    switch (RoomCode_Decode(g_code_entry, &ip_be, &port)) {
    case ROOM_CODE_OK:
        break;
    case ROOM_CODE_MALFORMED:
        SDL_Log("[launcher] join refused: ROOM_CODE_MALFORMED");
        set_msg(COL_ERR, "Invalid room code - check for typos.",
                "Codes are 12 letters/digits.");
        return;
    case ROOM_CODE_OLD_FORMAT:
        SDL_Log("[launcher] join refused: ROOM_CODE_OLD_FORMAT");
        set_msg(COL_ERR, "This code is from an OLDER game version.",
                "Ask the host to update, then use their new code.");
        return;
    case ROOM_CODE_FUTURE_VERSION:
        SDL_Log("[launcher] join refused: ROOM_CODE_FUTURE_VERSION");
        /* #157 review P-2: RoomCode_Decode gates the version char BEFORE the
         * checksum, so ANY 12-char string not starting with '4' lands here --
         * including a mis-heard first character, which is a typo, not a version
         * skew. Do not tell the user to update on that evidence alone. */
        set_msg(COL_ERR, "Code not recognised - it may be from a NEWER game version.",
                "Check the first character, or ask the host to confirm the code.");
        return;
    default:
        SDL_Log("[launcher] join refused: unrecognized decode result");
        set_msg(COL_ERR, "Could not read that room code.", NULL);
        return;
    }
    SDL_Log("[launcher] join code validated OK - spawning the game");

    char* handoff = handoff_file_path();
    char content[128];
    SDL_snprintf(content, sizeof(content), "mode=join\npeer_code=%s\n", g_code_entry);
    if (handoff == NULL || !write_whole_file(handoff, content)) {
        SDL_free(handoff);
        set_msg(COL_ERR, "Could not write the handoff file.", NULL);
        return;
    }

    const char* extra[] = { "--direct-p2p-handoff", handoff };
    const bool ok = spawn_game(extra, 2);
    SDL_free(handoff);
    if (ok) {
        set_msg(COL_OK, "Game launched - connecting to the host.",
                "Watch the game window for progress.");
    }
}

static void start_lan(void) {
    if (g_ip_entry[0] == '\0') {
        set_msg(COL_ERR, "Enter the other player's IP address first.", NULL);
        return;
    }
    char player_str[8];
    SDL_snprintf(player_str, sizeof(player_str), "%d", g_lan_player);
    const char* extra[] = { "--p2p-remote-ip", g_ip_entry, "--p2p-local-player", player_str };
    if (spawn_game(extra, 4)) {
        set_msg(COL_OK, "Game launched - connecting over LAN.",
                "The other player picks the other player number.");
    }
}

/* Host screen poll: read netplay.status on a cadence; when a valid code
 * appears (or a drift re-encode replaces it), show it and copy it to
 * the clipboard. */
static void host_poll(void) {
    const Uint64 now = SDL_GetTicks();
    if (now < g_next_poll_ms) {
        return;
    }
    g_next_poll_ms = now + STATUS_POLL_MS;

    char state[32];
    char code[64];
    read_status_file(state, sizeof(state), code, sizeof(code));
    SDL_strlcpy(g_host_status, state, sizeof(g_host_status));

    /* #157 review P-1: netplay.status is written by the game, and NOTHING on
     * the game's own shutdown path clears it -- a graceful quit (Cmd+Q) while
     * hosting leaves HOSTING+code on disk. Polling the file alone would keep
     * presenting a dead endpoint as live, and the user would hand that code to
     * someone who then punches a stranger's address for 15 s. The child's
     * liveness is the authority here, not the file: a code is live only while
     * the process that published it is still running. */
    if (!child_running()) {
        if (g_host_code[0] != '\0') {
            g_host_code[0] = '\0';
            g_code_copied = false;
            set_msg(COL_ERR, "The game exited - that room code is no longer valid.",
                    "Start hosting again to get a new one.");
        }
        SDL_strlcpy(g_host_status, "IDLE", sizeof(g_host_status));
        return;
    }

    if (strcmp(state, "HOSTING") == 0) {
        uint32_t ip_be = 0;
        uint16_t port = 0;
        if (RoomCode_Decode(code, &ip_be, &port) == ROOM_CODE_OK &&
            strcmp(code, g_host_code) != 0) {
            SDL_strlcpy(g_host_code, code, sizeof(g_host_code));
            g_code_copied = SDL_SetClipboardText(g_host_code);
            set_msg(COL_OK,
                    g_code_copied ? "Room code copied to the clipboard."
                                  : "Room code ready (clipboard copy failed).",
                    "Send it to the other player.");
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Text entry. */

static void entry_append_filtered(char* buf, size_t buflen, const char* text, bool code_chars) {
    for (const char* p = text; *p != '\0'; p++) {
        const char c = *p;
        bool ok;
        if (code_chars) {
            /* Room-code chars; the decoder tolerates dashes/spaces. */
            ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z') || c == '-' || c == ' ';
        } else {
            /* IP / hostname chars. */
            ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z') || c == '.' || c == ':' || c == '-';
        }
        const size_t len = strlen(buf);
        if (ok && len + 1 < buflen) {
            buf[len] = c;
            buf[len + 1] = '\0';
        }
    }
}

static char* active_entry_buf(void) {
    if (g_screen == SCREEN_JOIN) {
        return g_code_entry;
    }
    if (g_screen == SCREEN_LAN) {
        return g_ip_entry;
    }
    return NULL;
}

static size_t active_entry_cap(void) {
    return g_screen == SCREEN_JOIN ? sizeof(g_code_entry) : sizeof(g_ip_entry);
}

static void enter_screen(Screen s) {
    g_screen = s;
    set_msg(COL_TEXT, "", NULL);
    if (s == SCREEN_JOIN || s == SCREEN_LAN) {
        SDL_StartTextInput(g_win);
    } else {
        SDL_StopTextInput(g_win);
    }
}

/* ---------------------------------------------------------------------- */
/* Screens. */

static void render_child_state(float y) {
    if (g_child == NULL) {
        return;
    }
    if (child_running()) {
        draw_text(24, y, 1.0f, COL_DIM, "game process: running");
    } else {
        char line[64];
        SDL_snprintf(line, sizeof(line), "game process: exited (code %d)", g_child_exit_code);
        draw_text(24, y, 1.0f, COL_DIM, line);
    }
}

static void render_msg(float y) {
    if (g_msg[0] != '\0') {
        draw_text_centered(WIN_W / 2.0f, y, 2.0f, g_msg_color, g_msg);
    }
    if (g_msg2[0] != '\0') {
        draw_text_centered(WIN_W / 2.0f, y + 24, 1.0f, COL_DIM, g_msg2);
    }
}

static void screen_menu(void) {
    draw_text_centered(WIN_W / 2.0f, 48, 3.0f, COL_ACCENT, "3S NETPLAY");
    draw_text_centered(WIN_W / 2.0f, 84, 1.0f, COL_DIM, "host or join a match without editing files");

    const Button b_host = { { 170, 140, 300, 52 }, "HOST A GAME" };
    const Button b_join = { { 170, 210, 300, 52 }, "JOIN WITH A CODE" };
    const Button b_lan = { { 170, 280, 300, 52 }, "LAN GAME" };

    if (button(&b_host)) {
        enter_screen(SCREEN_HOST);
        start_host();
    }
    if (button(&b_join)) {
        enter_screen(SCREEN_JOIN);
    }
    if (button(&b_lan)) {
        enter_screen(SCREEN_LAN);
    }

    render_msg(360);
    render_child_state(440);
    draw_text_centered(WIN_W / 2.0f, 456, 1.0f, COL_DIM, "esc quits");
}

static void screen_host(void) {
    draw_text_centered(WIN_W / 2.0f, 40, 2.0f, COL_TEXT, "HOSTING");

    host_poll();

    if (g_host_code[0] != '\0') {
        draw_text_centered(WIN_W / 2.0f, 96, 1.0f, COL_DIM, "give this code to the other player:");
        draw_text_centered(WIN_W / 2.0f, 130, 4.0f, COL_ACCENT, g_host_code);

        const Button b_copy = { { 220, 200, 200, 40 }, "COPY AGAIN" };
        if (button(&b_copy)) {
            if (SDL_SetClipboardText(g_host_code)) {
                set_msg(COL_OK, "Copied.", NULL);
            } else {
                set_msg(COL_ERR, "Clipboard copy failed.", SDL_GetError());
            }
        }
        if (strcmp(g_host_status, "HOSTING") != 0) {
            draw_text_centered(WIN_W / 2.0f, 260, 1.0f, COL_DIM,
                               "no longer advertising - peer connected, or hosting ended");
        }
    } else {
        draw_text_centered(WIN_W / 2.0f, 140, 2.0f, COL_DIM, "waiting for the room code...");
        draw_text_centered(WIN_W / 2.0f, 170, 1.0f, COL_DIM,
                           "the game is discovering its public address");
    }

    render_msg(320);

    const Button b_back = { { 240, 390, 160, 40 }, "BACK" };
    if (button(&b_back)) {
        enter_screen(SCREEN_MENU);
    }
    render_child_state(440);
    draw_text_centered(WIN_W / 2.0f, 456, 1.0f, COL_DIM,
                       "back leaves the game running - quit it from the game window");
}

static void render_entry_field(const char* value, const char* placeholder) {
    const SDL_FRect field = { 120, 150, 400, 44 };
    SDL_SetRenderDrawColor(g_ren, 34, 34, 46, 255);
    SDL_RenderFillRect(g_ren, &field);
    SDL_SetRenderDrawColor(g_ren, 120, 120, 150, 255);
    SDL_RenderRect(g_ren, &field);

    if (value[0] != '\0') {
        draw_text(field.x + 12, field.y + 14, 2.0f, COL_TEXT, value);
    } else {
        draw_text(field.x + 12, field.y + 14, 2.0f, COL_DIM, placeholder);
    }
    /* Blinking caret. */
    if ((SDL_GetTicks() / 500) % 2 == 0) {
        const float cx = field.x + 12 + (float)strlen(value) * GLYPH * 2.0f;
        const SDL_FRect caret = { cx, field.y + 12, 3, 20 };
        SDL_SetRenderDrawColor(g_ren, 230, 230, 235, 255);
        SDL_RenderFillRect(g_ren, &caret);
    }
}

static void screen_join(void) {
    draw_text_centered(WIN_W / 2.0f, 40, 2.0f, COL_TEXT, "JOIN WITH A CODE");
    draw_text_centered(WIN_W / 2.0f, 110, 1.0f, COL_DIM,
                       "type or paste the host's room code (ctrl+v / cmd+v)");

    render_entry_field(g_code_entry, "room code");

    const Button b_go = { { 240, 220, 160, 44 }, "JOIN" };
    if (button(&b_go)) {
        start_join();
    }

    render_msg(300);

    const Button b_back = { { 240, 390, 160, 40 }, "BACK" };
    if (button(&b_back)) {
        enter_screen(SCREEN_MENU);
    }
    render_child_state(440);
    draw_text_centered(WIN_W / 2.0f, 456, 1.0f, COL_DIM, "enter joins - esc goes back");
}

static void screen_lan(void) {
    draw_text_centered(WIN_W / 2.0f, 40, 2.0f, COL_TEXT, "LAN GAME");
    draw_text_centered(WIN_W / 2.0f, 110, 1.0f, COL_DIM,
                       "no room code needed - enter the other machine's IP");

    render_entry_field(g_ip_entry, "192.168.1.50");

    char player_label[32];
    SDL_snprintf(player_label, sizeof(player_label), "PLAYER: %d", g_lan_player);
    const Button b_player = { { 150, 220, 160, 44 }, player_label };
    if (button(&b_player)) {
        g_lan_player = g_lan_player == 1 ? 2 : 1;
    }

    const Button b_go = { { 330, 220, 160, 44 }, "START" };
    if (button(&b_go)) {
        start_lan();
    }

    render_msg(300);

    const Button b_back = { { 240, 390, 160, 40 }, "BACK" };
    if (button(&b_back)) {
        enter_screen(SCREEN_MENU);
    }
    render_child_state(440);
    draw_text_centered(WIN_W / 2.0f, 456, 1.0f, COL_DIM,
                       "one machine is player 1, the other player 2");
}

/* ---------------------------------------------------------------------- */

static void handle_event(const SDL_Event* ev) {
    switch (ev->type) {
    case SDL_EVENT_QUIT:
        g_quit = true;
        break;

    case SDL_EVENT_MOUSE_MOTION:
        g_mouse_x = ev->motion.x;
        g_mouse_y = ev->motion.y;
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev->button.button == SDL_BUTTON_LEFT) {
            g_mouse_x = ev->button.x;
            g_mouse_y = ev->button.y;
            g_mouse_clicked = true;
        }
        break;

    case SDL_EVENT_TEXT_INPUT: {
        char* buf = active_entry_buf();
        if (buf != NULL) {
            entry_append_filtered(buf, active_entry_cap(), ev->text.text,
                                  g_screen == SCREEN_JOIN);
        }
        break;
    }

    case SDL_EVENT_KEY_DOWN: {
        char* buf = active_entry_buf();
        if (ev->key.key == SDLK_ESCAPE) {
            if (g_screen == SCREEN_MENU) {
                g_quit = true;
            } else {
                enter_screen(SCREEN_MENU);
            }
        } else if (buf != NULL && ev->key.key == SDLK_BACKSPACE) {
            const size_t len = strlen(buf);
            if (len > 0) {
                buf[len - 1] = '\0';
            }
        } else if (buf != NULL && ev->key.key == SDLK_V &&
                   (ev->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0) {
            char* clip = SDL_GetClipboardText();
            if (clip != NULL) {
                entry_append_filtered(buf, active_entry_cap(), clip,
                                      g_screen == SCREEN_JOIN);
                SDL_free(clip);
            }
        } else if (ev->key.key == SDLK_RETURN || ev->key.key == SDLK_KP_ENTER) {
            if (g_screen == SCREEN_JOIN) {
                start_join();
            } else if (g_screen == SCREEN_LAN) {
                start_lan();
            }
        }
        break;
    }

    default:
        break;
    }
}

int main(int argc, char** argv) {
    SDL_SetAppMetadata("3S Netplay Launcher", NULL, "dev.crowdedstreet.3s-netplay-launcher");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    if (!SDL_CreateWindowAndRenderer("3S Netplay Launcher", WIN_W, WIN_H, 0, &g_win, &g_ren)) {
        fprintf(stderr, "window/renderer creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    if (!find_game_binary(argc, argv)) {
        set_msg(COL_ERR, "Game binary not found next to the launcher.",
                "Run with --game <path-to-game>, or set THIRDSARM_GAME.");
    } else {
        SDL_Log("[launcher] game binary: %s", g_game_path);
    }

    /* Optional auto-start flags — the button actions, triggerable from
     * the command line (scripting / testing; the UI stays up either
     * way): --host, --join <code>, --lan <ip> <player 1|2>. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0) {
            enter_screen(SCREEN_HOST);
            start_host();
        } else if (strcmp(argv[i], "--join") == 0 && i + 1 < argc) {
            enter_screen(SCREEN_JOIN);
            SDL_strlcpy(g_code_entry, argv[i + 1], sizeof(g_code_entry));
            start_join();
            i++;
        } else if (strcmp(argv[i], "--lan") == 0 && i + 2 < argc) {
            enter_screen(SCREEN_LAN);
            SDL_strlcpy(g_ip_entry, argv[i + 1], sizeof(g_ip_entry));
            g_lan_player = SDL_atoi(argv[i + 2]) == 2 ? 2 : 1;
            start_lan();
            i += 2;
        }
    }

    while (!g_quit) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 33)) {
            handle_event(&ev);
            while (SDL_PollEvent(&ev)) {
                handle_event(&ev);
            }
        }

        SDL_SetRenderDrawColor(g_ren, 24, 24, 32, 255);
        SDL_RenderClear(g_ren);

        switch (g_screen) {
        case SCREEN_MENU:
            screen_menu();
            break;
        case SCREEN_HOST:
            screen_host();
            break;
        case SCREEN_JOIN:
            screen_join();
            break;
        case SCREEN_LAN:
            screen_lan();
            break;
        }

        g_mouse_clicked = false;
        SDL_RenderPresent(g_ren);
    }

    /* Deliberately no SDL_KillProcess: the spawned game IS the match;
     * closing the launcher must not end it. DestroyProcess only frees
     * our handle. */
    if (g_child != NULL) {
        SDL_DestroyProcess(g_child);
    }
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
