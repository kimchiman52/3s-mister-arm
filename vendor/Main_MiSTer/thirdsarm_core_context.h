#ifndef THIRDSARM_CORE_CONTEXT_H
#define THIRDSARM_CORE_CONTEXT_H

#include <stddef.h>

int thirdsarm_core_context_init(const char *rbf_path, char *error, size_t error_size);
const char *thirdsarm_core_context_core_name();

#endif
