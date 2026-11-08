#include "driver2.h"
#include "players.h"
#include "pedest.h"
#include "cars.h"
#include "mission.h"
#include "system.h"
#include "pad.h"

#include <stdio.h>
#include <stdlib.h>

// REDRIVER2 mac mod: automated visual test.
// REDRIVER2 -modtest : Take a Ride (Chicago), steps out of the car, draws the pistol,
// shoots and turns, saving screenshots modtest_<n>.bmp, then quits.

int gModTestMode = 0;
int gModTestForward = 0;
int gModTestCarPad = 0;
int gModTestSteps = 0;
extern int gSprintTest;

extern int gPistolDrawn;
extern void PsyX_TakeScreenshot();
extern void Pistol_TestFire(void);
extern SVECTOR gAimRot[3];



static short sweep[][3] = {
	{ 0, -950, 650 }, { 0, -950, 700 }, { 0, -1000, 650 }, { 0, -1000, 700 }, { 0, -1000, 750 }, { 0, -980, 720 },
};

static void Shot(const char* name)
{
	PsyX_TakeScreenshot();
	remove(name);
	rename("SCREENSHOT.BMP", name);
	printf("MODTEST screenshot %s\n", name);
	fflush(stdout);
}

void ModTest_Frame(void)
{
	static int f = 0;

	if (!gModTestMode)
		return;

	f++;

	// gModTestMode 6 = traffic test: count cars over time, screenshot
	if (gModTestMode == 6)
	{
		extern int numCivCars, maxCivCars;

		if (f == 1600)
			Shot("modtest_traffic_busy.bmp");

		if (f % 100 == 0)
		{
			int i, active = 0, nearby = 0;
			for (i = 0; i < MAX_CARS; i++)
				if (car_data[i].controlType != 0)
				{
					int dx = car_data[i].hd.where.t[0] - player[0].pos[0];
					int dz = car_data[i].hd.where.t[2] - player[0].pos[2];
					active++;
					if (ABS(dx) < 15000 && ABS(dz) < 15000 && dx * dx + dz * dz < 15000 * 15000)
						nearby++;
				}
			{
				extern int num_pedestrians;
				extern int Traffic_IsRush(void);
				printf("MODTEST traffic f=%d civ=%d max=%d active=%d near75m=%d peds=%d rush=%d spd=%d\n", f, numCivCars, maxCivCars, active, nearby, num_pedestrians, Traffic_IsRush(), player[0].playerCarId >= 0 ? car_data[player[0].playerCarId].hd.speed : -1);
			}
			fflush(stdout);
		}
		if (f == 1610)
			exit(0);
		return;
	}

	// gModTestMode 5 = GPS test: waypoint on this city's secret car, print the route
	if (gModTestMode == 5)
	{
		extern int gWaypointSet, gWaypointLevel;
		extern VECTOR gWaypointPos;
		extern VECTOR gGpsRoutePts[];
		extern int gGpsRouteCount;
		extern void GpsRoute_Update(void);
		static const int sc[4][2] = { { -207616, 659706 }, { -487936, -136689 }, { 123936, 4928 }, { -40432, 383328 } };

		if (f == 100)
		{
			gWaypointPos.vx = sc[GameLevel][0]; gWaypointPos.vy = 0; gWaypointPos.vz = sc[GameLevel][1];
			gWaypointSet = 1;
			gWaypointLevel = GameLevel;
			GpsRoute_Update();
			printf("MODTEST gps city=%d points=%d", GameLevel, gGpsRouteCount);
			if (gGpsRouteCount > 1)
				printf(" first=(%d,%d) beforelast=(%d,%d) last=(%d,%d)", gGpsRoutePts[0].vx, gGpsRoutePts[0].vz,
					gGpsRoutePts[gGpsRouteCount - 2].vx, gGpsRoutePts[gGpsRouteCount - 2].vz,
					gGpsRoutePts[gGpsRouteCount - 1].vx, gGpsRoutePts[gGpsRouteCount - 1].vz);
			printf("\n");
			fflush(stdout);
			exit(0);
		}
		return;
	}

	// gModTestMode 4 = car exit test: gas + handbrake while moving must NOT exit; Enter must
	if (gModTestMode == 4)
	{
		if (f == 60)
			gModTestCarPad = MPAD_CROSS;							// gas
		if (f == 120)
			gModTestCarPad = MPAD_CROSS | MPAD_D_UP | MPAD_TRIANGLE;	// gas + handbrake (keyboard Up + Space)
		if (f == 160)
		{
			printf("MODTEST exit gas+handbrake: playerType=%d (1 = still in car)\n", player[0].playerType);
			gModTestCarPad = MPAD_CROSS;
		}
		if (f == 170)
			gModTestCarPad = MPAD_R3;								// Enter
		if (f == 172)
			gModTestCarPad = 0;
		if (f == 200)
		{
			printf("MODTEST exit enter: playerType=%d (2 = on foot)\n", player[0].playerType);
			fflush(stdout);
			exit(0);
		}
		return;
	}

	// gModTestMode 3 = sprint test
	if (gModTestMode == 3)
	{
		static int x0, z0;

		if (f == 60 && player[0].playerType == 1 && player[0].playerCarId >= 0)
			ActivatePlayerPedestrian(&car_data[player[0].playerCarId], NULL, 0, NULL, TANNER_MODEL);

		if (f == 150 && player[0].pPed)
			player[0].pPed->dir.vy = (player[0].pPed->dir.vy + 2048) & 0xfff;	// face away from the car

		if (f == 160) { gModTestForward = 1; x0 = player[0].pos[0]; z0 = player[0].pos[2]; gModTestSteps = 0; }
		if (f == 220)
		{
			int dx = player[0].pos[0] - x0, dz = player[0].pos[2] - z0;
			printf("MODTEST run   60 frames: %d units, %d footsteps\n", SquareRoot0(dx * dx + dz * dz), gModTestSteps);
			gModTestSteps = 0;
			Shot("modtest_7_run.bmp");
			gSprintTest = 1; x0 = player[0].pos[0]; z0 = player[0].pos[2];
		}
		if (f == 280)
		{
			int dx = player[0].pos[0] - x0, dz = player[0].pos[2] - z0;
			printf("MODTEST sprint 60 frames: %d units, %d footsteps\n", SquareRoot0(dx * dx + dz * dz), gModTestSteps);
			gModTestSteps = 0;
			Shot("modtest_8_sprint.bmp");
			gSprintTest = 0; gModTestForward = 0;
		}
		if (f == 300) { gModTestForward = 2; x0 = player[0].pos[0]; z0 = player[0].pos[2]; }	// walk back, no pistol
		if (f == 340)
		{
			gModTestSteps = 0;
			int dx = player[0].pos[0] - x0, dz = player[0].pos[2] - z0;
			printf("MODTEST back walk 40: %d units\n", SquareRoot0(dx * dx + dz * dz));
			gPistolDrawn = 1; x0 = player[0].pos[0]; z0 = player[0].pos[2];
		}
		if (f == 362) Shot("modtest_9_backpistol.bmp");
		if (f == 380)
		{
			int dx = player[0].pos[0] - x0, dz = player[0].pos[2] - z0;
			printf("MODTEST back pistol 40: %d units\n", SquareRoot0(dx * dx + dz * dz));
			gSprintTest = 1; x0 = player[0].pos[0]; z0 = player[0].pos[2];
		}
		if (f == 402) Shot("modtest_10_backsprint.bmp");
		if (f == 420)
		{
			int dx = player[0].pos[0] - x0, dz = player[0].pos[2] - z0;
			printf("MODTEST back sprint 40: %d units, %d footsteps (back pistol+sprint 80 frames)\n", SquareRoot0(dx * dx + dz * dz), gModTestSteps);
			fflush(stdout);
			exit(0);
		}
		return;
	}

	// gModTestMode 2 = aim sweep only
	if (gModTestMode == 2)
	{
		int n = sizeof(sweep) / sizeof(sweep[0]);
		int k = (f - 170) / 10;

		if (f == 60 && player[0].playerType == 1 && player[0].playerCarId >= 0)
			ActivatePlayerPedestrian(&car_data[player[0].playerCarId], NULL, 0, NULL, TANNER_MODEL);
		if (f == 160)
			gPistolDrawn = 1;
		if (f >= 170 && k < n && (f - 170) % 10 == 0)
		{
			gAimRot[0].vx = sweep[k][0]; gAimRot[0].vy = sweep[k][1]; gAimRot[0].vz = sweep[k][2];
		}
		if (f == 170 + n * 10)
			exit(0);
		return;
	}

	if (f == 60 && player[0].playerType == 1 && player[0].playerCarId >= 0)
		ActivatePlayerPedestrian(&car_data[player[0].playerCarId], NULL, 0, NULL, TANNER_MODEL);

	if (f == 150)
		Shot("modtest_1_onfoot.bmp");

	if (f == 150 && getenv("REDRIVER2_VIEW_CELLS"))
	{
		char name[64];
		sprintf(name, "modtest_view_%s.bmp", getenv("REDRIVER2_VIEW_CELLS"));
		Shot(name);
		exit(0);
	}

	if (f == 160)
		gPistolDrawn = 1;

	if (f == 175)
		Shot("modtest_2_drawn.bmp");

	if (f == 180)
		Pistol_TestFire();

	if (f == 181)
		Shot("modtest_3_fire.bmp");

	if (f == 182)
		Shot("modtest_3b_fire.bmp");

	if (f == 190 && player[0].pPed)
		player[0].pPed->dir.vy = (player[0].pPed->dir.vy + 1024) & 0xfff;

	if (f == 205)
		Shot("modtest_4_turned.bmp");

	if (f == 210)
		Pistol_TestFire();

	if (f == 211)
		Shot("modtest_5_turnedfire.bmp");

	// shoot the car next to Tanner 7 times, then look at the fire
	if (f >= 220 && f <= 340 && (f - 220) % 20 == 0)
		Pistol_TestFire();

	if (f == 400)
		Shot("modtest_6_carburning.bmp");

	// holster and watch the felony drain
	if (f == 405)
		gPistolDrawn = 0;

	if (f >= 400 && f <= 560 && f % 20 == 0)
	{
		extern short pedestrianFelony;
		extern int CopsCanSeePlayer;
		printf("MODTEST felony f=%d drawn=%d seen=%d felony=%d\n", f, gPistolDrawn, CopsCanSeePlayer, pedestrianFelony);
		fflush(stdout);
	}

	if (f == 570)
		exit(0);
}
