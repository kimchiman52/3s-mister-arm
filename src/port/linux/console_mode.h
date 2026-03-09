#ifndef PORT_LINUX_CONSOLE_MODE_H
#define PORT_LINUX_CONSOLE_MODE_H

#include <stdbool.h>

/// Switch the current Linux console to graphics mode.
/// Returns true when the mode was switched successfully.
bool ConsoleMode_Enter(void);

/// Restore the Linux console mode back to text mode.
void ConsoleMode_Exit(void);

#endif
