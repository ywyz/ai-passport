#pragma once
#include <stdbool.h>

/* Slot zero is the persistent Refresh action; slots 1..count are tasks. */
unsigned task_list_move(unsigned selected, unsigned count, bool down);
