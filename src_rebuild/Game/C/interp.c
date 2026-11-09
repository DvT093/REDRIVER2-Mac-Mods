// [mod] 60 fps: the game still simulates at 30 Hz, but draws extra frames in between.
// Each frame shows the world between the previous and the current sim step (camera, cars, peds lerped).
// gInterpFrame = 1 on every draw that is not the first after a sim step: effects that advance
// state while drawing (smoke, rain, debris, flashes) are frozen there (see FX_FROZEN in pause.h).
#include "driver2.h"
#include "cars.h"
#include "pedest.h"
#include "camera.h"
#include "draw.h"
#include "pause.h"
#include "players.h"
#include "mission.h"
#include "director.h"
#include <stdlib.h>

extern PEDESTRIAN pedestrians[MAX_PEDESTRIANS];

int gInterpFrame = 0;
int gRenderCnt = 0;

typedef struct
{
	MATRIX where;
	MATRIX drawCarMat;
	VECTOR_NOPAD boxLoc;
	short frontRot, backRot;
	char valid;
} CAR_SNAP;

typedef struct
{
	VECTOR_NOPAD pos;
	SVECTOR dir;
	char valid;
} PED_SNAP;

typedef struct
{
	VECTOR pos;
	SVECTOR ang;
	MATRIX inv, cam, face;
	VECTOR playerCam;
	char valid;
} CAM_SNAP;

static CAR_SNAP prevCar[MAX_CARS], saveCar[MAX_CARS];
static PED_SNAP prevPed[MAX_PEDESTRIANS], savePed[MAX_PEDESTRIANS];
static CAM_SNAP prevCam, curCam, saveCam;
static int applied = 0;

#define CAR_JUMP	3000	// more than this per step = teleport/respawn: no lerp
#define PED_JUMP	1000
#define CAM_JUMP	3000

static int lerpI(int a, int b, int t) { return a + (int)(((long long)(b - a) * t) >> 12); }
static short lerpAng(int a, int b, int t) { int d = ((b - a + 2048) & 4095) - 2048; return (short)(a + ((d * t) >> 12)); }

static void lerpMatrix(MATRIX* out, const MATRIX* a, const MATRIX* b, int t, int withPos)
{
	int i, j;
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			out->m[i][j] = (short)lerpI(a->m[i][j], b->m[i][j], t);
	if (withPos)
		for (i = 0; i < 3; i++)
			out->t[i] = lerpI(a->t[i], b->t[i], t);
}

static int farApart(int ax, int ay, int az, int bx, int by, int bz, int limit)
{
	return ABS(ax - bx) > limit || ABS(ay - by) > limit || ABS(az - bz) > limit;
}

static void SnapCam(CAM_SNAP* c)
{
	c->pos = camera_position;
	c->ang = camera_angle;
	c->inv = inv_camera_matrix;
	c->cam = camera_matrix;
	c->face = face_camera;
	c->playerCam = player[0].cameraPos;
	c->valid = 1;
}

static void LoadCam(const CAM_SNAP* c)
{
	camera_position = c->pos;
	camera_angle = c->ang;
	inv_camera_matrix = c->inv;
	camera_matrix = c->cam;
	face_camera = c->face;
	player[0].cameraPos = c->playerCam;
}

// 60 fps on? Single player only; off in split screen.
int Interp_Active(void)
{
	static int env = -1;
	if (env < 0)
	{
		const char* e = getenv("REDRIVER2_60FPS");	// [mod] REDRIVER2_60FPS=0 = original 30 fps
		env = (e && e[0] == '0') ? 0 : 1;
	}
	return env && NumPlayers == 1;
}

// may we draw extra frames right now (no menus/map/fast forward)?
int Interp_ExtraFramesAllowed(void)
{
	return !pauseflag && !gShowMap && !FastForward;
}

// before StepGame: current state becomes "previous"
void Interp_BeforeStep(void)
{
	int i;
	LPPEDESTRIAN p;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		prevCar[i].valid = cp->controlType != 0;
		if (!prevCar[i].valid)
			continue;
		prevCar[i].where = cp->hd.where;
		prevCar[i].drawCarMat = cp->hd.drawCarMat;
		prevCar[i].boxLoc = cp->hd.oBox.location;
		prevCar[i].frontRot = FrontWheelRotation[i];
		prevCar[i].backRot = BackWheelRotation[i];
	}

	for (i = 0; i < MAX_PEDESTRIANS; i++)
		prevPed[i].valid = 0;

	for (p = pUsedPeds; p; p = p->pNext)
	{
		i = p - pedestrians;
		if (i < 0 || i >= MAX_PEDESTRIANS)
			continue;
		prevPed[i].pos = p->position;
		prevPed[i].dir = p->dir;
		prevPed[i].valid = 1;
	}

	prevCam = curCam;
}

// after StepGame + InitCamera: remember the camera this step produced
void Interp_AfterCamera(void)
{
	SnapCam(&curCam);
	if (!prevCam.valid)
		prevCam = curCam;
}

