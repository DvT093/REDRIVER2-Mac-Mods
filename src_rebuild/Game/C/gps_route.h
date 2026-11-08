#ifndef GPS_ROUTE_H
#define GPS_ROUTE_H

#define GPS_MAX_POINTS 1024

extern VECTOR gGpsRoutePts[GPS_MAX_POINTS];
extern int gGpsRouteCount;

extern void GpsRoute_Update(void);
extern void GpsRoute_Clear(void);

#endif
