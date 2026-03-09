#include "port/resources.h"
#include "port/paths.h"
#include "port/sdl/sdl_app.h"

#include <SDL3/SDL.h>

#if defined(ENABLE_ISO_IMPORT)
#include <cdio/iso9660.h>
#endif

#if defined(ENABLE_ISO_IMPORT)
typedef enum FlowState {
    INIT,
    DIALOG_OPENED,
    COPY_ERROR,
    COPY_SUCCESS,
} ResourceCopyingFlowState;

static ResourceCopyingFlowState flow_state = INIT;
#endif

static bool file_exists(const char* path) {
    SDL_PathInfo path_info;
    SDL_GetPathInfo(path, &path_info);
    return path_info.type == SDL_PATHTYPE_FILE;
}

static bool check_if_file_present(const char* filename) {
    char* file_path = Resources_GetPath(filename);
    const bool result = file_exists(file_path);
    SDL_free(file_path);
    return result;
}

#if defined(ENABLE_ISO_IMPORT) && defined(ENABLE_SDL_DIALOGS)
static void create_resources_directory() {
    char* path = Resources_GetPath(NULL);
    SDL_CreateDirectory(path);
    SDL_free(path);
}

#define CHUNK_SECTORS 16
#define BUFFER_SIZE (ISO_BLOCKSIZE * CHUNK_SECTORS)

static void open_file_dialog_callback(void* userdata, const char* const* filelist, int filter) {
    (void)userdata;
    (void)filter;

    if (filelist == NULL || filelist[0] == NULL) {
        flow_state = COPY_ERROR;
        return;
    }

    const char* iso_path = filelist[0];

    iso9660_t* iso = iso9660_open(iso_path);

    if (iso == NULL) {
        flow_state = COPY_ERROR;
        return;
    }

    iso9660_stat_t* stat = iso9660_ifs_stat(iso, "/THIRD/SF33RD.AFS;1");

    if (stat == NULL) {
        stat = iso9660_ifs_stat(iso, "/SF33RD.AFS;1");

        if (stat == NULL) {
            iso9660_close(iso);
            flow_state = COPY_ERROR;
            return;
        }
    }

    create_resources_directory();
    char* dst_path = Resources_GetPath("SF33RD.AFS");
    SDL_IOStream* dst_io = SDL_IOFromFile(dst_path, "w");
    SDL_free(dst_path);

    uint8_t buffer[BUFFER_SIZE];
    uint64_t bytes_remaining = stat->total_size;
    lsn_t current_lsn = stat->lsn;

    while (bytes_remaining > 0) {
        const uint64_t bytes_to_read = SDL_min(sizeof(buffer), bytes_remaining);
        const uint64_t sectors_to_read = (bytes_to_read + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE;

        const long bytes_read = iso9660_iso_seek_read(iso, buffer, current_lsn, sectors_to_read);
        SDL_WriteIO(dst_io, buffer, bytes_read);

        bytes_remaining -= bytes_read;
        current_lsn += sectors_to_read;
    }

    iso9660_stat_free(stat);
    iso9660_close(iso);
    SDL_CloseIO(dst_io);
    flow_state = COPY_SUCCESS;
}
#endif

#if defined(ENABLE_ISO_IMPORT)

static void show_info_message(const char* title, const char* message) {
#if defined(ENABLE_SDL_DIALOGS)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, message, window);
#else
    SDL_Log("%s: %s", title, message);
#endif
}

static void show_error_message(const char* title, const char* message) {
#if defined(ENABLE_SDL_DIALOGS)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, window);
#else
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", title, message);
#endif
}

static void open_dialog() {
    flow_state = DIALOG_OPENED;

#if defined(ENABLE_SDL_DIALOGS)
    const SDL_DialogFileFilter filter = { .name = "Game iso", .pattern = "iso" };
    SDL_ShowOpenFileDialog(open_file_dialog_callback, NULL, window, &filter, 1, NULL, false);
#else
    flow_state = COPY_ERROR;
#endif
}
#endif

char* Resources_GetPath(const char* file_path) {
    const char* base = Paths_GetPrefPath();
    char* full_path = NULL;

    if (file_path == NULL) {
        SDL_asprintf(&full_path, "%sresources/", base);
    } else {
        SDL_asprintf(&full_path, "%sresources/%s", base, file_path);
    }

    return full_path;
}

bool Resources_CheckIfPresent() {
    return check_if_file_present("SF33RD.AFS");
}

bool Resources_RunResourceCopyingFlow() {
#if defined(ENABLE_ISO_IMPORT)
    switch (flow_state) {
    case INIT:
        show_info_message("Resources are missing",
                          "3SX needs resources from a copy of \"Street Fighter III: 3rd Strike\" to run. Choose "
                          "the iso in the next dialog");
        open_dialog();
        break;

    case DIALOG_OPENED:
        break;

    case COPY_ERROR:
        show_error_message("Invalid iso", "The iso you provided doesn't contain the required files");
        open_dialog();
        break;

    case COPY_SUCCESS: {
        char* resources_path = Resources_GetPath(NULL);
        char* message = NULL;
        SDL_asprintf(&message, "You can find them at:\n%s", resources_path);
        show_info_message("Resources copied successfully", message);
        SDL_free(resources_path);
        SDL_free(message);
        flow_state = INIT;
        return true;
    }
    }
#endif

    return false;
}
