#include "thirdsarm_wrapper.h"

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "mister_joy_shm.h"

#include "cfg.h"
#include "file_io.h"
#include "frame_timer.h"
#include "fpga_io.h"
#include "input.h"
#include "audio.h"
#include "osd.h"
#include "menu.h"
#include "support/arcade/mra_loader.h"
#include "thirdsarm_core_context.h"
#include "user_io.h"
#include "video.h"

extern char **environ;

volatile sig_atomic_t g_child_pid = -1;

static MisterJoyShm *g_joy_shm = nullptr;

namespace {

constexpr const char *kCoreName = "3S-ARM";
constexpr const char *kRuntimeHome = "/media/fat/games/3s-arm";
constexpr const char *kRuntimeBinary = "/media/fat/games/3s-arm/bin/3s-arm";
constexpr const char *kRuntimeArchive = "/media/fat/games/3s-arm/resources/SF33RD.AFS";
constexpr const char *kRuntimeLibDir = "/media/fat/games/3s-arm/lib";
constexpr const char *kLogDir = "/media/fat/games/3s-arm/logs";
constexpr const char *kWrapperLogPath = "/media/fat/games/3s-arm/logs/osd-wrapper.log";
constexpr const char *kLastRunLogPath = "/media/fat/games/3s-arm/logs/last-run.log";
constexpr const char *kRuntimeScaleModeEnv = "THIRDSARM_SCALE_MODE_STARTUP_OVERRIDE";
constexpr const char *kRuntimeScaleModeExplicitMarker = "# thirdsarm-wrapper-scale-mode-explicit";
constexpr const char *kRuntimeScaleModeAutoMarker = "# thirdsarm-wrapper-scale-mode-auto";
constexpr const char *kMenuCore = "menu.rbf";
constexpr const char *kMenuExec = "MiSTer";
constexpr const char *kRuntimeTtySwitchEnv = "THIRDSARM_WRAPPER_USE_TTY2";
constexpr int kRuntimeFpsToggleSignal = SIGUSR1;
constexpr int kRuntimeSuperEffectQualityCycleSignal = SIGUSR2;
#define kRuntimeGhostResolutionCycleSignal (SIGRTMIN)
#define kRuntimeGhostCountCycleSignal (SIGRTMIN + 1)
#define kRuntimeArmClockCycleSignal (SIGRTMIN + 2)
#define kRuntimeGameModeCycleSignal (SIGRTMIN + 3)
#define kRuntimeHoldToPauseCycleSignal (SIGRTMIN + 4)

enum RuntimeScaleModeMenu
{
	kScaleModeAuto = 0,
	kScaleModeNative,
	kScaleModeNearest,
	kScaleModeMenuCount
};

enum RuntimeSuperEffectQualityMenu
{
	kSuperEffectQualityFull = 0,
	kSuperEffectQualityCachedBg,
	kSuperEffectQualityMenuCount
};

enum RuntimeGhostResolutionMenu
{
	kGhostResolutionFull = 0,
	kGhostResolutionHalf,
	kGhostResolutionMenuCount
};

enum RuntimeGhostCountMenu
{
	kGhostCount0 = 0,
	kGhostCount1,
	kGhostCount2,
	kGhostCount3,
	kGhostCount4,
	kGhostCountMenuCount
};

enum RuntimeArmClockMenu
{
	kArmClockStock = 0,
	kArmClock1000,
	kArmClock1200,
	kArmClockMenuCount
};

enum RuntimeGameModeMenu
{
	kGameModeConsole = 0,
	kGameModeArcade,
	kGameModeMenuCount
};

enum RuntimeHoldToPauseMenu
{
	kHoldToPauseOff = 0,
	kHoldToPauseOn,
	kHoldToPauseMenuCount
};

enum FpsOverlayMode
{
	kFpsOverlayOff = 0,
	kFpsOverlayFps = 1,
	kFpsOverlayDebug = 2,
	kFpsOverlayModeCount = 3,
};

volatile sig_atomic_t g_wrapper_signal = 0;
int g_wrapper_fps_mode = kFpsOverlayOff;
int g_wrapper_super_effect_quality = kSuperEffectQualityCachedBg;
int g_wrapper_ghost_resolution = kGhostResolutionFull;
int g_wrapper_ghost_count = kGhostCount4;
int g_wrapper_arm_clock = kArmClockStock;
int g_wrapper_arm_clock_active = kArmClockStock;
int g_wrapper_game_mode = kGameModeConsole;
int g_wrapper_hold_to_pause = kHoldToPauseOff;
int g_wrapper_restart_requested = 0;
int g_wrapper_used_full_user_io_init = 0;
static bool g_native_video_mode = false;

struct StartupScaleModeSelection
{
	int set_env = 0;
	int direct_video = 0;
	int vga_scaler = 0;
	int forced_scandoubler = 0;
	int io_type = 0;
	int vga_mode_int = 0;
	char source[32] = {};
	char value[32] = {};
	char vga_mode[16] = {};
};

struct RuntimeConfigDefaultEntry
{
	const char *key;
	const char *value;
};

static const RuntimeConfigDefaultEntry kRuntimeGeneratedDefaults[] = {
	{ "fullscreen", "true" },
	{ "window-width", "320" },
	{ "window-height", "240" },
	{ "scale-mode", "native" },
	{ "software-frame-mode", "on" },
	{ "super-effect-quality", "cached-bg" },
	{ "show-fps", "false" },
	{ "video-driver-order", "dummy" },
	{ "render-driver-order", "software" },
	{ "game-mode", "console" },
};

void write_log_line(FILE *file, const char *fmt, ...);

bool force_requested()
{
	const char *force = getenv("THIRDSARM_WRAPPER_FORCE");
	return force && strcmp(force, "0");
}

bool runtime_tty2_requested()
{
	const char *value = getenv(kRuntimeTtySwitchEnv);
	return value && strcmp(value, "0") && strcasecmp(value, "false");
}

bool matches_core_name(const char *name)
{
	return name && name[0] && !strcasecmp(name, kCoreName);
}

int get_active_vt()
{
	FILE *file = fopen("/sys/class/tty/tty0/active", "r");
	if (!file) return 1;

	char buffer[64] = {};
	if (!fgets(buffer, sizeof(buffer), file))
	{
		fclose(file);
		return 1;
	}

	fclose(file);

	int vt = 1;
	if (sscanf(buffer, "tty%d", &vt) == 1 && vt > 0) return vt;
	return 1;
}

void restore_console(int active_vt)
{
	if (active_vt < 1) active_vt = 1;
	video_chvt(active_vt);

	char console_path[32] = {};
	snprintf(console_path, sizeof(console_path), "/dev/tty%d", active_vt);
	int console_fd = open(console_path, O_WRONLY | O_CLOEXEC);
	if (console_fd < 0) return;

	static const char reset_sequence[] = "\033c";
	write(console_fd, reset_sequence, sizeof(reset_sequence) - 1);
	close(console_fd);
}

void restore_runtime_console_mode(FILE *wrapper_log, int runtime_vt)
{
	if (runtime_vt < 1) runtime_vt = 1;

	char runtime_tty[32] = {};
	snprintf(runtime_tty, sizeof(runtime_tty), "/dev/tty%d", runtime_vt);

	int fd = open(runtime_tty, O_RDWR | O_CLOEXEC);
	if (fd < 0)
	{
		write_log_line(wrapper_log, "runtime_console_restore=open_failed tty=%s errno=%d", runtime_tty, errno);
		return;
	}

	(void)ioctl(fd, KDSKBMODE, K_XLATE);
	if (ioctl(fd, KDSETMODE, KD_TEXT) < 0)
	{
		write_log_line(wrapper_log, "runtime_console_restore=kd_text_failed tty=%s errno=%d", runtime_tty, errno);
	}

	close(fd);
}

void write_log_line(FILE *file, const char *fmt, ...)
{
	if (!file) return;

	va_list ap;
	va_start(ap, fmt);
	vfprintf(file, fmt, ap);
	va_end(ap);
	fputc('\n', file);
	fflush(file);
}

void write_fd_line(int fd, const char *fmt, ...)
{
	if (fd < 0) return;

	char buffer[1024];
	va_list ap;
	va_start(ap, fmt);
	int written = vsnprintf(buffer, sizeof(buffer), fmt, ap);
	va_end(ap);
	if (written <= 0) return;

	size_t len = (written >= (int)sizeof(buffer)) ? sizeof(buffer) - 1 : (size_t)written;
	buffer[len++] = '\n';
	(void)write(fd, buffer, len);
}

void pin_current_thread_to_cpu(FILE *wrapper_log, const char *label, int cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);

