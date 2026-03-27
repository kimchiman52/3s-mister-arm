#ifndef MISTER_JOY_SHM_H
#define MISTER_JOY_SHM_H

#include <stdint.h>

#define MISTER_JOY_SHM_PATH "/dev/shm/threesx-joy"
#define MISTER_JOY_SHM_MAGIC 0x334A5358   /* "3JSX" */
#define MISTER_JOY_SHM_VERSION 1
#define MISTER_JOY_MAX_PLAYERS 2

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t joy_mask[MISTER_JOY_MAX_PLAYERS];
    int8_t   left_stick_x[MISTER_JOY_MAX_PLAYERS];
    int8_t   left_stick_y[MISTER_JOY_MAX_PLAYERS];
    int8_t   right_stick_x[MISTER_JOY_MAX_PLAYERS];
    int8_t   right_stick_y[MISTER_JOY_MAX_PLAYERS];
} MisterJoyShm;

#endif
