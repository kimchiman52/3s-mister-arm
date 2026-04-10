#!/usr/bin/env bash
set -euo pipefail

if [[ -n "${DOCKER_BIN:-}" ]]; then
    docker_bin="$DOCKER_BIN"
else
    docker_bin="$(command -v docker || true)"
    if [[ -z "$docker_bin" && -x /usr/local/bin/docker ]]; then
        docker_bin="/usr/local/bin/docker"
    fi
fi

if [[ -z "$docker_bin" ]]; then
    echo "docker CLI not found; set DOCKER_BIN explicitly" >&2
    exit 127
fi

CONTAINER="${CONTAINER:-3s-mister-arm-build}"

cat <<'SRC' | "$docker_bin" exec -i "$CONTAINER" sh -lc '
set -e
tmp_c="$(mktemp /tmp/sdl_rect_bench.XXXXXX.c)"
tmp_bin="$(mktemp /tmp/sdl_rect_bench.XXXXXX)"
trap '\''rm -f "$tmp_c" "$tmp_bin"'\'' EXIT
cat > "$tmp_c"
clang -O2 -I/src/third_party/sdl3/build/include -L/src/third_party/sdl3/build/lib -Wl,-rpath,/src/third_party/sdl3/build/lib "$tmp_c" -lSDL3 -lm -o "$tmp_bin"
LD_LIBRARY_PATH=/src/third_party/sdl3/build/lib SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software "$tmp_bin"
'
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define FRAME_COUNT 180
#define TASK_COUNT 282
#define RUN_LEN 3

typedef struct Task {
    SDL_FRect dst;
    SDL_FRect src;
    SDL_FColor color;
} Task;

static void fill_tasks(Task *tasks, int count) {
    for (int i = 0; i < count; i++) {
        const int col = i % 24;
        const int row = (i / 24) % 14;
        tasks[i].dst.x = (float)((col * 13) % 352);
        tasks[i].dst.y = (float)((row * 11) % 192);
        tasks[i].dst.w = 16.0f + (float)(i % 3);
        tasks[i].dst.h = 16.0f + (float)((i / 3) % 3);
        tasks[i].src.x = (float)((i * 7) % 48);
        tasks[i].src.y = (float)((i * 5) % 48);
        tasks[i].src.w = 16.0f;
        tasks[i].src.h = 16.0f;
        tasks[i].color.r = (float)((37 * i) % 256) / 255.0f;
        tasks[i].color.g = (float)((73 * i) % 256) / 255.0f;
        tasks[i].color.b = (float)((19 * i) % 256) / 255.0f;
        tasks[i].color.a = 0.85f;
    }
}

static void populate_rect_vertices(const Task *task, SDL_Vertex out_vertices[4]) {
    const float x0 = task->dst.x;
    const float y0 = task->dst.y;
    const float x1 = x0 + task->dst.w;
    const float y1 = y0 + task->dst.h;
    const float s0 = task->src.x / 64.0f;
    const float t0 = task->src.y / 64.0f;
    const float s1 = (task->src.x + task->src.w) / 64.0f;
    const float t1 = (task->src.y + task->src.h) / 64.0f;
    out_vertices[0] = (SDL_Vertex){ .position = { x0, y0 }, .color = task->color, .tex_coord = { s0, t0 } };
    out_vertices[1] = (SDL_Vertex){ .position = { x1, y0 }, .color = task->color, .tex_coord = { s1, t0 } };
    out_vertices[2] = (SDL_Vertex){ .position = { x0, y1 }, .color = task->color, .tex_coord = { s0, t1 } };
    out_vertices[3] = (SDL_Vertex){ .position = { x1, y1 }, .color = task->color, .tex_coord = { s1, t1 } };
}

static SDL_Texture *create_source_texture(SDL_Renderer *renderer) {
    SDL_Surface *surface = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA8888);
    if (!surface) {
        return NULL;
    }
    for (int y = 0; y < 64; y++) {
        Uint8 *row_bytes = ((Uint8 *)surface->pixels) + ((size_t)y * (size_t)surface->pitch);
        Uint32 *row = (Uint32 *)row_bytes;
        for (int x = 0; x < 64; x++) {
            const Uint8 c = ((x / 8) ^ (y / 8)) ? 255 : 120;
            row[x] =
                SDL_MapSurfaceRGBA(surface, c, (Uint8)(255 - c), (Uint8)((x * 4) & 255), (Uint8)(128 + ((y * 2) & 127)));
        }
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    return texture;
}

