#ifndef WAYPOINT_H
#define WAYPOINT_H

// GPS waypoint (REDRIVER2 mac mod): set a point on the fullscreen map,
// shown as a blip + arrow on the in-game overhead map.

extern int gWaypointSet;
extern VECTOR gWaypointPos;

extern void Waypoint_FullscreenMapUpdate(void);	// call inside DrawFullscreenMap after offsets are known
extern void Waypoint_DrawFullscreen(void);
extern void Waypoint_DrawOverhead(void);
extern void Waypoint_Clear(void);

#endif