// draw state = lerp(previous, current, t) with t in 0..4096
void Interp_Apply(int t)
{
	int i;
	LPPEDESTRIAN p;

	applied = 0;
	if (t >= 4096 || t < 0)
		return;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		CAR_SNAP* a = &prevCar[i];

		saveCar[i].valid = 0;
		if (!a->valid || cp->controlType == 0)
			continue;
		if (farApart(a->where.t[0], a->where.t[1], a->where.t[2], cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2], CAR_JUMP))
			continue;

		saveCar[i].where = cp->hd.where;
		saveCar[i].drawCarMat = cp->hd.drawCarMat;
		saveCar[i].boxLoc = cp->hd.oBox.location;
		saveCar[i].frontRot = FrontWheelRotation[i];
		saveCar[i].backRot = BackWheelRotation[i];
		saveCar[i].valid = 1;

		lerpMatrix(&cp->hd.where, &a->where, &saveCar[i].where, t, 1);
		lerpMatrix(&cp->hd.drawCarMat, &a->drawCarMat, &saveCar[i].drawCarMat, t, 1);
		cp->hd.oBox.location.vx = lerpI(a->boxLoc.vx, saveCar[i].boxLoc.vx, t);
		cp->hd.oBox.location.vy = lerpI(a->boxLoc.vy, saveCar[i].boxLoc.vy, t);
		cp->hd.oBox.location.vz = lerpI(a->boxLoc.vz, saveCar[i].boxLoc.vz, t);
		FrontWheelRotation[i] = lerpAng(a->frontRot, saveCar[i].frontRot, t) & 0xfff;
		BackWheelRotation[i] = lerpAng(a->backRot, saveCar[i].backRot, t) & 0xfff;
	}

	for (i = 0; i < MAX_PEDESTRIANS; i++)
		savePed[i].valid = 0;

	for (p = pUsedPeds; p; p = p->pNext)
	{
		PED_SNAP* a;
		i = p - pedestrians;
		if (i < 0 || i >= MAX_PEDESTRIANS)
			continue;
		a = &prevPed[i];
		if (!a->valid || farApart(a->pos.vx, a->pos.vy, a->pos.vz, p->position.vx, p->position.vy, p->position.vz, PED_JUMP))
			continue;

		savePed[i].pos = p->position;
		savePed[i].dir = p->dir;
		savePed[i].valid = 1;

		p->position.vx = lerpI(a->pos.vx, savePed[i].pos.vx, t);
		p->position.vy = lerpI(a->pos.vy, savePed[i].pos.vy, t);
		p->position.vz = lerpI(a->pos.vz, savePed[i].pos.vz, t);
		p->dir.vx = lerpAng(a->dir.vx, savePed[i].dir.vx, t);
		p->dir.vy = lerpAng(a->dir.vy, savePed[i].dir.vy, t);
		p->dir.vz = lerpAng(a->dir.vz, savePed[i].dir.vz, t);
	}

	saveCam.valid = 0;
	if (prevCam.valid && curCam.valid &&
		!farApart(prevCam.pos.vx, prevCam.pos.vy, prevCam.pos.vz, curCam.pos.vx, curCam.pos.vy, curCam.pos.vz, CAM_JUMP))
	{
		CAM_SNAP l;
		SnapCam(&saveCam);

		l.pos.vx = lerpI(prevCam.pos.vx, curCam.pos.vx, t);
		l.pos.vy = lerpI(prevCam.pos.vy, curCam.pos.vy, t);
		l.pos.vz = lerpI(prevCam.pos.vz, curCam.pos.vz, t);
		l.pos.pad = curCam.pos.pad;
		l.ang.vx = lerpAng(prevCam.ang.vx, curCam.ang.vx, t);
		l.ang.vy = lerpAng(prevCam.ang.vy, curCam.ang.vy, t);
		l.ang.vz = lerpAng(prevCam.ang.vz, curCam.ang.vz, t);
		l.ang.pad = curCam.ang.pad;
		lerpMatrix(&l.inv, &prevCam.inv, &curCam.inv, t, 1);
		lerpMatrix(&l.cam, &prevCam.cam, &curCam.cam, t, 1);
		lerpMatrix(&l.face, &prevCam.face, &curCam.face, t, 1);
		l.playerCam.vx = lerpI(prevCam.playerCam.vx, curCam.playerCam.vx, t);
		l.playerCam.vy = lerpI(prevCam.playerCam.vy, curCam.playerCam.vy, t);
		l.playerCam.vz = lerpI(prevCam.playerCam.vz, curCam.playerCam.vz, t);
		l.playerCam.pad = curCam.playerCam.pad;
		LoadCam(&l);
	}

	applied = 1;
}

// put the real (current) state back so the sim never sees lerped values
void Interp_Restore(void)
{
	int i;
	LPPEDESTRIAN p;

	if (!applied)
		return;
	applied = 0;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		if (!saveCar[i].valid)
			continue;
		cp->hd.where = saveCar[i].where;
		cp->hd.drawCarMat = saveCar[i].drawCarMat;
		cp->hd.oBox.location = saveCar[i].boxLoc;
		FrontWheelRotation[i] = saveCar[i].frontRot;
		BackWheelRotation[i] = saveCar[i].backRot;
	}

	for (p = pUsedPeds; p; p = p->pNext)
	{
		i = p - pedestrians;
		if (i < 0 || i >= MAX_PEDESTRIANS || !savePed[i].valid)
			continue;
		p->position = savePed[i].pos;
		p->dir = savePed[i].dir;
	}

	if (saveCam.valid)
		LoadCam(&saveCam);
}
