/*
 * direct_p2p_handoff.c — Step 9 of docs/plan-stun-direct-p2p.md.
 *
 * Minimal key=value reader for the wrapper's handoff file. See the
 * header for the file-format contract.
 *
 * Parsing strategy: fgets a line at a time, skip blanks / comments /
 * leading whitespace, split on the first '=', trim CR/LF/space from
 * both halves, match against the three known keys. Anything else is
 * ignored for forward compatibility (the wrapper may add fields later,
 * and the game shouldn't refuse to launch on an unknown key).
 *
 * The whole file is capped at 4KB. Anything larger is refused with a
 * stderr warning — the real wrapper writes at most ~60 bytes.
 */

#include "netplay/direct_p2p_handoff.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ENABLE_NETPLAY
#include "port/config/config.h"
#endif

#define HANDOFF_MAX_FILE_BYTES 4096
#define HANDOFF_MAX_LINE_BYTES 256

static void rtrim_inplace(char* s) {
    if (s == NULL) return;
    size_t n = strlen(s);
    while (n > 0) {
        const char c = s[n - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            s[n - 1] = '\0';
            n -= 1;
        } else {
            break;
        }
    }
}

static char* ltrim(char* s) {
    if (s == NULL) return s;
    while (*s == ' ' || *s == '\t') {
        s += 1;
    }
    return s;
}

static bool streq(const char* a, const char* b) {
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

bool DirectP2PHandoff_ReadFile(const char* path, DirectP2PHandoff* out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));

    if (path == NULL || path[0] == '\0') {
        return false;
    }

    /* Size check before open so we never slurp a giant file into fgets
     * loops. Missing file is silent — caller decides whether it's an
     * error. */
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if (st.st_size > HANDOFF_MAX_FILE_BYTES) {
        fprintf(stderr, "[direct_p2p_handoff] file %s exceeds %d bytes (%lld); ignoring.\n",
                path, HANDOFF_MAX_FILE_BYTES, (long long)st.st_size);
        return false;
    }

    FILE* f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "[direct_p2p_handoff] could not open %s: %s\n", path, strerror(errno));
        return false;
    }

    DirectP2PHandoffMode mode = DIRECT_P2P_HANDOFF_MODE_NONE;
    int port = 0;
    bool port_seen = false;
    char peer_code[64] = { 0 };
    bool peer_code_seen = false;
    bool parse_error = false;

    char line[HANDOFF_MAX_LINE_BYTES];
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        /* Strip trailing newline/CR/whitespace. */
        rtrim_inplace(line);
        char* p = ltrim(line);

        /* Skip blanks and '#' comments. */
        if (*p == '\0' || *p == '#') continue;

        char* eq = strchr(p, '=');
        if (eq == NULL) {
            /* Lines without '=' aren't fatal — forward-compatible skip. */
            continue;
        }
        *eq = '\0';
        char* key = p;
        char* value = eq + 1;
        rtrim_inplace(key);
        value = ltrim(value);
        /* value already rtrimmed above (rtrim_inplace on line). */

        if (streq(key, "mode")) {
            if (streq(value, "host")) {
                mode = DIRECT_P2P_HANDOFF_MODE_HOST;
            } else if (streq(value, "join")) {
                mode = DIRECT_P2P_HANDOFF_MODE_JOIN;
            } else {
                fprintf(stderr, "[direct_p2p_handoff] unknown mode=\"%s\" in %s\n", value, path);
                parse_error = true;
            }
        } else if (streq(key, "port")) {
            /* atoi is permissive (0 on junk); accept 0..65535. */
            long v = strtol(value, NULL, 10);
            if (v < 0 || v > 65535) {
                fprintf(stderr, "[direct_p2p_handoff] port out of range: %s\n", value);
                parse_error = true;
            } else {
                port = (int)v;
                port_seen = true;
            }
        } else if (streq(key, "peer_code") || streq(key, "code")) {
            /* Accept both `peer_code=` (spec) and `code=` (legacy wrapper
             * output from vendor/Main_MiSTer/thirdsarm_wrapper.cpp before
             * 2026-04-23). Old wrappers in the field keep working. */
            size_t vl = strlen(value);
            if (vl == 0) {
                fprintf(stderr, "[direct_p2p_handoff] peer_code is empty in %s\n", path);
                parse_error = true;
            } else if (vl >= sizeof(peer_code)) {
                fprintf(stderr, "[direct_p2p_handoff] peer_code too long (%zu) in %s\n", vl, path);
                parse_error = true;
            } else {
                memcpy(peer_code, value, vl + 1);
                peer_code_seen = true;
            }
        } else {
            /* Unknown key — ignore. Forward compat. */
        }
    }

    fclose(f);

    if (parse_error) {
        return false;
    }

    if (mode == DIRECT_P2P_HANDOFF_MODE_NONE) {
        fprintf(stderr, "[direct_p2p_handoff] missing mode= in %s\n", path);
        return false;
    }

    if (mode == DIRECT_P2P_HANDOFF_MODE_JOIN && !peer_code_seen) {
        fprintf(stderr, "[direct_p2p_handoff] mode=join requires peer_code= in %s\n", path);
        return false;
    }

    out->mode = mode;
    out->port = port_seen ? port : 0;
    if (peer_code_seen) {
        memcpy(out->peer_code, peer_code, sizeof(out->peer_code));
    }
    return true;
}

void DirectP2PHandoff_Consume(const char* path) {
    if (path == NULL || path[0] == '\0') return;
    if (unlink(path) != 0) {
        /* ENOENT is fine — someone else already cleaned up. */
        if (errno != ENOENT) {
            fprintf(stderr, "[direct_p2p_handoff] unlink %s failed: %s\n", path, strerror(errno));
        }
    }
}

const char* DirectP2PHandoff_ConfigDefaultPath(void) {
#ifdef ENABLE_NETPLAY
    return Config_GetString(CFG_KEY_NETPLAY_DIRECT_P2P_HANDOFF_PATH);
#else
    return NULL;
#endif
}

bool DirectP2PHandoff_FileExists(const char* path) {
    if (path == NULL || path[0] == '\0') return false;
    struct stat st;
    return stat(path, &st) == 0;
}
