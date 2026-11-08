#ifndef FARCITY_H
#define FARCITY_H

// REDRIVER2 mac mod: draw-only copy of city regions beyond the 4 loaded ones (see farcity.c)

extern int gFarCity;

extern void FarCity_Reset(void);
extern void FarCity_BeginFrame(void);
extern int FarCity_CellNeedsFar(int cellx, int cellz);
extern PACKED_CELL_OBJECT* FarCity_First(int cellx, int cellz, CELL_ITERATOR* pci, int level);
extern PACKED_CELL_OBJECT* FarCity_Next(CELL_ITERATOR* pci);

#endif
