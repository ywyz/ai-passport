/**
 * travel_app.h — travel application shell (D1).
 */
#ifndef TRAVEL_APP_H
#define TRAVEL_APP_H

#include <stdbool.h>

/* start the travel UI app: replaces the demo menu at boot; returns 0 ok */
int travel_app_start(void);
/* stop all worker tasks/timers and delete pages (cooperative teardown) */
void travel_app_stop(void);
/* expose state so pages/workers can coordinate */
bool travel_app_active(void);

#endif /* TRAVEL_APP_H */
