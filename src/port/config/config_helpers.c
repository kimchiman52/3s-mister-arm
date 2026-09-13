#include "port/config/config_helpers.h"

#include <SDL3/SDL.h>

void trim(char* string) {
    char* p = string;

    // Trim leading
    while (SDL_isspace((unsigned char)*p)) {
        p++;
    }

    if (p != string) {
        SDL_memmove(string, p, SDL_strlen(p) + 1);
    }

    // Trim trailing
    char* end = string + SDL_strlen(string);

    while ((end > string) && SDL_isspace((unsigned char)end[-1])) {
        end--;
        *end = '\0';
    }
}

void io_printf(SDL_IOStream* io, const char* format, ...) {
    va_list args;
    va_start(args, format);

    char* rendered = NULL;
    SDL_vasprintf(&rendered, format, args);
    va_end(args);

    if (rendered == NULL) {
        return;
    }

    SDL_WriteIO(io, rendered, SDL_strlen(rendered));
    SDL_free(rendered);
}

void dict_read(FILE* file, DictIterator iterator) {
    if (file == NULL) {
        return;
    }

    char line[256];

    while (fgets(line, sizeof(line), file)) {
        // Remove newline
        line[strcspn(line, "\r\n")] = '\0';

        char* p = line;

        // Skip leading whitespace
        while (SDL_isspace((unsigned char)*p)) {
            p++;
        }

        // Skip empty/comment lines. A `#` is a comment ONLY here, as the first
        // non-blank character of a line; an inline `#` is deliberately NOT
        // stripped, so `key = value # note` keeps ` # note` in the value (the
        // %[^\n] scanset below runs to end-of-line and trim() touches only
        // whitespace).
        //
        // That matches the wrapper's own parser byte for byte
        // (vendor/Main_MiSTer/replay_proxy.c -> RpConfigLoadFrom), which reads
        // THE SAME FILE from a separate binary. Their agreeing is the property
        // worth protecting: adding inline stripping to one side alone creates a
        // divergence in what a config line means. Adding it to both would
        // truncate legitimate values -- `replays-root` and
        // `netplay-direct-p2p-handoff-path` are filesystem paths where `#` is
        // legal, and this function also parses keymap.c's file.
        //
        // Both parsers move together or neither does. See the longer note at
        // RpConfigLoadFrom for the one user-visible symptom (`replay-proxy-host
        // = off # note` reads as a hostname, not the disable sentinel).
        if (*p == '\0' || *p == '#') {
            continue;
        }

        char key[128];
        char value[128];

        if (sscanf(p, "%127[^=]=%127[^\n]", key, value) != 2) {
            continue;
        }

        trim(key);
        trim(value);

        if (SDL_strlen(key) == 0 || SDL_strlen(value) == 0) {
            continue;
        }

        const bool should_continue = iterator(key, value);

        if (!should_continue) {
            break;
        }
    }
}