	if (sched_setaffinity(0, sizeof(set), &set) == 0)
	{
		write_log_line(wrapper_log, "%s_affinity=cpu%d", label, cpu);
		return;
	}

	write_log_line(wrapper_log, "%s_affinity=failed cpu=%d errno=%d", label, cpu, errno);
}

void set_error_message(char *error, size_t error_size, const char *message)
{
	if (!error || !error_size) return;
	snprintf(error, error_size, "%s", message);
}

void get_cwd_string(char *buffer, size_t size)
{
	if (!buffer || size == 0) return;
	if (!getcwd(buffer, size)) snprintf(buffer, size, "(cwd-unavailable:%d)", errno);
}

void redirect_stdio_to_log(FILE *wrapper_log, int *saved_stdout, int *saved_stderr)
{
	if (!wrapper_log) return;

	int log_fd = fileno(wrapper_log);
	if (log_fd < 0) return;

	*saved_stdout = dup(STDOUT_FILENO);
	*saved_stderr = dup(STDERR_FILENO);

	if (*saved_stdout >= 0) dup2(log_fd, STDOUT_FILENO);
	if (*saved_stderr >= 0) dup2(log_fd, STDERR_FILENO);
}

void restore_stdio(int saved_stdout, int saved_stderr)
{
	if (saved_stdout >= 0)
	{
		dup2(saved_stdout, STDOUT_FILENO);
		close(saved_stdout);
	}

	if (saved_stderr >= 0)
	{
		dup2(saved_stderr, STDERR_FILENO);
		close(saved_stderr);
	}
}

void wrapper_signal_handler(int signum)
{
	g_wrapper_signal = signum;
	if (g_child_pid > 0) kill((pid_t)g_child_pid, signum);
}

void install_signal_handlers(struct sigaction *old_int, struct sigaction *old_hup, struct sigaction *old_term)
{
	struct sigaction act = {};
	act.sa_handler = wrapper_signal_handler;
	sigemptyset(&act.sa_mask);

	sigaction(SIGINT, &act, old_int);
	sigaction(SIGHUP, &act, old_hup);
	sigaction(SIGTERM, &act, old_term);
}

void restore_signal_handlers(const struct sigaction *old_int, const struct sigaction *old_hup, const struct sigaction *old_term)
{
	sigaction(SIGINT, old_int, nullptr);
	sigaction(SIGHUP, old_hup, nullptr);
	sigaction(SIGTERM, old_term, nullptr);
}

void trim_in_place(char *value)
{
	if (!value) return;

	char *start = value;
	while (*start && isspace((unsigned char)*start)) start++;
	if (start != value) memmove(value, start, strlen(start) + 1);

	size_t len = strlen(value);
	while (len > 0 && isspace((unsigned char)value[len - 1]))
	{
		value[--len] = 0;
	}
}

bool read_runtime_config_value(const char *wanted_key, char *value, size_t value_size)
{
	if (value && value_size) value[0] = 0;

	char path[PATH_MAX] = {};
	snprintf(path, sizeof(path), "%s/config", kRuntimeHome);

	FILE *file = fopen(path, "r");
	if (!file) return false;

	char line[256] = {};
	while (fgets(line, sizeof(line), file))
	{
		char *cursor = line;
		while (*cursor && isspace((unsigned char)*cursor)) cursor++;
		if (!*cursor || *cursor == '#') continue;

		char *equals = strchr(cursor, '=');
		if (!equals) continue;

		*equals = 0;
		char *key = cursor;
		char *raw_value = equals + 1;
		trim_in_place(key);
		trim_in_place(raw_value);
		if (!key[0] || !raw_value[0] || strcasecmp(key, wanted_key)) continue;

		if (value && value_size) snprintf(value, value_size, "%s", raw_value);
		fclose(file);
		return true;
	}

	fclose(file);
	return false;
}

static bool runtime_scale_mode_has_explicit_marker()
{
	char path[PATH_MAX] = {};
	snprintf(path, sizeof(path), "%s/config", kRuntimeHome);

	FILE *file = fopen(path, "r");
	if (!file) return false;

	char line[256] = {};
	while (fgets(line, sizeof(line), file))
	{
		char *cursor = line;
		while (*cursor && isspace((unsigned char)*cursor)) cursor++;
		trim_in_place(cursor);
		if (!strcmp(cursor, kRuntimeScaleModeExplicitMarker))
		{
			fclose(file);
			return true;
		}
	}

	fclose(file);
	return false;
}

static bool runtime_scale_mode_has_auto_marker()
{
	char path[PATH_MAX] = {};
	snprintf(path, sizeof(path), "%s/config", kRuntimeHome);

	FILE *file = fopen(path, "r");
	if (!file) return false;

	char line[256] = {};
	while (fgets(line, sizeof(line), file))
	{
		char *cursor = line;
		while (*cursor && isspace((unsigned char)*cursor)) cursor++;
		trim_in_place(cursor);
		if (!strcmp(cursor, kRuntimeScaleModeAutoMarker))
		{
			fclose(file);
			return true;
		}
	}

	fclose(file);
	return false;
}

static bool runtime_config_matches_generated_defaults()
{
	char path[PATH_MAX] = {};
	snprintf(path, sizeof(path), "%s/config", kRuntimeHome);

	FILE *file = fopen(path, "r");
	if (!file) return false;

	bool seen[sizeof(kRuntimeGeneratedDefaults) / sizeof(kRuntimeGeneratedDefaults[0])] = {};
	char line[256] = {};
	while (fgets(line, sizeof(line), file))
	{
		char *cursor = line;
		while (*cursor && isspace((unsigned char)*cursor)) cursor++;
		if (!*cursor || *cursor == '#') continue;

		char *equals = strchr(cursor, '=');
		if (!equals)
		{
			fclose(file);
			return false;
		}

		*equals = 0;
		char *key = cursor;
		char *raw_value = equals + 1;
		trim_in_place(key);
		trim_in_place(raw_value);

		bool matched = false;
		for (size_t i = 0; i < (sizeof(kRuntimeGeneratedDefaults) / sizeof(kRuntimeGeneratedDefaults[0])); ++i)
		{
			if (strcasecmp(key, kRuntimeGeneratedDefaults[i].key) != 0) continue;
			if (strcasecmp(raw_value, kRuntimeGeneratedDefaults[i].value) != 0)
			{
				fclose(file);
				return false;
			}

			seen[i] = true;
			matched = true;
			break;
		}

		if (!matched)
		{
			fclose(file);
			return false;
		}
	}

	fclose(file);

	for (size_t i = 0; i < (sizeof(kRuntimeGeneratedDefaults) / sizeof(kRuntimeGeneratedDefaults[0])); ++i)
	{
		if (!seen[i]) return false;
	}

	return true;
}

static bool runtime_scale_mode_is_generated_default(const char *value)
{
	return value && !strcasecmp(value, "native") && !runtime_scale_mode_has_explicit_marker() &&
	       runtime_config_matches_generated_defaults();
}

static bool runtime_scale_mode_is_auto_default(const char *value)
{
	if (!value || strcasecmp(value, "native") != 0 || runtime_scale_mode_has_explicit_marker()) return false;
	return runtime_scale_mode_has_auto_marker() || runtime_scale_mode_is_generated_default(value);
}

int read_runtime_scale_mode_default()
{
	char value[64] = {};
	if (!read_runtime_config_value("scale-mode", value, sizeof(value))) return kScaleModeAuto;
	if (runtime_scale_mode_is_auto_default(value)) return kScaleModeAuto;

	if (!strcasecmp(value, "native")) return kScaleModeNative;
	if (!strcasecmp(value, "nearest")) return kScaleModeNearest;
	return kScaleModeAuto;
}

static const char *runtime_scale_mode_config_value(int mode)
{
	switch (mode)
	{
	case kScaleModeNative: return "native";
	case kScaleModeNearest: return "nearest";
	default: return nullptr;
	}
}

int read_runtime_super_effect_quality_default()
{
	char value[64] = {};
	if (!read_runtime_config_value("super-effect-quality", value, sizeof(value))) return kSuperEffectQualityCachedBg;

	if (!strcasecmp(value, "cached-bg"))
		return kSuperEffectQualityCachedBg;
	if (!strcasecmp(value, "full")) return kSuperEffectQualityFull;
	return kSuperEffectQualityCachedBg;
}

