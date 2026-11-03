#ifndef ROUTE_H
#define ROUTE_H
#include <stddef.h>
#define MAX_WAYPOINTS 256
typedef struct { double longitude, latitude, altitude, airspeed; } Waypoint;
typedef struct { Waypoint points[MAX_WAYPOINTS]; size_t count; } Route;
/* Reads the documented JSON schema; errors include the approximate byte offset. */
int load_route(const char *filename, Route *route);
#endif
