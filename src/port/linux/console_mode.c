#include "port/linux/console_mode.h"

#if defined(PORT_MISTER) && defined(__linux__)

#include <linux/kd.h>

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static const char* kRuntimeConsolePathEnv = "THIRDSARM_RUNTIME_TTY";
static int console_fd = -1;
static char console_path[128] = { 0 };
static int saved_kd_mode = -1;
static int saved_kb_mode = -1;
static bool saved_kb_mode_valid = false;
static struct termios saved_termios;
static bool saved_termios_valid = false;
static bool console_mode_entered = false;
static bool restore_registered = false;

static void close_console_fd(void) {
    if (console_fd >= 0) {
        close(console_fd);
        console_fd = -1;
    }

    console_path[0] = '\0';
}

static int open_path(const char* path) {
    if (path == NULL || path[0] == '\0') {
        return -1;
    }

#if defined(O_CLOEXEC)
    const int fd = open(path, O_RDWR | O_CLOEXEC);
#else
    const int fd = open(path, O_RDWR);
#endif

    return fd;
}

static int accept_console_fd(int fd, const char* path) {
    int kd_mode = 0;

    if (fd < 0) {
        return -1;
    }

    if (ioctl(fd, KDGETMODE, &kd_mode) < 0) {
        close(fd);
        return -1;
    }

    if (path != NULL && path[0] != '\0') {
        snprintf(console_path, sizeof(console_path), "%s", path);
    } else {
        console_path[0] = '\0';
    }

    return fd;
}

static int open_console_path(const char* path) {
    return accept_console_fd(open_path(path), path);
}

static bool requested_console_path(char* out_path, size_t out_path_len) {
    const char* env_path = getenv(kRuntimeConsolePathEnv);

    if (out_path == NULL || out_path_len == 0 || env_path == NULL || env_path[0] == '\0') {
        return false;
    }

    if (env_path[0] == '/') {
        return snprintf(out_path, out_path_len, "%s", env_path) > 0;
    }

    if (strncmp(env_path, "tty", 3) == 0) {
        return snprintf(out_path, out_path_len, "/dev/%s", env_path) > 0;
    }

    return false;
}

static bool active_console_path(char* out_path, size_t out_path_len) {
    if (out_path == NULL || out_path_len == 0) {
        return false;
    }

    int fd = -1;
    char active_buf[64] = { 0 };
    ssize_t count = 0;

#if defined(O_CLOEXEC)
    fd = open("/sys/class/tty/tty0/active", O_RDONLY | O_CLOEXEC);
#else
    fd = open("/sys/class/tty/tty0/active", O_RDONLY);
#endif

    if (fd < 0) {
        return false;
    }

    count = read(fd, active_buf, sizeof(active_buf) - 1);
    close(fd);

    if (count <= 0) {
        return false;
    }

    active_buf[count] = '\0';

    for (int i = 0; active_buf[i] != '\0'; i++) {
        if (active_buf[i] == '\n' || active_buf[i] == '\r' || active_buf[i] == '\t' || active_buf[i] == ' ') {
            active_buf[i] = '\0';
            break;
        }
    }

    if (strncmp(active_buf, "tty", 3) != 0) {
        return false;
    }

    return snprintf(out_path, out_path_len, "/dev/%s", active_buf) > 0;
}

static int open_stdio_tty(int stdio_fd) {
    char tty_path[128] = { 0 };

    if (ttyname_r(stdio_fd, tty_path, sizeof(tty_path)) != 0) {
        return -1;
    }

    return open_console_path(tty_path);
}

