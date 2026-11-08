#include "driver2.h"
#include "farcity.h"
#include "map.h"
#include "spool.h"
#include "cell.h"
#include "system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// REDRIVER2 mac mod: draw the city beyond the 4 loaded regions.
//
// The game keeps 4 regions (32x32 cells each) of the city in memory, in 4 "barrel" slots
// chosen by even/odd region coordinates. Everything else (collision, roads, AI, PVS) keeps
// using those slots, untouched. This file only adds a draw-only copy of the regions further
// away: when the cell loop in draw.c reaches a cell whose region is not in its slot, the
// objects come from here instead.
//
// Region data is read straight from the level file (same layout as LoadRegionData):
// packed cell pointers, cell lists, cell objects. Two things differ from the slot copy:
// - cell pointers are not offset by cell_slots_add (each region has its own cell array)
// - CELL_DATA.num holds an absolute object index baked for the region's slot:
//   num_straddlers + cell_objects_add[slot] + local. Below num_straddlers it is a
//   straddler (always loaded, global cell_objects).

extern int SpoolLumpOffset;
extern char g_CurrentLevelFileName[64];
extern int citystart[8];
extern int GameLevel;

char g_FarCityLevelName[64];		// level file the game opened (CD path or loose file), set in SetCityType
extern int gModTestMode;

#define FAR_MAX_REGIONS			1024
#define FAR_LOADS_PER_FRAME		6

struct FAR_REGION
{
	int state;					// 0 = not loaded, 1 = loaded, -1 = empty / failed
	u_short ptrs[1024];
	CELL_DATA* cells;
	int numCells;
	PACKED_CELL_OBJECT* objs;
	int numObjs;
	int objBase;
	int* stamp;					// per object: frame it was last drawn (dedupe)
};

int gFarCity = 1;

static FAR_REGION* g_far[FAR_MAX_REGIONS];
static FILE* g_farFile = NULL;
static int g_farFrame = 1;
static int g_farLoadsThisFrame = 0;
static int g_farLoaded = 0;

static FAR_REGION* g_iterRegion = NULL;
static int g_farOpenFailed = 0;

void FarCity_Reset(void)
{
	int i;

	for (i = 0; i < FAR_MAX_REGIONS; i++)
	{
		if (g_far[i])
		{
			free(g_far[i]->cells);
			free(g_far[i]->objs);
			free(g_far[i]->stamp);
			free(g_far[i]);
			g_far[i] = NULL;
		}
	}

	if (g_farFile)
		fclose(g_farFile);

	g_farFile = NULL;
	g_farOpenFailed = 0;
	g_farLoaded = 0;
	g_iterRegion = NULL;
}

void FarCity_BeginFrame(void)
{
	static int envChecked = 0;

	if (!envChecked)
	{
		const char* v = getenv("REDRIVER2_FAR_CITY");
		if (v)
			gFarCity = atoi(v);
		envChecked = 1;
	}

	g_farFrame++;
	g_farLoadsThisFrame = 0;
}

// open the level file: a loose file as the game uses it, or for a CD path
// ("\DRIVER2\LEVELS\CHICAGO.LEV;1") the extracted copy in mod/DRIVER2/LEVELS/
static int OpenLevelFile(void)
{
	char name[128];
	char* p;

	if (g_CurrentLevelFileName[0])
	{
		strcpy(name, g_CurrentLevelFileName);
	}
	else
	{
		const char* src = g_FarCityLevelName;

		while (*src == '\\' || *src == '/')
			src++;

		sprintf(name, "mod/%s", src);

		p = strchr(name, ';');
		if (p)
			*p = 0;
	}

	for (p = name; *p; p++)
		if (*p == '\\')
			*p = '/';

	g_farFile = fopen(name, "rb");

	if (!g_farFile)
	{
		static int warned = 0;
		if (!warned)
			printf("[farcity] could not open level copy '%s', far city off\n", name);
		warned = 1;
		return 0;
	}

	if (gModTestMode)
		printf("FARCITY using %s\n", name);

	return 1;
}