static const char *runtime_super_effect_quality_config_value(int mode)
{
	switch (mode)
	{
	case kSuperEffectQualityCachedBg: return "cached-bg";
	default: return "full";
	}
}

static bool is_native_analog_tv_output_mode(int vga_mode_int)
{
	return vga_mode_int == 1 || vga_mode_int == 2 || vga_mode_int == 3;
}

bool write_runtime_scale_mode_default(int mode)
{
	char path[PATH_MAX] = {};
	char temp_path[PATH_MAX] = {};
	snprintf(path, sizeof(path), "%s/config", kRuntimeHome);
	snprintf(temp_path, sizeof(temp_path), "%s/config.tmp", kRuntimeHome);

	FILE *in = fopen(path, "r");
	FILE *out = fopen(temp_path, "w");
	if (!out)
	{
		if (in) fclose(in);
		return false;
	}

	bool wrote_scale_mode = false;
	bool wrote_scale_mode_marker = false;
	bool wrote_scale_mode_auto_marker = false;
	char line[256] = {};
	if (in)
	{
		while (fgets(line, sizeof(line), in))
		{
			char inspect[256] = {};
			snprintf(inspect, sizeof(inspect), "%s", line);

			char *cursor = inspect;
			while (*cursor && isspace((unsigned char)*cursor)) cursor++;
			trim_in_place(cursor);
				if (*cursor == '#')
				{
					if (!strcmp(cursor, kRuntimeScaleModeExplicitMarker))
					{
					if (mode != kScaleModeAuto)
					{
						fprintf(out, "%s\n", kRuntimeScaleModeExplicitMarker);
						wrote_scale_mode_marker = true;
						}
						continue;
					}

					if (!strcmp(cursor, kRuntimeScaleModeAutoMarker))
					{
						if (mode == kScaleModeAuto)
						{
							fprintf(out, "%s\n", kRuntimeScaleModeAutoMarker);
							wrote_scale_mode_auto_marker = true;
						}
						continue;
					}

					fputs(line, out);
					continue;
			}

			char *equals = strchr(cursor, '=');
			if (equals)
			{
				*equals = 0;
				trim_in_place(cursor);
				if (!strcasecmp(cursor, "scale-mode"))
				{
					const char *config_value = runtime_scale_mode_config_value(mode);
					if (config_value != nullptr)
					{
						fprintf(out, "scale-mode = %s\n", config_value);
						wrote_scale_mode = true;
					}
					continue;
				}
			}

			fputs(line, out);
		}

		fclose(in);
	}

	if (!wrote_scale_mode)
	{
		const char *config_value = runtime_scale_mode_config_value(mode);
		if (config_value != nullptr)
		{
			fprintf(out, "\nscale-mode = %s\n", config_value);
		}
	}

	if ((mode != kScaleModeAuto) && !wrote_scale_mode_marker)
	{
		fprintf(out, "%s\n", kRuntimeScaleModeExplicitMarker);
	}
	else if ((mode == kScaleModeAuto) && !wrote_scale_mode_auto_marker)
	{
		fprintf(out, "%s\n", kRuntimeScaleModeAutoMarker);
	}

	if (fclose(out) != 0) return false;
	if (rename(temp_path, path) != 0)
	{
		remove(temp_path);
		return false;
	}

	return true;
}

bool write_runtime_super_effect_quality_default(int mode)
{
	char path[PATH_MAX] = {};
	char temp_path[PATH_MAX] = {};
	snprintf(path, sizeof(path), "%s/config", kRuntimeHome);
	snprintf(temp_path, sizeof(temp_path), "%s/config.tmp", kRuntimeHome);

	FILE *in = fopen(path, "r");
	FILE *out = fopen(temp_path, "w");
	if (!out)
	{
		if (in) fclose(in);
		return false;
	}

	bool wrote_value = false;
	char line[256] = {};
	if (in)
	{
		while (fgets(line, sizeof(line), in))
		{
			char inspect[256] = {};
			snprintf(inspect, sizeof(inspect), "%s", line);

			char *cursor = inspect;
			while (*cursor && isspace((unsigned char)*cursor)) cursor++;
			if (*cursor == '#')
			{
				fputs(line, out);
				continue;
			}

			char *equals = strchr(cursor, '=');
			if (equals)
			{
				*equals = 0;
				trim_in_place(cursor);
				if (!strcasecmp(cursor, "super-effect-quality"))
				{
					fprintf(out, "super-effect-quality = %s\n", runtime_super_effect_quality_config_value(mode));
					wrote_value = true;
					continue;
				}
			}

			fputs(line, out);
		}

		fclose(in);
	}

	if (!wrote_value)
	{
		fprintf(out, "\nsuper-effect-quality = %s\n", runtime_super_effect_quality_config_value(mode));
	}

	if (fclose(out) != 0) return false;
	if (rename(temp_path, path) != 0)
	{
		remove(temp_path);
		return false;
	}

	return true;
}

int read_runtime_ghost_resolution_default()
{
	char value[64] = {};
	if (!read_runtime_config_value("ghost-resolution", value, sizeof(value))) return kGhostResolutionFull;

	if (!strcasecmp(value, "half")) return kGhostResolutionHalf;
	return kGhostResolutionFull;
}


static const char *runtime_ghost_resolution_config_value(int mode)
{
	switch (mode)
	{
	case kGhostResolutionHalf: return "half";
	default: return "full";
	}
}

bool write_runtime_ghost_resolution_default(int mode)
{
	char path[PATH_MAX] = {};
	char temp_path[PATH_MAX] = {};
	snprintf(path, sizeof(path), "%s/config", kRuntimeHome);
	snprintf(temp_path, sizeof(temp_path), "%s/config.tmp", kRuntimeHome);

	FILE *in = fopen(path, "r");
	FILE *out = fopen(temp_path, "w");
	if (!out)
	{
		if (in) fclose(in);
		return false;
	}

	bool wrote_value = false;
	char line[256] = {};
	if (in)
	{
		while (fgets(line, sizeof(line), in))
		{
			char inspect[256] = {};
			snprintf(inspect, sizeof(inspect), "%s", line);

			char *cursor = inspect;
			while (*cursor && isspace((unsigned char)*cursor)) cursor++;
			if (*cursor == '#')
			{
				fputs(line, out);
				continue;
			}

			char *equals = strchr(cursor, '=');
			if (equals)
			{
				*equals = 0;
				trim_in_place(cursor);
				if (!strcasecmp(cursor, "ghost-resolution"))
				{
					fprintf(out, "ghost-resolution = %s\n", runtime_ghost_resolution_config_value(mode));
					wrote_value = true;
					continue;
				}
			}

			fputs(line, out);
		}

		fclose(in);
	}

	if (!wrote_value)
	{
		fprintf(out, "\nghost-resolution = %s\n", runtime_ghost_resolution_config_value(mode));
	}

	if (fclose(out) != 0) return false;
	if (rename(temp_path, path) != 0)
	{
		remove(temp_path);
		return false;
	}

	return true;
}

int read_runtime_ghost_count_default()
{
	char value[64] = {};
	if (!read_runtime_config_value("ghost-count", value, sizeof(value))) return kGhostCount4;

	int val = atoi(value);
	if (val == 0) return kGhostCount0;
	if (val == 1) return kGhostCount1;
	if (val == 2) return kGhostCount2;
	if (val == 3) return kGhostCount3;
	return kGhostCount4;
}


static const char *runtime_ghost_count_config_value(int mode)
{
	switch (mode)
	{
	case kGhostCount0: return "0";
	case kGhostCount1: return "1";
	case kGhostCount2: return "2";
	case kGhostCount3: return "3";
	default: return "4";
	}
}