static double benchmark_copy(SDL_Renderer *renderer, SDL_Texture *target, SDL_Texture *texture, const Task *tasks) {
    Uint64 total_ns = 0;
    for (int frame = 0; frame < FRAME_COUNT; frame++) {
        if (!SDL_SetRenderTarget(renderer, target) || !SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) ||
            !SDL_RenderClear(renderer)) {
            fprintf(stderr, "copy setup failed: %s\n", SDL_GetError());
            return -1.0;
        }
        Uint64 start = SDL_GetTicksNS();
        for (int i = 0; i < TASK_COUNT; i++) {
            if (!SDL_SetTextureColorModFloat(texture, tasks[i].color.r, tasks[i].color.g, tasks[i].color.b) ||
                !SDL_SetTextureAlphaModFloat(texture, tasks[i].color.a) ||
                !SDL_RenderTexture(renderer, texture, &tasks[i].src, &tasks[i].dst)) {
                fprintf(stderr, "copy submit failed: %s\n", SDL_GetError());
                return -1.0;
            }
        }
        SDL_Surface *surface = SDL_RenderReadPixels(renderer, NULL);
        Uint64 end = SDL_GetTicksNS();
        if (!surface) {
            fprintf(stderr, "copy readback failed: %s\n", SDL_GetError());
            return -1.0;
        }
        SDL_DestroySurface(surface);
        total_ns += end - start;
    }
    return (double)total_ns / (double)FRAME_COUNT / 1e6;
}

static double benchmark_geometry_runs(SDL_Renderer *renderer, SDL_Texture *target, SDL_Texture *texture, const Task *tasks) {
    SDL_Vertex vertices[RUN_LEN * 4];
    int indices[RUN_LEN * 6];
    for (int task = 0; task < RUN_LEN; task++) {
        const int vb = task * 4;
        const int ib = task * 6;
        indices[ib + 0] = vb + 0;
        indices[ib + 1] = vb + 1;
        indices[ib + 2] = vb + 2;
        indices[ib + 3] = vb + 1;
        indices[ib + 4] = vb + 2;
        indices[ib + 5] = vb + 3;
    }

    Uint64 total_ns = 0;
    for (int frame = 0; frame < FRAME_COUNT; frame++) {
        if (!SDL_SetRenderTarget(renderer, target) || !SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) ||
            !SDL_RenderClear(renderer)) {
            fprintf(stderr, "geometry setup failed: %s\n", SDL_GetError());
            return -1.0;
        }
        Uint64 start = SDL_GetTicksNS();
        for (int i = 0; i < TASK_COUNT; i += RUN_LEN) {
            int run = RUN_LEN;
            if ((i + run) > TASK_COUNT) {
                run = TASK_COUNT - i;
            }
            for (int j = 0; j < run; j++) {
                populate_rect_vertices(&tasks[i + j], &vertices[j * 4]);
            }
            if (!SDL_RenderGeometry(renderer, texture, vertices, run * 4, indices, run * 6)) {
                fprintf(stderr, "geometry submit failed: %s\n", SDL_GetError());
                return -1.0;
            }
        }
        SDL_Surface *surface = SDL_RenderReadPixels(renderer, NULL);
        Uint64 end = SDL_GetTicksNS();
        if (!surface) {
            fprintf(stderr, "geometry readback failed: %s\n", SDL_GetError());
            return -1.0;
        }
        SDL_DestroySurface(surface);
        total_ns += end - start;
    }
    return (double)total_ns / (double)FRAME_COUNT / 1e6;
}

int main(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("bench", 64, 64, SDL_WINDOW_HIDDEN);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "software");
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Texture *target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 384, 224);
    SDL_Texture *texture = create_source_texture(renderer);
    Task *tasks = calloc(TASK_COUNT, sizeof(Task));
    if (!target || !texture || !tasks) {
        fprintf(stderr, "setup failed: %s\n", SDL_GetError());
        return 1;
    }
    fill_tasks(tasks, TASK_COUNT);
    const double copy_ms = benchmark_copy(renderer, target, texture, tasks);
    const double geom_ms = benchmark_geometry_runs(renderer, target, texture, tasks);
    if ((copy_ms < 0.0) || (geom_ms < 0.0)) {
        free(tasks);
        SDL_DestroyTexture(texture);
        SDL_DestroyTexture(target);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    printf("copy_ms=%.3f\ngeometry_run_ms=%.3f\ndelta_ms=%.3f\n", copy_ms, geom_ms, copy_ms - geom_ms);
    free(tasks);
    SDL_DestroyTexture(texture);
    SDL_DestroyTexture(target);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
SRC