static int open_console_fd(void) {
    char requested_path[128] = { 0 };
    char active_path[128] = { 0 };
    int fd = -1;

    console_path[0] = '\0';

    if (requested_console_path(requested_path, sizeof(requested_path))) {
        fd = open_console_path(requested_path);
        if (fd >= 0) {
            return fd;
        }

        fprintf(stderr, "ConsoleMode: requested console device %s was not a usable VT\n", requested_path);
    }

    /* On MiSTer wrapper launches, /dev/tty can resolve to a handle that opens
       successfully but rejects KD/KB ioctls. Prefer the wrapper-selected or
       active VT device (for example /dev/tty1) before falling back to aliases. */
    if (active_console_path(active_path, sizeof(active_path))) {
        fd = open_console_path(active_path);
        if (fd >= 0) {
            return fd;
        }
    }

    fd = open_stdio_tty(STDIN_FILENO);
    if (fd >= 0) {
        return fd;
    }

    fd = open_stdio_tty(STDOUT_FILENO);
    if (fd >= 0) {
        return fd;
    }

    fd = open_stdio_tty(STDERR_FILENO);
    if (fd >= 0) {
        return fd;
    }

    fd = open_console_path("/dev/tty");
    if (fd >= 0) {
        return fd;
    }

    const char* candidates[] = { "/dev/tty0", "/dev/console" };

    for (int i = 0; i < 2; i++) {
        fd = open_console_path(candidates[i]);

        if (fd >= 0) {
            return fd;
        }
    }

    return -1;
}

void ConsoleMode_Exit(void) {
    if (!console_mode_entered) {
        return;
    }

    if (saved_kb_mode_valid) {
        ioctl(console_fd, KDSKBMODE, saved_kb_mode);
    } else {
        ioctl(console_fd, KDSKBMODE, K_XLATE);
    }

    if (saved_kd_mode >= 0) {
        ioctl(console_fd, KDSETMODE, saved_kd_mode);
    } else {
        ioctl(console_fd, KDSETMODE, KD_TEXT);
    }

    if (saved_termios_valid) {
        tcsetattr(console_fd, TCSANOW, &saved_termios);
    }

    close_console_fd();
    saved_kd_mode = -1;
    saved_kb_mode = -1;
    saved_kb_mode_valid = false;
    saved_termios_valid = false;
    console_mode_entered = false;
}

bool ConsoleMode_Enter(void) {
    if (console_mode_entered) {
        return true;
    }

    console_fd = open_console_fd();

    if (console_fd < 0) {
        fprintf(stderr, "ConsoleMode: unable to open Linux console device\n");
        return false;
    }

    fprintf(stderr, "ConsoleMode: using console device %s\n", console_path[0] != '\0' ? console_path : "(unknown)");

    if (tcgetattr(console_fd, &saved_termios) == 0) {
        struct termios raw = saved_termios;
        raw.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
        raw.c_iflag &= ~(IXON | IXOFF | ICRNL | INLCR);
        raw.c_oflag &= ~(OPOST);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(console_fd, TCSANOW, &raw);
        saved_termios_valid = true;
    }

    if (ioctl(console_fd, KDGETMODE, &saved_kd_mode) < 0) {
        saved_kd_mode = KD_TEXT;
    }

    if (ioctl(console_fd, KDGKBMODE, &saved_kb_mode) == 0) {
        saved_kb_mode_valid = true;
    }

    if (ioctl(console_fd, KDSKBMODE, K_OFF) < 0) {
        fprintf(stderr, "ConsoleMode: failed to set KDSKBMODE=K_OFF\n");
        if (saved_termios_valid) {
            tcsetattr(console_fd, TCSANOW, &saved_termios);
            saved_termios_valid = false;
        }
        saved_kd_mode = -1;
        saved_kb_mode = -1;
        saved_kb_mode_valid = false;
        close_console_fd();
        return false;
    }

    if (ioctl(console_fd, KDSETMODE, KD_GRAPHICS) < 0) {
        fprintf(stderr, "ConsoleMode: failed to switch console to KD_GRAPHICS\n");
        if (saved_kb_mode_valid) {
            ioctl(console_fd, KDSKBMODE, saved_kb_mode);
        } else {
            ioctl(console_fd, KDSKBMODE, K_XLATE);
        }
        saved_kb_mode = -1;
        saved_kb_mode_valid = false;
        if (saved_termios_valid) {
            tcsetattr(console_fd, TCSANOW, &saved_termios);
            saved_termios_valid = false;
        }
        saved_kd_mode = -1;
        close_console_fd();
        return false;
    }

    if (!restore_registered) {
        atexit(ConsoleMode_Exit);
        restore_registered = true;
    }

    console_mode_entered = true;
    fprintf(stderr, "ConsoleMode: switched to KD_GRAPHICS\n");
    return true;
}

#else

bool ConsoleMode_Enter(void) {
    return false;
}

void ConsoleMode_Exit(void) {
}

#endif