bool write_runtime_ghost_count_default(int mode)
{
	char path[PATH_MAX] = {};
	char temp_path[PATH_MAX] = {};
	snprintf(path, sizeof(path), "%s/config", kRuntimeHome);
	snprintf(temp_path, sizeof(temp_path), "%s/config.tmp", kRuntimeHome);

	FILE *in = fopen(path, "r");
	FILE *out = fopen(temp_path, "w");
	if (!out)
	{
		if (in) fclose(in);
		return false;
	}

	bool wrote_value = false;
	char line[256] = {};
	if (in)
	{
		while (fgets(line, sizeof(line), in))
		{
			char inspect[256] = {};
			snprintf(inspect, sizeof(inspect), "%s", line);

			char *cursor = inspect;
			while (*cursor && isspace((unsigned char)*cursor)) cursor++;
			if (*cursor == '#')
			{
				fputs(line, out);
				continue;
			}

			char *equals = strchr(cursor, '=');
			if (equals)
			{
				*equals = 0;
				trim_in_place(cursor);
				if (!strcasecmp(cursor, "ghost-count"))
				{
					fprintf(out, "ghost-count = %s\n", runtime_ghost_count_config_value(mode));
					wrote_value = true;
					continue;
				}
			}

			fputs(line, out);
		}

		fclose(in);
	}

	if (!wrote_value)
	{
		fprintf(out, "\nghost-count = %s\n", runtime_ghost_count_config_value(mode));
	}

	if (fclose(out) != 0) return false;
	if (rename(temp_path, path) != 0)
	{
		remove(temp_path);
		return false;
	}

	return true;
}

int read_runtime_arm_clock_default()
{
	char value[64] = {};
	if (!read_runtime_config_value("arm-clock", value, sizeof(value))) return kArmClockStock;

	if (!strcmp(value, "1000")) return kArmClock1000;
	if (!strcmp(value, "1200")) return kArmClock1200;
	return kArmClockStock;
}

static const char *runtime_arm_clock_config_value(int mode)
{
	switch (mode)
	{
	case kArmClock1000: return "1000";
	case kArmClock1200: return "1200";
	default: return "stock";
	}
}

bool write_runtime_arm_clock_default(int mode)
{
	char path[PATH_MAX] = {};
	char temp_path[PATH_MAX] = {};
	snprintf(path, sizeof(path), "%s/config", kRuntimeHome);
	snprintf(temp_path, sizeof(temp_path), "%s/config.tmp", kRuntimeHome);

	FILE *in = fopen(path, "r");
	FILE *out = fopen(temp_path, "w");
	if (!out)
	{
		if (in) fclose(in);
		return false;
	}

	bool wrote_value = false;
	char line[256] = {};
	if (in)
	{
		while (fgets(line, sizeof(line), in))
		{
			char inspect[256] = {};
			snprintf(inspect, sizeof(inspect), "%s", line);

			char *cursor = inspect;
			while (*cursor && isspace((unsigned char)*cursor)) cursor++;
			if (*cursor == '#')
			{
				fputs(line, out);
				continue;
			}

			char *equals = strchr(cursor, '=');
			if (equals)
			{
				*equals = 0;
				trim_in_place(cursor);
				if (!strcasecmp(cursor, "arm-clock"))
				{
					fprintf(out, "arm-clock = %s\n", runtime_arm_clock_config_value(mode));
					wrote_value = true;
					continue;
				}
			}

			fputs(line, out);
		}

		fclose(in);
	}

	if (!wrote_value)
	{
		fprintf(out, "\narm-clock = %s\n", runtime_arm_clock_config_value(mode));
	}

	if (fclose(out) != 0) return false;
	if (rename(temp_path, path) != 0)
	{
		remove(temp_path);
		return false;
	}

	return true;
}

int read_runtime_game_mode_default()
{
	char value[64] = {};
	if (!read_runtime_config_value("game-mode", value, sizeof(value))) return kGameModeConsole;

	if (!strcasecmp(value, "arcade")) return kGameModeArcade;
	return kGameModeConsole;
}

static const char *runtime_game_mode_config_value(int mode)
{
	switch (mode)
	{
	case kGameModeArcade: return "arcade";
	default: return "console";
	}
}

bool write_runtime_game_mode_default(int mode)
{
	char path[PATH_MAX] = {};
	char temp_path[PATH_MAX] = {};
	snprintf(path, sizeof(path), "%s/config", kRuntimeHome);
	snprintf(temp_path, sizeof(temp_path), "%s/config.tmp", kRuntimeHome);

	FILE *in = fopen(path, "r");
	FILE *out = fopen(temp_path, "w");
	if (!out)
	{
		if (in) fclose(in);
		return false;
	}

	bool wrote_value = false;
	char line[256] = {};
	if (in)
	{
		while (fgets(line, sizeof(line), in))
		{
			char inspect[256] = {};
			snprintf(inspect, sizeof(inspect), "%s", line);

			char *cursor = inspect;
			while (*cursor && isspace((unsigned char)*cursor)) cursor++;
			if (*cursor == '#')
			{
				fputs(line, out);
				continue;
			}

			char *equals = strchr(cursor, '=');
			if (equals)
			{
				*equals = 0;
				trim_in_place(cursor);
				if (!strcasecmp(cursor, "game-mode"))
				{
					fprintf(out, "game-mode = %s\n", runtime_game_mode_config_value(mode));
					wrote_value = true;
					continue;
				}
			}

			fputs(line, out);
		}

		fclose(in);
	}

	if (!wrote_value)
	{
		fprintf(out, "\ngame-mode = %s\n", runtime_game_mode_config_value(mode));
	}

	if (fclose(out) != 0) return false;
	if (rename(temp_path, path) != 0)
	{
		remove(temp_path);
		return false;
	}

	return true;
}

int read_runtime_hold_to_pause_default()
{
	char value[64] = {};
	if (!read_runtime_config_value("hold-to-pause", value, sizeof(value))) return kHoldToPauseOff;

	if (!strcasecmp(value, "on")) return kHoldToPauseOn;
	return kHoldToPauseOff;
}

static const char *runtime_hold_to_pause_config_value(int mode)
{
	switch (mode)
	{
	case kHoldToPauseOn: return "on";
	default: return "off";
	}
}

bool write_runtime_hold_to_pause_default(int mode)
{
	char path[PATH_MAX] = {};
	char temp_path[PATH_MAX] = {};
	snprintf(path, sizeof(path), "%s/config", kRuntimeHome);
	snprintf(temp_path, sizeof(temp_path), "%s/config.tmp", kRuntimeHome);

	FILE *in = fopen(path, "r");
	FILE *out = fopen(temp_path, "w");
	if (!out)
	{
		if (in) fclose(in);
		return false;
	}

	bool wrote_value = false;
	char line[256] = {};
	if (in)
	{
		while (fgets(line, sizeof(line), in))
		{
			char inspect[256] = {};
			snprintf(inspect, sizeof(inspect), "%s", line);

			char *cursor = inspect;
			while (*cursor && isspace((unsigned char)*cursor)) cursor++;
			if (*cursor == '#')
			{
				fputs(line, out);
				continue;
			}

			char *equals = strchr(cursor, '=');
			if (equals)
			{
				*equals = 0;
				trim_in_place(cursor);
				if (!strcasecmp(cursor, "hold-to-pause"))
				{
					fprintf(out, "hold-to-pause = %s\n", runtime_hold_to_pause_config_value(mode));
					wrote_value = true;
					continue;
				}
			}

			fputs(line, out);
		}

		fclose(in);
	}

	if (!wrote_value)
	{
		fprintf(out, "\nhold-to-pause = %s\n", runtime_hold_to_pause_config_value(mode));
	}

	if (fclose(out) != 0) return false;
	if (rename(temp_path, path) != 0)
	{
		remove(temp_path);
		return false;
	}

	return true;
}

StartupScaleModeSelection resolve_startup_scale_mode()
{
	StartupScaleModeSelection selection = {};
	selection.direct_video = cfg.direct_video;
	selection.vga_scaler = cfg.vga_scaler;
	selection.forced_scandoubler = cfg.forced_scandoubler;
	selection.io_type = fpga_get_io_type();
	selection.vga_mode_int = cfg.vga_mode_int;
	snprintf(selection.vga_mode, sizeof(selection.vga_mode), "%s", cfg.vga_mode[0] ? cfg.vga_mode : "rgb");

	char configured_value[sizeof(selection.value)] = {};
	if (read_runtime_config_value("scale-mode", configured_value, sizeof(configured_value)))
	{
		if (!runtime_scale_mode_is_auto_default(configured_value))
		{
			snprintf(selection.source, sizeof(selection.source), "config-explicit");
			snprintf(selection.value, sizeof(selection.value), "%s", configured_value);
			return selection;
		}
	}

	selection.set_env = 1;
	if (selection.direct_video != 0)
	{
		snprintf(selection.source, sizeof(selection.source), "auto-direct-video");
		snprintf(selection.value, sizeof(selection.value), "native");
	}
	else if ((selection.io_type == 0) && (selection.vga_scaler == 0) && (selection.forced_scandoubler == 0) &&
	         is_native_analog_tv_output_mode(selection.vga_mode_int))
	{
		snprintf(selection.source, sizeof(selection.source), "auto-native-analog");
		snprintf(selection.value, sizeof(selection.value), "native");
	}
	else if (selection.io_type == 0)
	{
		snprintf(selection.source, sizeof(selection.source), "auto-analog-io");
		snprintf(selection.value, sizeof(selection.value), "native");
	}
	else
	{
		snprintf(selection.source, sizeof(selection.source), "auto-scaled-path");
		snprintf(selection.value, sizeof(selection.value), "nearest");
	}

	return selection;
}

