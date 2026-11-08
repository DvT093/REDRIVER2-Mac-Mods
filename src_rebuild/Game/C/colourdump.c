#include "driver2.h"
#include "cars.h"
#include "players.h"
#include "mission.h"
#include "system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// REDRIVER2 mac mod: one-off tool to extract car colour names.
// Run: REDRIVER2 -dumpcolours <city 0-3> <carSelection 0-9>
// Loads Take a Ride with that car, waits until textures are in VRAM,
// prints "COLOURS <city> <sel> <model> name|name|..." and quits.

int gColourDumpMode = 0;
int gColourDumpSel = 0;

extern unsigned short vram[1024 * 512];

static void ClutColour(int clutId, int idx, int* r, int* g, int* b)
{
	int x = (clutId & 0x3f) * 16;
	int y = (clutId >> 6) & 0x1ff;
	unsigned short c = vram[y * 1024 + x + idx];

	*r = (c & 31) * 255 / 31;
	*g = ((c >> 5) & 31) * 255 / 31;
	*b = ((c >> 10) & 31) * 255 / 31;
}

const char* ColourName(int r, int g, int b)
{
	int mx = MAX(r, MAX(g, b));
	int mn = MIN(r, MIN(g, b));
	int sat = mx ? (mx - mn) * 255 / mx : 0;
	int h = 0;

	if (mx < 45)
		return "Black";

	if (sat < 45)
	{
		if (mx > 200) return "White";
		if (mx > 140) return "Silver";
		return "Grey";
	}

	// hue 0..359
	if (mx == r)      h = (60 * (g - b) / (mx - mn) + 360) % 360;
	else if (mx == g) h = 60 * (b - r) / (mx - mn) + 120;
	else              h = 60 * (r - g) / (mx - mn) + 240;

	if (h < 15 || h >= 340)
		return (mx < 120) ? "Maroon" : "Red";
	if (h < 40)
		return (mx < 140) ? "Brown" : "Orange";
	if (h < 70)
		return (mx < 140) ? "Olive" : "Yellow";
	if (h < 160)
		return (mx < 110) ? "Dark green" : "Green";
	if (h < 200)
		return "Turquoise";
	if (h < 255)
		return (mx < 110) ? "Navy" : "Blue";
	if (h < 290)
		return "Purple";
	return "Pink";
}

void ColourDump_Frame(void)
{
	static int frames = 0;
	CAR_DATA* cp;
	CAR_MODEL* car;
	u_short* pciv = (u_short*)&civ_clut[1];
	int offsets[64];
	int numOffsets = 0;
	int i, p, k;
	int palValid[6];
	long long sumR[6] = { 0 }, sumG[6] = { 0 }, sumB[6] = { 0 }, cnt[6] = { 0 };

	if (!gColourDumpMode)
		return;

	if (++frames < 60)
		return;

	if (player[0].playerCarId < 0)
	{
		printf("COLOURS %d %d -1 ERROR_NO_CAR\n", GameLevel, gColourDumpSel);
		exit(0);
	}

	cp = &car_data[player[0].playerCarId];
	car = &NewCarModel[cp->ap.model];

	// collect the unique clut offsets used by this car's textured polys
	for (i = 0; i < car->numGT3 && numOffsets < 64; i++)
	{
		int off = car->pGT3[i].clut_uv0 >> 16;

		for (k = 0; k < numOffsets; k++)
			if (offsets[k] == off)
				break;

		if (k == numOffsets)
			offsets[numOffsets++] = off;
	}

	for (p = 0; p < 6; p++)
		palValid[p] = 0;

	for (k = 0; k < numOffsets; k++)
	{
		int base = pciv[offsets[k]];
		int idx;

		if (base == 0)
			continue;

		for (p = 0; p < 6; p++)
			if (pciv[offsets[k] + p] != 0)
				palValid[p] = 1;

		// paint = colour indices that differ between palettes
		for (idx = 0; idx < 16; idx++)
		{
			int r0, g0, b0, varies = 0;

			ClutColour(base, idx, &r0, &g0, &b0);

			for (p = 1; p < 6; p++)
			{
				int c = pciv[offsets[k] + p];
				int r, g, b;

				if (c == 0)
					continue;

				ClutColour(c, idx, &r, &g, &b);

				if (ABS(r - r0) + ABS(g - g0) + ABS(b - b0) > 24)
					varies = 1;
			}

			if (!varies)
				continue;

			for (p = 0; p < 6; p++)
			{
				int c = pciv[offsets[k] + p];
				int r, g, b;

				if (c == 0)
					continue;

				ClutColour(c, idx, &r, &g, &b);

				// skip near-black shading entries, they say little about the paint
				if (MAX(r, MAX(g, b)) < 24)
					continue;

				sumR[p] += r; sumG[p] += g; sumB[p] += b; cnt[p]++;
			}
		}
	}

	printf("COLOURS %d %d %d pal=%d ", GameLevel, gColourDumpSel, cp->ap.model, cp->ap.palette);

	for (p = 0; p < 6; p++)
	{
		if (!palValid[p] || cnt[p] == 0)
		{
			printf("%s-", p ? "|" : "");
			continue;
		}

		{
			int r = (int)(sumR[p] / cnt[p]);
			int g = (int)(sumG[p] / cnt[p]);
			int b = (int)(sumB[p] / cnt[p]);

			printf("%s%s(%d,%d,%d)", p ? "|" : "", ColourName(r, g, b), r, g, b);
		}
	}

	printf("\n");
	fflush(stdout);
	exit(0);
}
