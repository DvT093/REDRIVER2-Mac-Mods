#include "driver2.h"
#include "debris.h"
#include "camera.h"
#include "models.h"
#include "draw.h"
#include "sound.h"
#include "gamesnd.h"
#include "cell.h"

#include <stdio.h>

// REDRIVER2 mac mod: lamp posts that fall over.
//
// When a car hits a lamp post hard enough (bcollide.c), the static object is removed
// from the map (same trick as damage_object) and a copy becomes a "falling lamp":
// it rotates around its base in the direction of the impact, accelerates like a
// falling pole (gravity * sin(tilt)), bounces once when it hits the ground and then
// lies there.

extern MATRIX2 matrixtable[64];
extern MATRIX inv_camera_matrix;
extern void Apply_Inv_CameraMatrix(VECTOR* v);
extern void SetFrustrumMatrix(void);
extern int FrustrumCheck(VECTOR* pos, int bounding_sphere);
extern int gModTestMode;

#define MAX_FALLING_LAMPS	24
#define LAMP_REST_ANGLE		1010		// ~89 degrees (4096 = full circle)
#define LAMP_GRAVITY		3			// angular acceleration factor

struct FALLING_LAMP
{
	int active;
	CELL_OBJECT cop;
	int tilt;			// 0 = upright, 1024 = lying flat (4096 = 360 deg)
	int tiltVel;		// per frame, *16 fixed point
	int fallYaw;		// direction it falls to
	int bounced;
	int resting;
};

static FALLING_LAMP g_lamps[MAX_FALLING_LAMPS];
static int g_nextLamp = 0;

int Lamp_IsFalling(void)
{
	int i;
	for (i = 0; i < MAX_FALLING_LAMPS; i++)
		if (g_lamps[i].active && !g_lamps[i].resting)
			return 1;
	return 0;
}

// cop: unpacked object, ppco: its packed entry in the map (to remove it)
// dirX/dirZ: impact direction (e.g. car velocity), speed: impact strength 0..4096+
int Lamp_KnockOver(CELL_OBJECT* cop, PACKED_CELL_OBJECT* ppco, int dirX, int dirZ, int speed)
{
	FALLING_LAMP* lamp = &g_lamps[g_nextLamp];

	g_nextLamp = (g_nextLamp + 1) % MAX_FALLING_LAMPS;

	lamp->active = 1;
	lamp->cop = *cop;
	lamp->tilt = 0;
	lamp->fallYaw = ratan2(dirX, dirZ);
	lamp->bounced = 0;
	lamp->resting = 0;

	// harder hit = faster start (but always falls)
	lamp->tiltVel = 16 * 6 + speed / 64;
	if (lamp->tiltVel > 16 * 60)
		lamp->tiltVel = 16 * 60;

	// remove the static lamp from the map (like damage_object)
	if (ppco)
	{
		ppco->value = 0xffff;
		ppco->pos.vy = 1;
	}

	cop->pos.vx = OBJECT_SMASHED_MARK;

	if (gModTestMode)
		printf("LAMP knocked over type=%d at %d,%d yaw=%d vel=%d\n", lamp->cop.type, lamp->cop.pos.vx, lamp->cop.pos.vz, lamp->fallYaw, lamp->tiltVel);

	return 1;
}

void Lamp_Update(void)
{
	int i;

	for (i = 0; i < MAX_FALLING_LAMPS; i++)
	{
		FALLING_LAMP* lamp = &g_lamps[i];

		if (!lamp->active || lamp->resting)
			continue;

		// falling pole: angular acceleration grows with sin(tilt)
		lamp->tiltVel += 16 + LAMP_GRAVITY * RSIN(lamp->tilt) / 256;
		lamp->tilt += lamp->tiltVel / 16;

		if (lamp->tilt >= LAMP_REST_ANGLE)
		{
			lamp->tilt = LAMP_REST_ANGLE;

			if (!lamp->bounced && lamp->tiltVel > 16 * 20)
			{
				// one bounce up off the ground, then settle
				VECTOR pos, vel;

				lamp->tiltVel = -lamp->tiltVel / 5;
				lamp->bounced = 1;

				// sparks + clatter where the top hits the ground
				pos.vx = lamp->cop.pos.vx + FIXEDH(RSIN(lamp->fallYaw) * 700);
				pos.vy = lamp->cop.pos.vy - 20;
				pos.vz = lamp->cop.pos.vz + FIXEDH(RCOS(lamp->fallYaw) * 700);
				vel.vx = 0; vel.vy = -20; vel.vz = 0;
				Setup_Sparks(&pos, &vel, 12, 0);

				Start3DSoundVolPitch(-1, SOUND_BANK_SFX, 2, pos.vx, -pos.vy, pos.vz, -1500, 2600);
			}
			else
			{
				lamp->tiltVel = 0;
				lamp->resting = 1;
			}
		}
		else if (lamp->tilt < 0)
		{
			lamp->tilt = 0;
		}
	}
}

void Lamp_Draw(void)
{
	int i;

	for (i = 0; i < MAX_FALLING_LAMPS; i++)
	{
		FALLING_LAMP* lamp = &g_lamps[i];
		MODEL* model;
		MATRIX ry1, rx, ry2, objm, tmp, world, cam;
		VECTOR pos, wpos;

		if (!lamp->active)
			continue;

		model = modelpointers[lamp->cop.type];

		if (!model)
			continue;

		// world rotation = Ry(fall) * Rx(tilt) * Ry(-fall) * object rotation
		InitMatrix(ry1); RotMatrixY(lamp->fallYaw & 0xfff, &ry1);
		InitMatrix(rx);  RotMatrixX(lamp->tilt & 0xfff, &rx);
		InitMatrix(ry2); RotMatrixY(-lamp->fallYaw & 0xfff, &ry2);

		InitMatrix(objm);
		{
			MATRIX2* m2 = &matrixtable[lamp->cop.yang & 63];
			int r, c;
			for (r = 0; r < 3; r++)
				for (c = 0; c < 3; c++)
					objm.m[r][c] = m2->m[r][c];
		}

		MulMatrix0(&ry1, &rx, &tmp);
		MulMatrix0(&tmp, &ry2, &world);
		MulMatrix0(&world, &objm, &tmp);
		MulMatrix0(&inv_camera_matrix, &tmp, &cam);

		wpos.vx = lamp->cop.pos.vx;
		wpos.vy = lamp->cop.pos.vy;
		wpos.vz = lamp->cop.pos.vz;

		SetFrustrumMatrix();

		if (FrustrumCheck(&wpos, model->bounding_sphere) == -1)
			continue;

		pos.vx = wpos.vx - camera_position.vx;
		pos.vy = wpos.vy - camera_position.vy;
		pos.vz = wpos.vz - camera_position.vz;

		Apply_Inv_CameraMatrix(&pos);

		gte_SetRotMatrix(&cam);
		gte_SetTransVector(&pos);

		PlotMDL_less_than_128(model);
	}
}