void set_runtime_environment(const StartupScaleModeSelection &startup_scale_mode)
{
	setenv("THIRDSARM_HOME", kRuntimeHome, 1);
	setenv("SDL_VIDEODRIVER", "dummy", 1);
	setenv("SDL_VIDEO_DRIVER", "dummy", 1);
	setenv("SDL_RENDER_DRIVER", "software", 1);
	if (startup_scale_mode.set_env)
	{
		setenv(kRuntimeScaleModeEnv, startup_scale_mode.value, 1);
	}
	else
	{
		unsetenv(kRuntimeScaleModeEnv);
	}

	setenv("LD_LIBRARY_PATH", kRuntimeLibDir, 1);
}

void split_message_line(const char *start, char *line, size_t line_size)
{
	size_t index = 0;
	if (!line_size) return;

	while (start[index] && start[index] != '\n' && index + 1 < line_size)
	{
		line[index] = start[index];
		index++;
	}
	line[index] = 0;
}

void show_wrapper_message(const char *title, const char *message)
{
	OsdSetSize(8);
	OsdSetTitle(title, 0);
	OsdClear();

	const char *cursor = message;
	for (unsigned char line = 0; line < 7 && cursor && *cursor; ++line)
	{
		char text[33] = {};
		split_message_line(cursor, text, sizeof(text));
		OsdWrite(line, text);

		const char *newline = strchr(cursor, '\n');
		cursor = newline ? newline + 1 : nullptr;
	}

	OsdEnable(OSD_MSG);
	OsdUpdate();
}

void disable_wrapper_osd()
{
	OsdMenuCtl(0);
	OsdDisable();
	OsdUpdate();
}

int read_runtime_fps_default()
{
	char value[64] = {};
	if (!read_runtime_config_value("show-fps", value, sizeof(value))) return kFpsOverlayOff;
	if (!strcasecmp(value, "fps")) return kFpsOverlayFps;
	if (!strcasecmp(value, "debug")) return kFpsOverlayDebug;
	return kFpsOverlayOff;
}

static const char *runtime_fps_mode_config_value(int mode)
{
	switch (mode)
	{
	case kFpsOverlayFps: return "fps";
	case kFpsOverlayDebug: return "debug";
	default: return "off";
	}
}

bool write_runtime_fps_default(int mode)
{
	const char *mode_str = runtime_fps_mode_config_value(mode);
	char path[PATH_MAX] = {};
	char temp_path[PATH_MAX] = {};
	snprintf(path, sizeof(path), "%s/config", kRuntimeHome);
	snprintf(temp_path, sizeof(temp_path), "%s/config.tmp", kRuntimeHome);

	FILE *in = fopen(path, "r");
	FILE *out = fopen(temp_path, "w");
	if (!out)
	{
		if (in) fclose(in);
		return false;
	}

	bool wrote_value = false;
	char line[256] = {};
	if (in)
	{
		while (fgets(line, sizeof(line), in))
		{
			char inspect[256] = {};
			snprintf(inspect, sizeof(inspect), "%s", line);

			char *cursor = inspect;
			while (*cursor && isspace((unsigned char)*cursor)) cursor++;
			if (*cursor == '#')
			{
				fputs(line, out);
				continue;
			}

			char *equals = strchr(cursor, '=');
			if (equals)
			{
				*equals = 0;
				trim_in_place(cursor);
				if (!strcasecmp(cursor, "show-fps"))
				{
					fprintf(out, "show-fps = %s\n", mode_str);
					wrote_value = true;
					continue;
				}
			}

			fputs(line, out);
		}

		fclose(in);
	}

	if (!wrote_value)
	{
		fprintf(out, "\nshow-fps = %s\n", mode_str);
	}

	if (fclose(out) != 0) return false;
	if (rename(temp_path, path) != 0)
	{
		remove(temp_path);
		return false;
	}

	return true;
}

void poll_status_changes(pid_t child)
{
	// Cache previous status bits for change detection.
	// Initialized to 0xFFFFFFFF so the first poll detects
	// the seeded values and sends appropriate cycle signals.
	static uint32_t prev_fps = 0xFFFFFFFF;
	static uint32_t prev_sa_activation = 0xFFFFFFFF;
	static uint32_t prev_ghost_res = 0xFFFFFFFF;
	static uint32_t prev_ghost_count = 0xFFFFFFFF;
	static uint32_t prev_arm_clock = 0xFFFFFFFF;
	static uint32_t prev_game_mode = 0xFFFFFFFF;
	static uint32_t prev_hold_to_pause = 0xFFFFFFFF;

	// --- Option bits: detect changes and apply ---

	uint32_t fps = user_io_status_get("[11:10]");
	if (fps != prev_fps) {
		prev_fps = fps;
		// CONF_STR: 0=Off, 1=FPS, 2=Debug (matches kFpsOverlay* enums)
		int target = (int)fps;
		if (target != g_wrapper_fps_mode) {
			write_runtime_fps_default(target);
			// FPS signal is a toggle (cycles Off->FPS->Debug->Off).
			// Compute the number of toggles needed to reach the target.
			int toggles = (target - g_wrapper_fps_mode + kFpsOverlayModeCount)
			              % kFpsOverlayModeCount;
			g_wrapper_fps_mode = target;
			for (int i = 0; i < toggles; i++)
				kill(child, kRuntimeFpsToggleSignal);
		}
	}

	uint32_t sa_activation = user_io_status_get("[14]");
	if (sa_activation != prev_sa_activation) {
		prev_sa_activation = sa_activation;
		// Enum values match CONF_STR option order (0=Full, 1=CachedBg)
		int target = (int)sa_activation;
		if (target != g_wrapper_super_effect_quality) {
			write_runtime_super_effect_quality_default(target);
			int cycles = (target - g_wrapper_super_effect_quality
			              + kSuperEffectQualityMenuCount)
			             % kSuperEffectQualityMenuCount;
			g_wrapper_super_effect_quality = target;
			for (int i = 0; i < cycles; i++)
				kill(child, kRuntimeSuperEffectQualityCycleSignal);
		}
	}

	uint32_t ghost_res = user_io_status_get("[15]");
	if (ghost_res != prev_ghost_res) {
		prev_ghost_res = ghost_res;
		int target = (int)ghost_res;
		if (target != g_wrapper_ghost_resolution) {
			write_runtime_ghost_resolution_default(target);
			int cycles = (target - g_wrapper_ghost_resolution
			              + kGhostResolutionMenuCount)
			             % kGhostResolutionMenuCount;
			g_wrapper_ghost_resolution = target;
			for (int i = 0; i < cycles; i++)
				kill(child, kRuntimeGhostResolutionCycleSignal);
		}
	}

	uint32_t ghost_count = user_io_status_get("[18:16]");
	if (ghost_count != prev_ghost_count) {
		prev_ghost_count = ghost_count;
		int target = (int)ghost_count;
		if (target != g_wrapper_ghost_count) {
			write_runtime_ghost_count_default(target);
			int cycles = (target - g_wrapper_ghost_count
			              + kGhostCountMenuCount)
			             % kGhostCountMenuCount;
			g_wrapper_ghost_count = target;
			for (int i = 0; i < cycles; i++)
				kill(child, kRuntimeGhostCountCycleSignal);
		}
	}

	uint32_t arm_clock = user_io_status_get("[20:19]");
	if (arm_clock != prev_arm_clock) {
		prev_arm_clock = arm_clock;
		int target = (int)arm_clock;
		if (target != g_wrapper_arm_clock) {
			write_runtime_arm_clock_default(target);
			// Overclock defers to restart -- update the pending value only.
			// g_wrapper_arm_clock_active is applied in the restart loop.
			g_wrapper_arm_clock = target;
			// Do NOT send kRuntimeArmClockCycleSignal here.
		}
	}

	uint32_t game_mode = user_io_status_get("[13]");
	if (game_mode != prev_game_mode) {
		prev_game_mode = game_mode;
		int target = (int)game_mode;
		if (target != g_wrapper_game_mode) {
			write_runtime_game_mode_default(target);
			g_wrapper_game_mode = target;
			kill(child, kRuntimeGameModeCycleSignal);
		}
	}

	uint32_t hold_to_pause = user_io_status_get("[24]");
	if (hold_to_pause != prev_hold_to_pause) {
		prev_hold_to_pause = hold_to_pause;
		int target = (int)hold_to_pause;
		if (target != g_wrapper_hold_to_pause) {
			write_runtime_hold_to_pause_default(target);
			g_wrapper_hold_to_pause = target;
			kill(child, kRuntimeHoldToPauseCycleSignal);
		}
	}

	// --- T-type triggers (Reset/Restart) ---
	// HandleUI() pulses T bits (set 1 then 0) within a single call.
	// user_io_status_trigger_take() captures the pulse via a sticky flag
	// inside user_io_status_set().
	uint32_t triggers = user_io_status_trigger_take();

	if (triggers & (1u << 21)) {
		// Reset to Default
		user_io_status_set("[11:10]", 0); // FPS off
		user_io_status_set("[14]", kSuperEffectQualityCachedBg); // SA default=CachedBg
		user_io_status_set("[15]", 0);    // Ghost Res = Full (enum 0)
		user_io_status_set("[18:16]", kGhostCount4); // Ghost Count default=4
		user_io_status_set("[20:19]", 0); // Overclock = Stock
		user_io_status_set("[13]", 0);    // Game Mode = Console
		user_io_status_set("[24]", 0);    // Hold to Pause = Off
		prev_fps = 0xFFFFFFFF;
		prev_sa_activation = 0xFFFFFFFF;
		prev_ghost_res = 0xFFFFFFFF;
		prev_ghost_count = 0xFFFFFFFF;
		prev_arm_clock = 0xFFFFFFFF;
		prev_game_mode = 0xFFFFFFFF;
		prev_hold_to_pause = 0xFFFFFFFF;
	}

	if (triggers & (1u << 22)) {
		// Restart
		g_wrapper_restart_requested = 1;
		kill(child, SIGTERM);
	}
}


