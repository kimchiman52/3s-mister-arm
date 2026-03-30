#include "threesx_core_context.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cfg.h"
#include "osd.h"
#include "user_io.h"
#include "video.h"

namespace {

constexpr const char *kCoreName = "3SX";

char g_core_name[32] = "3SX";

const char *basename_ptr(const char *path)
{
	if (!path || !path[0]) return nullptr;

	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

void copy_core_name(char *dst, size_t dst_size, const char *path)
{
	if (!dst || !dst_size)
	{
		return;
	}

	const char *base = basename_ptr(path);
	if (!base || !base[0])
	{
		snprintf(dst, dst_size, "%s", kCoreName);
		return;
	}

	snprintf(dst, dst_size, "%s", base);
	char *dot = strrchr(dst, '.');
	if (dot) *dot = 0;
}

void set_error(char *error, size_t error_size, const char *message)
{
	if (!error || !error_size) return;
	snprintf(error, error_size, "%s", message);
}

}  // namespace

int threesx_core_context_init(const char *rbf_path, char *error, size_t error_size)
{
	copy_core_name(g_core_name, sizeof(g_core_name), rbf_path);
	if (strcasecmp(g_core_name, kCoreName))
	{
		set_error(error, error_size, "Expected core identity 3SX");
		return -1;
	}

	user_io_wrapper_set_core_names(kCoreName, g_core_name);
	OsdCoreNameSet(kCoreName);
	cfg_parse();
	video_init();
	return 0;
}

const char *threesx_core_context_core_name()
{
	return g_core_name;
}
