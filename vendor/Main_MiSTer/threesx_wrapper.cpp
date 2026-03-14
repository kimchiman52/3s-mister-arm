#include "threesx_wrapper.h"

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
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "cfg.h"
#include "file_io.h"
#include "frame_timer.h"
#include "fpga_io.h"
#include "input.h"
#include "audio.h"
#include "osd.h"
#include "threesx_core_context.h"
#include "user_io.h"
#include "video.h"

extern char **environ;

namespace {

constexpr const char *kCoreName = "3SX";
constexpr const char *kRuntimeHome = "/media/fat/games/3sx";
constexpr const char *kRuntimeBinary = "/media/fat/games/3sx/bin/3sx";
constexpr const char *kRuntimeArchive = "/media/fat/games/3sx/resources/SF33RD.AFS";
constexpr const char *kRuntimeLibDir = "/media/fat/games/3sx/lib";
constexpr const char *kLogDir = "/media/fat/games/3sx/logs";
constexpr const char *kWrapperLogPath = "/media/fat/games/3sx/logs/osd-wrapper.log";
constexpr const char *kLastRunLogPath = "/media/fat/games/3sx/logs/last-run.log";
constexpr const char *kRuntimeScaleModeEnv = "THREESX_SCALE_MODE_STARTUP_OVERRIDE";
constexpr const char *kMenuCore = "menu.rbf";
constexpr const char *kMenuExec = "MiSTer";
constexpr const char *kRuntimeTtySwitchEnv = "THREESX_WRAPPER_USE_TTY2";
constexpr int kRuntimeFpsToggleSignal = SIGUSR1;

enum WrapperMenuItem
{
	kMenuResume = 0,
	kMenuFpsToggle,
	kMenuQuit,
	kMenuItemCount
};

volatile sig_atomic_t g_wrapper_signal = 0;
volatile sig_atomic_t g_child_pid = -1;
int g_wrapper_menu_visible = 0;
int g_wrapper_menu_selected = kMenuResume;
int g_wrapper_fps_enabled = 0;
int g_wrapper_used_full_user_io_init = 0;

struct StartupScaleModeSelection
{
	int set_env = 0;
	int direct_video = 0;
	int vga_scaler = 0;
	int io_type = 0;
	char source[32] = {};
	char value[32] = {};
};

void write_log_line(FILE *file, const char *fmt, ...);

bool force_requested()
{
	const char *force = getenv("THREESX_WRAPPER_FORCE");
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

StartupScaleModeSelection resolve_startup_scale_mode()
{
	StartupScaleModeSelection selection = {};
	selection.direct_video = cfg.direct_video;
	selection.vga_scaler = cfg.vga_scaler;
	selection.io_type = fpga_get_io_type();

	char configured_value[sizeof(selection.value)] = {};
	if (read_runtime_config_value("scale-mode", configured_value, sizeof(configured_value)))
	{
		snprintf(selection.source, sizeof(selection.source), "config-explicit");
		snprintf(selection.value, sizeof(selection.value), "%s", configured_value);
		return selection;
	}

	selection.set_env = 1;
	if (selection.direct_video != 0)
	{
		snprintf(selection.source, sizeof(selection.source), "auto-direct-video");
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
	setenv("THREESX_HOME", kRuntimeHome, 1);
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

	const char *existing_ld = getenv("LD_LIBRARY_PATH");
	if (existing_ld && existing_ld[0])
	{
		char buffer[2048] = {};
		snprintf(buffer, sizeof(buffer), "%s:%s", kRuntimeLibDir, existing_ld);
		setenv("LD_LIBRARY_PATH", buffer, 1);
	}
	else
	{
		setenv("LD_LIBRARY_PATH", kRuntimeLibDir, 1);
	}
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
	threesx_wrapper_menu_set_visible(0);
	OsdMenuCtl(0);
	OsdDisable();
	OsdUpdate();
}

bool read_runtime_fps_default()
{
	char value[64] = {};
	if (!read_runtime_config_value("show-fps", value, sizeof(value))) return false;
	return !strcasecmp(value, "true") || !strcasecmp(value, "on") || !strcmp(value, "1");
}

void draw_wrapper_menu(int selected)
{
	char fps_line[33] = {};
	snprintf(fps_line, sizeof(fps_line), " FPS Counter: %s", g_wrapper_fps_enabled ? "On" : "Off");

	OsdSetSize(6);
	OsdSetTitle(kCoreName, 0);
	OsdClear();
	OsdWrite(0, " Menu");
	OsdWrite(1, "");
	OsdWrite(2, " Resume 3SX", selected == kMenuResume);
	OsdWrite(3, fps_line, selected == kMenuFpsToggle);
	OsdWrite(4, " Quit To MiSTer", selected == kMenuQuit);
	threesx_wrapper_menu_set_visible(1);
	OsdMenuCtl(1);
	OsdEnable(DISABLE_KEYBOARD);
	OsdUpdate();
}

void service_wrapper_menu(pid_t child)
{
	unsigned int key = threesx_wrapper_menu_key_take();
	if (!key)
	{
		if (g_wrapper_menu_visible) OsdUpdate();
		return;
	}

	const int released = (key & UPSTROKE) ? 1 : 0;
	key &= ~UPSTROKE;
	if (released) return;

	if (!g_wrapper_menu_visible)
	{
		if (key == KEY_F12 || key == KEY_MENU)
		{
			g_wrapper_menu_selected = kMenuResume;
			g_wrapper_menu_visible = 1;
			draw_wrapper_menu(g_wrapper_menu_selected);
		}
		return;
	}

	switch (key)
	{
	case KEY_F12:
	case KEY_MENU:
	case KEY_ESC:
	case KEY_BACK:
	case KEY_BACKSPACE:
		g_wrapper_menu_visible = 0;
		disable_wrapper_osd();
		return;

	case KEY_UP:
	case KEY_LEFT:
		g_wrapper_menu_selected = (g_wrapper_menu_selected + kMenuItemCount - 1) % kMenuItemCount;
		draw_wrapper_menu(g_wrapper_menu_selected);
		return;

	case KEY_DOWN:
	case KEY_RIGHT:
	case KEY_TAB:
		g_wrapper_menu_selected = (g_wrapper_menu_selected + 1) % kMenuItemCount;
		draw_wrapper_menu(g_wrapper_menu_selected);
		return;

	case KEY_ENTER:
	case KEY_SPACE:
		if (g_wrapper_menu_selected == kMenuFpsToggle)
		{
			if (kill(child, kRuntimeFpsToggleSignal) == 0)
			{
				g_wrapper_fps_enabled = !g_wrapper_fps_enabled;
				draw_wrapper_menu(g_wrapper_menu_selected);
			}
			return;
		}

		if (g_wrapper_menu_selected == kMenuQuit)
		{
			g_wrapper_menu_visible = 0;
			disable_wrapper_osd();
			kill(child, SIGTERM);
			return;
		}

		g_wrapper_menu_visible = 0;
		disable_wrapper_osd();
		return;
	}
}

void restart_to_menu(FILE *wrapper_log, int saved_stdout, int saved_stderr, int runtime_vt)
{
	write_log_line(wrapper_log, "return_to_menu=1");
	set_vga_fb(0);
	video_fb_enable(0);
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
		restore_console(active_vt);
		restore_stdio(saved_stdout, saved_stderr);
		if (wrapper_log) fclose(wrapper_log);
		return 1;
	}

	set_vga_fb(0);
	video_fb_enable(0);
	show_wrapper_message(kCoreName, message);
	usleep(1500 * 1000);
	disable_wrapper_osd();
	restore_console(active_vt);
	restart_to_menu(wrapper_log, saved_stdout, saved_stderr, active_vt);
	return 1;
}

int validate_runtime_paths(FILE *wrapper_log, int active_vt, int saved_stdout, int saved_stderr)
{
	if (!FileExists(kRuntimeBinary, 0))
	{
		return show_error_and_return("Missing /media/fat/games/3sx/bin/3sx", wrapper_log, active_vt, saved_stdout, saved_stderr);
	}

	if (!FileExists(kRuntimeArchive, 0))
	{
		return show_error_and_return("Missing /media/fat/games/3sx/resources/SF33RD.AFS", wrapper_log, active_vt, saved_stdout, saved_stderr);
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

		set_error_message(error, error_size, "Expected loaded core identity 3SX");
		return -1;
	}

	if (threesx_core_context_init(rbf_path, error, error_size) != 0)
	{
		return -1;
	}

	return 0;
}

const char *wrapper_core_name(bool forced)
{
	return forced ? threesx_core_context_core_name() : user_io_get_core_name();
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
		}

		service_wrapper_menu(child);

		usleep(1000);
	}

