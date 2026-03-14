#ifndef THREESX_CORE_CONTEXT_H
#define THREESX_CORE_CONTEXT_H

#include <stddef.h>

int threesx_core_context_init(const char *rbf_path, char *error, size_t error_size);
const char *threesx_core_context_core_name();

#endif