// Clear the DDR3 native video control word to prevent stale frame data.
// Maps just the first page of the 0x3A000000 region, zeroes the 4-byte
// control word, then unmaps.
void clear_native_video_ddr3_ctrl()
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) return;
	void *map = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x3A000000);
	if (map != MAP_FAILED)
	{
		*(volatile uint32_t *)map = 0;
		munmap(map, 4096);
	}
	close(fd);
}

void restart_to_menu(FILE *wrapper_log, int saved_stdout, int saved_stderr, int runtime_vt)
{
	write_log_line(wrapper_log, "return_to_menu=1");
	if (g_native_video_mode)
	{
		clear_native_video_ddr3_ctrl();
		user_io_status_set("[9]", 0);
		video_set_native_video_enabled(false);
		g_native_video_mode = false;
	}
	set_vga_fb(0);
	video_fb_enable(0);
	video_refresh_yc_mode();
	restore_runtime_console_mode(wrapper_log, runtime_vt);
	restore_console(1);
	const char *menu_exec = getFullPath(kMenuExec);

	sync();
	input_switch(0);
	int menu_load_rc = fpga_load_rbf_no_restart(kMenuCore);
	if (menu_load_rc != 0)
	{
		write_log_line(wrapper_log, "return_to_menu=load_failed rc=%d", menu_load_rc);
		restore_stdio(saved_stdout, saved_stderr);
		if (wrapper_log) fclose(wrapper_log);
		reboot(1);
	}

	restore_stdio(saved_stdout, saved_stderr);
	if (!menu_exec || !menu_exec[0])
	{
		write_log_line(wrapper_log, "return_to_menu=missing_exec");
		if (wrapper_log) fclose(wrapper_log);
		reboot(1);
	}

	write_log_line(wrapper_log, "return_to_menu=exec");
	if (wrapper_log) fclose(wrapper_log);

	execl(menu_exec, menu_exec, kMenuCore, "", nullptr);
	if (wrapper_log)
	{
		wrapper_log = fopen(kWrapperLogPath, "a");
		if (wrapper_log)
		{
			write_log_line(wrapper_log, "return_to_menu=exec_failed errno=%d", errno);
			fclose(wrapper_log);
		}
	}
	fprintf(stderr, "restart_to_menu: execl(%s) failed: %s\n", menu_exec, strerror(errno));
	reboot(1);
}

int show_error_and_return(const char *message, FILE *wrapper_log, int active_vt, int saved_stdout, int saved_stderr)
{
	write_log_line(wrapper_log, "error=%s", message);
	if (force_requested())
	{
		if (g_native_video_mode)
		{
			clear_native_video_ddr3_ctrl();
			user_io_status_set("[9]", 0);
			video_set_native_video_enabled(false);
			g_native_video_mode = false;
		}
		restore_console(active_vt);
		restore_stdio(saved_stdout, saved_stderr);
		if (wrapper_log) fclose(wrapper_log);
		return 1;
	}

	if (g_native_video_mode)
	{
		clear_native_video_ddr3_ctrl();
		user_io_status_set("[9]", 0);
		video_set_native_video_enabled(false);
		g_native_video_mode = false;
	}
	set_vga_fb(0);
	video_fb_enable(0);
	video_refresh_yc_mode();
	show_wrapper_message(kCoreName, message);
	usleep(1500 * 1000);
	disable_wrapper_osd();
	restore_console(active_vt);
	restart_to_menu(wrapper_log, saved_stdout, saved_stderr, active_vt);
	return 1;
}

void cleanup_joy_shm()
{
	if (g_joy_shm)
	{
		munmap(g_joy_shm, sizeof(MisterJoyShm));
		g_joy_shm = nullptr;
	}
	unlink(MISTER_JOY_SHM_PATH);
}

int validate_runtime_paths(FILE *wrapper_log, int active_vt, int saved_stdout, int saved_stderr)
{
	if (!FileExists(kRuntimeBinary, 0))
	{
		return show_error_and_return("Missing /media/fat/games/3s-arm/bin/3s-arm", wrapper_log, active_vt, saved_stdout, saved_stderr);
	}

	if (!FileExists(kRuntimeArchive, 0))
	{
		return show_error_and_return("Missing /media/fat/games/3s-arm/resources/SF33RD.AFS", wrapper_log, active_vt, saved_stdout, saved_stderr);
	}

	return 0;
}

int init_wrapper_context(bool forced, const char *rbf_path, char *error, size_t error_size)
{
	g_wrapper_used_full_user_io_init = 0;

	if (!forced)
	{
		user_io_init(rbf_path ? rbf_path : kCoreName, nullptr);
		g_wrapper_used_full_user_io_init = 1;

		const char *core_name = user_io_get_core_name();
		const char *orig_name = user_io_get_core_name(1);
		if (matches_core_name(core_name) || matches_core_name(orig_name))
		{
			return 0;
		}

		set_error_message(error, error_size, "Expected loaded core identity 3S-ARM");
		return -1;
	}

	if (thirdsarm_core_context_init(rbf_path, error, error_size) != 0)
	{
		return -1;
	}

	return 0;
}

const char *wrapper_core_name(bool forced)
{
	return forced ? thirdsarm_core_context_core_name() : user_io_get_core_name();
}

const char *wrapper_rbf_name(bool forced, int argc, char *argv[])
{
	if (forced) return (argc > 1) ? argv[1] : "";
	return user_io_get_core_name(1);
}

int wait_for_child(pid_t child, bool service_ui)
{
	int status = 0;
	for (;;)
	{
		pid_t rc = waitpid(child, &status, service_ui ? WNOHANG : 0);
		if (rc == child) break;
		if (rc < 0)
		{
			if (errno == EINTR) continue;
			return -1;
		}

		if (!service_ui)
		{
			continue;
		}

		if (is_fpga_ready(1))
		{
			frame_timer();
			input_poll(0);

			if (g_joy_shm)
			{
				uint32_t masks[MISTER_JOY_MAX_PLAYERS];
				if (input_btncheck_active) {
					memset(masks, 0, sizeof(masks));
				} else {
					input_get_joy_mask(masks, MISTER_JOY_MAX_PLAYERS);
				}
				for (int i = 0; i < MISTER_JOY_MAX_PLAYERS; i++)
					g_joy_shm->joy_mask[i] = masks[i];
			}
		}

		poll_status_changes(child);
		HandleUI();
		OsdUpdate();

		usleep(1000);
	}

	if (WIFEXITED(status)) return WEXITSTATUS(status);
	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return status;
}

}  // namespace

