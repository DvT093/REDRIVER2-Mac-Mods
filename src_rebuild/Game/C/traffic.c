#include "driver2.h"
#include "mission.h"
#include "camera.h"
#include "system.h"

#include <stdlib.h>
#include <math.h>

// REDRIVER2 mac mod: dynamic traffic in Take a Ride.
// maxCivCars (all civ cars incl. parked) slowly swings between quiet and busy, with an
// occasional "rush hour" burst that fills the junctions so jams form at the lights.
// MAX_CARS is 60: 60 - player - 4 cops leaves room for ~54 civ cars.

extern int maxCivCars, maxParkedCars, maxCopCars;
extern int numCivCars;

#define TRAFFIC_MIN		48
#define TRAFFIC_MAX		110		// MAX_CARS (120) - 1 player - cops - spares
#define TRAFFIC_PERIOD	(30 * 150)	// one quiet->busy->quiet wave ~2.5 min

static int g_rushFrames = 0;
static int g_nextRushCheck = 0;

void Traffic_Update(void)
{
	static int forced = -2;
	int target, wave;

	if (GameType != GAME_TAKEADRIVE || NumPlayers != 1)
		return;

	if (forced == -2)
	{
		const char* v = getenv("REDRIVER2_TRAFFIC_TARGET");	// test override
		forced = v ? atoi(v) : -1;
	}

	if (forced > 0)
	{
		maxCivCars = forced;
		return;
	}

	// slow wave 0..1 over the period
	wave = (int)((1.0 - cos(CameraCnt * 6.2831853 / TRAFFIC_PERIOD)) * 0.5 * 1000.0);
	target = TRAFFIC_MIN + (TRAFFIC_MAX - 8 - TRAFFIC_MIN) * wave / 1000;

	// every ~40 s: small chance of a rush hour burst lasting 30-60 s
	if (CameraCnt >= g_nextRushCheck)
	{
		g_nextRushCheck = CameraCnt + 30 * 40;

		if (g_rushFrames == 0 && (rand() % 100) < 30)
			g_rushFrames = 30 * (30 + rand() % 30);
	}

	if (g_rushFrames > 0)
	{
		g_rushFrames--;
		target = TRAFFIC_MAX;
	}

	if (target > MAX_CARS - 1 - maxCopCars - 1)
		target = MAX_CARS - 1 - maxCopCars - 1;

	maxCivCars = target;
	maxParkedCars = 28;
}

int Traffic_IsRush(void)
{
	return g_rushFrames > 0;
}