// sector: as the game counts it (CD sectors when reading from the CD image)
static int ReadSectors(char* dest, int sector, int count)
{
	if (!g_farFile && (g_farOpenFailed || !OpenLevelFile()))
	{
		g_farOpenFailed = 1;
		return 0;
	}

	sector -= citystart[GameLevel];		// 0 for a loose file

	if (fseek(g_farFile, (long)sector * CDSECTOR_SIZE, SEEK_SET) != 0)
		return 0;

	return fread(dest, CDSECTOR_SIZE, count, g_farFile) == (size_t)count;
}

static void UnpackPointers(FAR_REGION* fr, char* cell_addr)
{
	int packtype = *(int*)(cell_addr + 4);
	u_short* src = (u_short*)(cell_addr + 8);
	int loop;

	if (packtype == 1)
	{
		for (loop = 0; loop < 1024; loop++)
			fr->ptrs[loop] = *src++;
	}
	else if (packtype == 2)
	{
		u_int bitpos = 0x8000;
		u_int pcode = *src++;

		for (loop = 0; loop < 1024; loop++)
		{
			fr->ptrs[loop] = (pcode & bitpos) ? *src++ : 0xffff;

			bitpos >>= 1;
			if (bitpos == 0)
			{
				bitpos = 0x8000;
				pcode = *src++;
			}
		}
	}
	else
	{
		for (loop = 0; loop < 1024; loop++)
			fr->ptrs[loop] = 0xffff;
	}
}

static FAR_REGION* LoadRegion(int region, int rx, int rz)
{
	FAR_REGION* fr;
	Spool* sp;
	char* buf;
	int sector, total;
	u_short spofs;

	if (region < 0 || region >= FAR_MAX_REGIONS)
		return NULL;

	fr = g_far[region];

	if (fr)
		return fr->state == 1 ? fr : NULL;

	if (g_farLoadsThisFrame >= FAR_LOADS_PER_FRAME)
		return NULL;

	g_farLoadsThisFrame++;

	fr = (FAR_REGION*)calloc(1, sizeof(FAR_REGION));
	g_far[region] = fr;
	fr->state = -1;

	spofs = spoolinfo_offsets[region];

	if (spofs == 0xffff)
	{
		if (gModTestMode)
			printf("FARCITY region %d has no data\n", region);
		return NULL;
	}

	sp = (Spool*)(RegionSpoolInfo + spofs);

	total = sp->cell_data_size[0] + sp->cell_data_size[1] + sp->cell_data_size[2];

	if (total <= 0)
		return NULL;

	buf = (char*)malloc(total * CDSECTOR_SIZE);
	sector = SpoolLumpOffset / CDSECTOR_SIZE + sp->offset;

	if (!ReadSectors(buf, sector, total))
	{
		if (gModTestMode)
			printf("FARCITY read failed region %d file '%s' sector %d count %d\n", region, g_CurrentLevelFileName, sector, total);
		free(buf);
		return NULL;
	}

	// same order as LoadRegionData: [1] packed cell pointers, [0] cells, [2] cell objects
	UnpackPointers(fr, buf);

	fr->numCells = sp->cell_data_size[0] * CDSECTOR_SIZE / sizeof(CELL_DATA);
	fr->cells = (CELL_DATA*)malloc(sp->cell_data_size[0] * CDSECTOR_SIZE);
	memcpy(fr->cells, buf + sp->cell_data_size[1] * CDSECTOR_SIZE, sp->cell_data_size[0] * CDSECTOR_SIZE);

	fr->numObjs = sp->cell_data_size[2] * CDSECTOR_SIZE / sizeof(PACKED_CELL_OBJECT);
	fr->objs = (PACKED_CELL_OBJECT*)malloc(sp->cell_data_size[2] * CDSECTOR_SIZE);
	memcpy(fr->objs, buf + (sp->cell_data_size[1] + sp->cell_data_size[0]) * CDSECTOR_SIZE, sp->cell_data_size[2] * CDSECTOR_SIZE);

	fr->stamp = (int*)calloc(fr->numObjs > 0 ? fr->numObjs : 1, sizeof(int));
	fr->objBase = num_straddlers + cell_objects_add[(rx & 1) + (rz & 1) * 2];

	free(buf);

	fr->state = 1;
	g_farLoaded++;

	if (gModTestMode)
		printf("FARCITY loaded region %d (%d,%d) cells=%d objs=%d total=%d\n", region, rx, rz, fr->numCells, fr->numObjs, g_farLoaded);

	return fr;
}