int thirdsarm_wrapper_run(int argc, char *argv[])
{
	const bool forced = force_requested();
	g_wrapper_fps_mode = read_runtime_fps_default();
	g_wrapper_super_effect_quality = read_runtime_super_effect_quality_default();
	g_wrapper_ghost_resolution = read_runtime_ghost_resolution_default();
	g_wrapper_ghost_count = read_runtime_ghost_count_default();
	g_wrapper_arm_clock = read_runtime_arm_clock_default();
	g_wrapper_arm_clock_active = g_wrapper_arm_clock;
	g_wrapper_game_mode = read_runtime_game_mode_default();
	g_wrapper_hold_to_pause = read_runtime_hold_to_pause_default();
	g_wrapper_restart_requested = 0;
	g_wrapper_signal = 0;

	(void)mkdir(kLogDir, 0755);

	FILE *wrapper_log = fopen(kWrapperLogPath, "w");
	if (wrapper_log) setvbuf(wrapper_log, nullptr, _IOLBF, 0);
	int saved_stdout = -1;
	int saved_stderr = -1;
	redirect_stdio_to_log(wrapper_log, &saved_stdout, &saved_stderr);

	char init_error[128] = {};
	if (init_wrapper_context(forced, (argc > 1) ? argv[1] : nullptr, init_error, sizeof(init_error)) != 0)
	{
		write_log_line(wrapper_log, "error=%s", init_error);
		restore_stdio(saved_stdout, saved_stderr);
		if (wrapper_log) fclose(wrapper_log);
		return 1;
	}

	int active_vt = get_active_vt();
	char cwd_buffer[512] = {};
	get_cwd_string(cwd_buffer, sizeof(cwd_buffer));

	if (!g_wrapper_used_full_user_io_init)
	{
		// Forced/probe launches still use the slim path, so initialize persisted
		// volume/filter state explicitly there.
		load_volume();
		user_io_send_buttons(1);
	}

	// Tell HandleUI the core is fully loaded so it renders the CONF_STR menu
	// instead of disabling the OSD (menu.cpp checks mgl->done before enabling).
	mgl_get()->done = 1;

	// Seed CONF_STR status bits from persisted game config so the MiSTer
	// menu reflects the actual runtime settings. This overwrites any values
	// loaded from 3S-ARM.CFG by user_io_init -- the game config is authoritative.
	if (g_wrapper_used_full_user_io_init)
	{
		user_io_status_set("[11:10]", (uint32_t)g_wrapper_fps_mode);
		user_io_status_set("[14]", (uint32_t)g_wrapper_super_effect_quality);
		user_io_status_set("[15]", (uint32_t)g_wrapper_ghost_resolution);
		user_io_status_set("[18:16]", (uint32_t)g_wrapper_ghost_count);
		user_io_status_set("[20:19]", (uint32_t)g_wrapper_arm_clock);
	}

	write_log_line(wrapper_log, "==== 3S-ARM wrapper launch ====");
	write_log_line(wrapper_log, "pid=%d ppid=%d", getpid(), getppid());
	write_log_line(wrapper_log, "cwd=%s", cwd_buffer);
	write_log_line(wrapper_log, "pgrp=%d sid=%d", getpgrp(), getsid(0));
	write_log_line(wrapper_log, "forced_mode=%d", forced ? 1 : 0);
	write_log_line(wrapper_log, "core_name=%s", wrapper_core_name(forced));
	write_log_line(wrapper_log, "rbf_name=%s", wrapper_rbf_name(forced, argc, argv));
	write_log_line(wrapper_log, "active_vt=tty%d", active_vt);
	write_log_line(wrapper_log, "user_io_init_mode=%s", g_wrapper_used_full_user_io_init ? "full" : "slim");
	write_log_line(wrapper_log, "volume_init global=%d core=%d filter=%d", get_volume(), get_core_volume(), audio_filter_en());

	int validation_rc = validate_runtime_paths(wrapper_log, active_vt, saved_stdout, saved_stderr);
	if (validation_rc != 0) return validation_rc;

	const int runtime_vt = runtime_tty2_requested() ? 2 : active_vt;
	/* Detect native video mode: when THIRDSARM_NATIVE_VIDEO=1, the game writes
	   frames directly to DDR3 for the FPGA's native video reader instead of
	   going through the Linux framebuffer scaler path.  In this mode we must
	   NOT enable vga_fb or the FB scaler -- the core outputs video directly
	   via its VGA_R/G/B pins using the native video timing generator.  We set
	   status bit 9 to tell the FPGA to enable its native video reader. */
	{
		const char *nv_env = getenv("THIRDSARM_NATIVE_VIDEO");
		g_native_video_mode = !nv_env || strcmp(nv_env, "0") != 0;
	}
	video_set_native_video_enabled(g_native_video_mode);

	if (!forced)
	{
		disable_wrapper_osd();
		if (g_native_video_mode)
		{
			/* Native video: do NOT enable the scaler/FB path.
			   set_vga_fb(0) is already the default.  Clear any stale DDR3
			   control word, then signal FPGA to start the native video reader. */
			clear_native_video_ddr3_ctrl();
			if (runtime_vt != active_vt) video_chvt(runtime_vt);
			user_io_status_set("[9]", 1);
		}
		else
		{
			video_fb_clear(0);
			set_vga_fb(1);
			if (runtime_vt != active_vt) video_chvt(runtime_vt);
			video_fb_enable(1);
		}
		video_refresh_yc_mode();
	}

	write_log_line(wrapper_log,
	               "runtime_vt target=tty%d switch_env=%s active_before=tty%d",
	               runtime_vt,
	               runtime_tty2_requested() ? "set" : "unset",
	               active_vt);
	write_log_line(wrapper_log, "native_video=%s", g_native_video_mode ? "enabled" : "disabled");

	/* Create shared-memory region for joystick state visible to the game. */
	int shm_fd = open(MISTER_JOY_SHM_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (shm_fd >= 0)
	{
		if (ftruncate(shm_fd, sizeof(MisterJoyShm)) == 0)
		{
			g_joy_shm = (MisterJoyShm *)mmap(NULL, sizeof(MisterJoyShm),
			                                  PROT_READ | PROT_WRITE,
			                                  MAP_SHARED, shm_fd, 0);
			if (g_joy_shm == MAP_FAILED)
				g_joy_shm = nullptr;
		}
		close(shm_fd);
	}
	if (g_joy_shm)
	{
		g_joy_shm->magic = MISTER_JOY_SHM_MAGIC;
		g_joy_shm->version = MISTER_JOY_SHM_VERSION;
		memset(g_joy_shm->joy_mask, 0, sizeof(g_joy_shm->joy_mask));
		setenv("THIRDSARM_JOY_SHM", MISTER_JOY_SHM_PATH, 1);
		write_log_line(wrapper_log, "joy_shm=%s", MISTER_JOY_SHM_PATH);
	}

	for (;;)
	{
		const StartupScaleModeSelection startup_scale_mode = resolve_startup_scale_mode();
		write_log_line(wrapper_log,
		               "startup_scale_mode source=%s value=%s env=%s direct_video=%d vga_scaler=%d forced_scandoubler=%d io_type=%d vga_mode=%s vga_mode_int=%d",
		               startup_scale_mode.source,
		               startup_scale_mode.value,
		               startup_scale_mode.set_env ? kRuntimeScaleModeEnv : "unset",
		               startup_scale_mode.direct_video,
		               startup_scale_mode.vga_scaler,
		               startup_scale_mode.forced_scandoubler,
		               startup_scale_mode.io_type,
		               startup_scale_mode.vga_mode,
		               startup_scale_mode.vga_mode_int);

		set_runtime_environment(startup_scale_mode);

		int last_run_fd = open(kLastRunLogPath, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
		if (last_run_fd < 0)
		{
			cleanup_joy_shm();
			return show_error_and_return("Cannot open /media/fat/games/3s-arm/logs/last-run.log", wrapper_log, active_vt, saved_stdout, saved_stderr);
		}

		write_fd_line(last_run_fd, "==== 3S-ARM wrapper launch ====");
		write_fd_line(last_run_fd, "pid=%d ppid=%d", getpid(), getppid());
		write_fd_line(last_run_fd, "cwd=%s", cwd_buffer);
		write_fd_line(last_run_fd, "pgrp=%d sid=%d", getpgrp(), getsid(0));
		write_fd_line(last_run_fd, "active_vt=tty%d", active_vt);
		write_fd_line(last_run_fd,
		              "startup_scale_mode source=%s value=%s env=%s direct_video=%d vga_scaler=%d forced_scandoubler=%d io_type=%d vga_mode=%s vga_mode_int=%d",
		              startup_scale_mode.source,
		              startup_scale_mode.value,
		              startup_scale_mode.set_env ? kRuntimeScaleModeEnv : "unset",
		              startup_scale_mode.direct_video,
		              startup_scale_mode.vga_scaler,
		              startup_scale_mode.forced_scandoubler,
		              startup_scale_mode.io_type,
		              startup_scale_mode.vga_mode,
		              startup_scale_mode.vga_mode_int);

		int err_pipe[2];
		if (pipe2(err_pipe, O_CLOEXEC) < 0)
		{
			close(last_run_fd);
			cleanup_joy_shm();
			return show_error_and_return("Cannot create 3S-ARM launch pipe", wrapper_log, active_vt, saved_stdout, saved_stderr);
		}

		struct sigaction old_int = {}, old_hup = {}, old_term = {};
		install_signal_handlers(&old_int, &old_hup, &old_term);

		pid_t child = fork();
		if (child < 0)
		{
			restore_signal_handlers(&old_int, &old_hup, &old_term);
			close(err_pipe[0]);
			close(err_pipe[1]);
			close(last_run_fd);
			cleanup_joy_shm();
			return show_error_and_return("Cannot fork 3S-ARM runtime", wrapper_log, active_vt, saved_stdout, saved_stderr);
		}

		if (child == 0)
		{
			close(err_pipe[0]);
			prctl(PR_SET_PDEATHSIG, SIGTERM);

			// Reset CPU affinity so the game isn't pinned to the wrapper's
			// core. On restart, the parent is already pinned to CPU 0 and
			// fork() inherits that affinity.
			cpu_set_t all_cpus;
			CPU_ZERO(&all_cpus);
			CPU_SET(0, &all_cpus);
			CPU_SET(1, &all_cpus);
			sched_setaffinity(0, sizeof(all_cpus), &all_cpus);

			int stdin_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
			if (stdin_fd >= 0)
			{
				dup2(stdin_fd, STDIN_FILENO);
				close(stdin_fd);
			}

			dup2(last_run_fd, STDOUT_FILENO);
			dup2(last_run_fd, STDERR_FILENO);
			if (last_run_fd > STDERR_FILENO) close(last_run_fd);

			std::vector<char *> child_argv;
			child_argv.push_back(const_cast<char *>(kRuntimeBinary));
			for (int i = 2; i < argc; ++i) child_argv.push_back(argv[i]);
			child_argv.push_back(nullptr);

			execve(kRuntimeBinary, child_argv.data(), environ);

			int exec_errno = errno;
			(void)write(err_pipe[1], &exec_errno, sizeof(exec_errno));
			_exit(127);
		}

		g_child_pid = child;
		close(err_pipe[1]);

		int exec_errno = 0;
		ssize_t exec_read = read(err_pipe[0], &exec_errno, sizeof(exec_errno));
		close(err_pipe[0]);

		write_log_line(wrapper_log, "child_pid=%d", child);
		write_log_line(wrapper_log, "runtime=%s", kRuntimeBinary);
		write_log_line(wrapper_log, "THIRDSARM_HOME=%s", kRuntimeHome);
		write_log_line(wrapper_log, "LD_LIBRARY_PATH=%s", getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "");
		write_log_line(wrapper_log, "SDL_VIDEODRIVER=%s", getenv("SDL_VIDEODRIVER") ? getenv("SDL_VIDEODRIVER") : "");
		write_log_line(wrapper_log, "SDL_VIDEO_DRIVER=%s", getenv("SDL_VIDEO_DRIVER") ? getenv("SDL_VIDEO_DRIVER") : "");
		write_log_line(wrapper_log, "SDL_RENDER_DRIVER=%s", getenv("SDL_RENDER_DRIVER") ? getenv("SDL_RENDER_DRIVER") : "");
		write_log_line(wrapper_log,
		               "%s=%s",
		               kRuntimeScaleModeEnv,
		               getenv(kRuntimeScaleModeEnv) ? getenv(kRuntimeScaleModeEnv) : "");

		if (!forced)
		{
			// Keep the wrapper's polling/menu loop off the runtime's CPU.
			pin_current_thread_to_cpu(wrapper_log, "wrapper_ui", 0);
		}

		/* Enable joy passthrough so input_test/joy_digital bypass the
		   video_fb_state() gate and populate key_states for SHM export. */
		input_set_joy_passthrough(1);

		int exit_code = wait_for_child(child, !forced);
		restore_signal_handlers(&old_int, &old_hup, &old_term);
		g_child_pid = -1;

		write_fd_line(last_run_fd, "exit=%d", exit_code);
		write_log_line(wrapper_log, "child_exit=%d", exit_code);

		/* Reset ARM clock to stock after child exits, regardless of exit reason.
		   Catches crashes, SIGKILL, and abnormal termination that bypass the
		   game-side cleanup in SDLApp_Quit(). */
		if (g_wrapper_arm_clock_active != kArmClockStock)
		{
			/* Use unbuffered write for reliable sysfs interaction.
			   Restore min_freq and governor to safe defaults. */
			auto sysfs_reset = [](const char *path, const char *val) {
				int fd = open(path, O_WRONLY);
				if (fd >= 0) { (void)write(fd, val, strlen(val)); close(fd); }
			};
			/* Lower min before max (kernel rejects max < current min) */
			sysfs_reset("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", "800000");
			sysfs_reset("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "800000");
			sysfs_reset("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "ondemand");
			write_log_line(wrapper_log, "arm_clock_reset=stock (post-child)");
		}

		close(last_run_fd);

		if (exec_read > 0)
		{
			char error_message[512] = {};
			snprintf(error_message, sizeof(error_message), "Failed to launch 3S-ARM (%s)", strerror(exec_errno));
			cleanup_joy_shm();
			return show_error_and_return(error_message, wrapper_log, active_vt, saved_stdout, saved_stderr);
		}

		if (g_wrapper_signal)
		{
			int signal_exit = 128 + g_wrapper_signal;
			write_log_line(wrapper_log, "wrapper_signal=%d", g_wrapper_signal);
			if (g_native_video_mode)
			{
				clear_native_video_ddr3_ctrl();
				user_io_status_set("[9]", 0);
				video_set_native_video_enabled(false);
			}
			cleanup_joy_shm();
			restore_console(active_vt);
			restore_stdio(saved_stdout, saved_stderr);
			fclose(wrapper_log);
			return signal_exit;
		}

		if (forced)
		{
			if (g_native_video_mode)
			{
				clear_native_video_ddr3_ctrl();
				user_io_status_set("[9]", 0);
				video_set_native_video_enabled(false);
			}
			cleanup_joy_shm();
			restore_console(active_vt);
			restore_stdio(saved_stdout, saved_stderr);
			fclose(wrapper_log);
			return exit_code;
		}

		if (g_wrapper_restart_requested)
		{
			write_log_line(wrapper_log, "restart_runtime=1");
			if (g_native_video_mode)
				clear_native_video_ddr3_ctrl();
			g_wrapper_restart_requested = 0;
			g_wrapper_arm_clock_active = g_wrapper_arm_clock;

			// Re-seed status bits so the menu reflects current values after restart
			user_io_status_set("[11:10]", (uint32_t)g_wrapper_fps_mode);
			user_io_status_set("[14]", (uint32_t)g_wrapper_super_effect_quality);
			user_io_status_set("[15]", (uint32_t)g_wrapper_ghost_resolution);
			user_io_status_set("[18:16]", (uint32_t)g_wrapper_ghost_count);
			user_io_status_set("[20:19]", (uint32_t)g_wrapper_arm_clock);
			user_io_status_set("[24]", (uint32_t)g_wrapper_hold_to_pause);

			continue;
		}

		cleanup_joy_shm();
		restart_to_menu(wrapper_log, saved_stdout, saved_stderr, runtime_vt);
		return exit_code;
	}
}
