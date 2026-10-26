#include "suite_tasks_nav.h"

unsigned task_list_move(unsigned selected, unsigned count, bool down)
{
    if (!count) return 0;
    if (selected > count) selected = count;
    if (down) return selected == count ? 0 : selected + 1;
    return selected ? selected - 1 : count;
}