	if (WIFEXITED(status)) return WEXITSTATUS(status);
	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return status;
}

}  // namespace

int threesx_wrapper_run(int argc, char *argv[])
{
	const bool forced = force_requested();
	g_wrapper_menu_visible = 0;
	g_wrapper_menu_selected = kMenuResume;
	g_wrapper_fps_enabled = read_runtime_fps_default() ? 1 : 0;

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

	write_log_line(wrapper_log, "==== 3SX wrapper launch ====");
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
	if (!forced)
	{
		disable_wrapper_osd();
		video_fb_clear(0);
		set_vga_fb(1);
		if (runtime_vt != active_vt) video_chvt(runtime_vt);
		video_fb_enable(1);
	}

	write_log_line(wrapper_log,
	               "runtime_vt target=tty%d switch_env=%s active_before=tty%d",
	               runtime_vt,
	               runtime_tty2_requested() ? "set" : "unset",
	               active_vt);

	const StartupScaleModeSelection startup_scale_mode = resolve_startup_scale_mode();
	write_log_line(wrapper_log,
	               "startup_scale_mode source=%s value=%s env=%s direct_video=%d vga_scaler=%d io_type=%d",
	               startup_scale_mode.source,
	               startup_scale_mode.value,
	               startup_scale_mode.set_env ? kRuntimeScaleModeEnv : "unset",
	               startup_scale_mode.direct_video,
	               startup_scale_mode.vga_scaler,
	               startup_scale_mode.io_type);

	set_runtime_environment(startup_scale_mode);

	int last_run_fd = open(kLastRunLogPath, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
	if (last_run_fd < 0)
	{
		return show_error_and_return("Cannot open /media/fat/games/3sx/logs/last-run.log", wrapper_log, active_vt, saved_stdout, saved_stderr);
	}

	write_fd_line(last_run_fd, "==== 3SX wrapper launch ====");
	write_fd_line(last_run_fd, "pid=%d ppid=%d", getpid(), getppid());
	write_fd_line(last_run_fd, "cwd=%s", cwd_buffer);
	write_fd_line(last_run_fd, "pgrp=%d sid=%d", getpgrp(), getsid(0));
	write_fd_line(last_run_fd, "active_vt=tty%d", active_vt);
	write_fd_line(last_run_fd,
	              "startup_scale_mode source=%s value=%s env=%s direct_video=%d vga_scaler=%d io_type=%d",
	              startup_scale_mode.source,
	              startup_scale_mode.value,
	              startup_scale_mode.set_env ? kRuntimeScaleModeEnv : "unset",
	              startup_scale_mode.direct_video,
	              startup_scale_mode.vga_scaler,
	              startup_scale_mode.io_type);

	int err_pipe[2];
	if (pipe2(err_pipe, O_CLOEXEC) < 0)
	{
		close(last_run_fd);
		return show_error_and_return("Cannot create 3SX launch pipe", wrapper_log, active_vt, saved_stdout, saved_stderr);
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
		return show_error_and_return("Cannot fork 3SX runtime", wrapper_log, active_vt, saved_stdout, saved_stderr);
	}

	if (child == 0)
	{
		close(err_pipe[0]);

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
	write_log_line(wrapper_log, "THREESX_HOME=%s", kRuntimeHome);
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

	int exit_code = wait_for_child(child, !forced);
	restore_signal_handlers(&old_int, &old_hup, &old_term);
	g_child_pid = -1;

	write_fd_line(last_run_fd, "exit=%d", exit_code);
	write_log_line(wrapper_log, "child_exit=%d", exit_code);

	close(last_run_fd);

	if (exec_read > 0)
	{
		char error_message[512] = {};
		snprintf(error_message, sizeof(error_message), "Failed to launch 3SX (%s)", strerror(exec_errno));
		return show_error_and_return(error_message, wrapper_log, active_vt, saved_stdout, saved_stderr);
	}

	if (g_wrapper_signal)
	{
		int signal_exit = 128 + g_wrapper_signal;
		write_log_line(wrapper_log, "wrapper_signal=%d", g_wrapper_signal);
		restore_console(active_vt);
		restore_stdio(saved_stdout, saved_stderr);
		fclose(wrapper_log);
		return signal_exit;
	}

	if (forced)
	{
		restore_console(active_vt);
		restore_stdio(saved_stdout, saved_stderr);
		fclose(wrapper_log);
		return exit_code;
	}

	restart_to_menu(wrapper_log, saved_stdout, saved_stderr, runtime_vt);
	return exit_code;
}