int FarCity_CellNeedsFar(int cellx, int cellz)
{
	int rx, rz, slot;

	if (!gFarCity)
		return 0;

	rx = cellx / MAP_REGION_SIZE;
	rz = cellz / MAP_REGION_SIZE;
	slot = (rx & 1) + (rz & 1) * 2;

	return loading_region[slot] != -1 || RoadMapRegions[slot] != rx + rz * regions_across;
}

// object index from a cell list entry; NULL when it should be skipped
static PACKED_CELL_OBJECT* ResolveObject(u_short num)
{
	PACKED_CELL_OBJECT* ppco;
	int n = num & 16383;

	if (n < num_straddlers)
	{
		u_int bit = 1 << (n & 7);

		if (cell_object_computed_values[n / 8] & bit)
			return NULL;

		cell_object_computed_values[n / 8] |= bit;
		ppco = &cell_objects[n];
	}
	else
	{
		int local = n - g_iterRegion->objBase;

		if (local < 0 || local >= g_iterRegion->numObjs)
			return NULL;

		if (g_iterRegion->stamp[local] == g_farFrame)
			return NULL;

		g_iterRegion->stamp[local] = g_farFrame;
		ppco = &g_iterRegion->objs[local];
	}

	if (ppco->value == 0xffff && (ppco->pos.vy & 1))
		return NULL;

	return ppco;
}

PACKED_CELL_OBJECT* FarCity_Next(CELL_ITERATOR* pci)
{
	CELL_DATA* celld = pci->pcd;
	CELL_DATA* end = g_iterRegion->cells + g_iterRegion->numCells;
	PACKED_CELL_OBJECT* ppco;

	do {
		if (celld->num & 0x8000)	// end of cell objects
			return NULL;

		celld++;

		if (celld >= end || (celld->num & 0x4000))	// end of list
			return NULL;

		ppco = ResolveObject(celld->num);
	} while (!ppco);

	pci->pcd = celld;
	pci->ppco = ppco;

	return ppco;
}

PACKED_CELL_OBJECT* FarCity_First(int cellx, int cellz, CELL_ITERATOR* pci, int level)
{
	FAR_REGION* fr;
	CELL_DATA* cell;
	CELL_DATA* end;
	PACKED_CELL_OBJECT* ppco;
	int rx, rz;
	u_short ptr;

	rx = cellx / MAP_REGION_SIZE;
	rz = cellz / MAP_REGION_SIZE;

	fr = LoadRegion(rx + rz * regions_across, rx, rz);

	if (!fr)
		return NULL;

	ptr = fr->ptrs[(cellz % MAP_REGION_SIZE) * MAP_REGION_SIZE + (cellx % MAP_REGION_SIZE)];

	if (ptr == 0xffff || ptr >= fr->numCells)
		return NULL;

	cell = &fr->cells[ptr];
	end = fr->cells + fr->numCells;

	if (level == -1)
	{
		if (cell->num & 0x4000)
			return NULL;
	}
	else
	{
		level |= 0x4000;
		while (cell->num != level)
		{
			cell++;
			if (cell >= end || (cell->num & 0x8000))
				return NULL;
		}
		cell++;
		if (cell >= end)
			return NULL;
	}

	if (cell->num & 0x4000)	// empty list
		return NULL;

	g_iterRegion = fr;

	pci->nearCell.x = (cellx - (cells_across / 2)) * MAP_CELL_SIZE;
	pci->nearCell.z = (cellz - (cells_down / 2)) * MAP_CELL_SIZE;
	pci->use_computed = 1;
	pci->pcd = cell;

	ppco = ResolveObject(cell->num);

	if (!ppco)
		ppco = FarCity_Next(pci);

	pci->ppco = ppco;

	return ppco;
}
