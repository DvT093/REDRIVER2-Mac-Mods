#include "driver2.h"
#include "players.h"
#include "pedest.h"
#include "cars.h"
#include "bcollide.h"
#include "debris.h"
#include "felony.h"
#include "mission.h"
#include "camera.h"
#include "objcoll.h"
#include "sound.h"
#include "system.h"

#include "../utils/audio_source/snd_al_source.h"
#include "../utils/audio_source/snd_wav_cache.h"
#include "AL/al.h"

#ifndef PSX
#include <SDL.h>
#endif

// REDRIVER2 mac mod: pistol (Take a Ride, on foot only).
// G = draw / holster, F = shoot (hold = autofire). Straight ahead, no auto-aim. Hits cars (damage + dents + sparks) and walls
// (sparks); nearby pedestrians dive away; felony goes up. Muzzle flash glow and
// a gunshot WAV (procedurally generated, game/mod/gunshot.wav).

extern int gMasterVolume;
extern int bAvoidTanner;
extern void SetupCivJump(LPPEDESTRIAN pPed, CAR_DATA* cp);

int gPistolDrawn = 0;
int gCopsHeardShot = 0;	// frames left in which cops 'see' the player because they heard a shot

// right arm aim pose (RSHOULDER, RELBOW, RHAND rotations; 4096 = full circle). Tuned with -modtest.
SVECTOR gAimRot[3] = { { 0, -1000, 650, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };	// measured: ~12 deg down, slightly inward (aims at cars/people)

static int g_flashFrames = 0;
static int g_tracerFrames = 0;
static VECTOR g_tracerEnd;			// world (ped convention: up = negative y)
static VECTOR g_lastMuzzle;			// world, updated every frame the gun is drawn
static int g_haveMuzzle = 0;
static VECTOR g_flashPos;
static int g_cooldown = 0;

static ALuint g_shotSource = 0;
static CSoundSource_OpenALCache* g_shotWave = NULL;
static int g_shotLoaded = 0;

#define PISTOL_RANGE		12000
#define PISTOL_FIRE_INTERVAL	6			// frames between shots while F is held (~5 shots/s)
#define PISTOL_HITS_TO_BURN	6			// hits until the car reaches max damage and catches fire
#define PISTOL_FELONY		400			// per shot (FELONY_PURSUIT_MIN_VALUE = 658)
#define PISTOL_SCARE_RANGE	8000
#define PISTOL_HEAR_RANGE	30000		// ~150 m: cops within this distance hear a shot
#define PISTOL_HEAR_FRAMES	75			// they 'see' you for ~2.5 s after a shot
#define PISTOL_FELONY_DRAIN	32			// per frame once the pistol is away and no cop sees you

static void LoadShotSound(void)
{
	CSoundSource_WaveCache* wav;

	if (g_shotLoaded)
		return;

	g_shotLoaded = 1;

	wav = new CSoundSource_WaveCache();

	if (!wav->Load("mod/gunshot.wav"))
	{
		printf("[pistol] could not load mod/gunshot.wav\n");
		delete wav;
		return;
	}

	g_shotWave = new CSoundSource_OpenALCache(wav);

	alGenSources(1, &g_shotSource);
	alSourcei(g_shotSource, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcei(g_shotSource, AL_BUFFER, g_shotWave->m_alBuffer);
}

static void PlayShotSound(void)
{
	float vol;

	LoadShotSound();

	if (!g_shotSource)
		return;

	vol = (float)((10000 + gMasterVolume) / 79) / 128.0f;

	alSourceStop(g_shotSource);
	alSourcef(g_shotSource, AL_GAIN, vol);
	alSourcePlay(g_shotSource);
}

static int KeyPressed(int scancode, int* prev)
{
#ifndef PSX
	const Uint8* ks = SDL_GetKeyboardState(NULL);
	int down = ks && ks[scancode];
	int pressed = down && !*prev;

	*prev = down;
	return pressed;
#else
	return 0;
#endif
}

// distance along the ray (dx,dz normalised to 4096) to a circle; -1 if missed
static int RayHitCircle(int ox, int oz, int dx, int dz, int cx, int cz, int radius, int range)
{
	long long rx = cx - ox, rz = cz - oz;
	long long t = (rx * dx + rz * dz) / 4096;		// projection on the ray
	long long px, pz, d2;

	if (t < 0 || t > range)
		return -1;

	px = ox + (dx * t) / 4096 - cx;
	pz = oz + (dz * t) / 4096 - cz;
	d2 = px * px + pz * pz;

	if (d2 > (long long)radius * radius)
		return -1;

	// step back from the closest point to where the ray enters the circle
	t -= SquareRoot0((int)((long long)radius * radius - d2));

	return (t < 0) ? 0 : (int)t;
}

// car axes from its heading: forward f = (sin, cos), right r = (cos, -sin)
static void CarLocal(CAR_DATA* cp, int x, int z, int* side, int* fwd)
{
	int d = cp->hd.direction;
	int sn = RSIN(d), cs = RCOS(d);
	int dx = x - cp->hd.where.t[0];
	int dz = z - cp->hd.where.t[2];

	*fwd = FIXEDH(dx * sn + dz * cs);
	*side = FIXEDH(dx * cs - dz * sn);
}

static int CarDamageRegion(CAR_DATA* cp, int hx, int hz)
{
	int lbody = cp->ap.carCos->colBox.vz / 2;
	int door, nose;

	CarLocal(cp, hx, hz, &door, &nose);

	if (door < 1)
		return (nose > lbody) ? 0 : (-lbody < nose) ? 5 : 4;

	return (nose > lbody) ? 1 : (-lbody < nose) ? 2 : 3;
}

// ray (origin o, unit dir d*4096) against the car's oriented rectangle; returns entry distance or -1
static int RayHitCar(CAR_DATA* cp, int ox, int oz, int dx, int dz, int range)
{
	int hw = cp->ap.carCos->colBox.vx;	// half width
	int hl = cp->ap.carCos->colBox.vz;	// half length
	int os, of, ds, df;
	int ex, ez;
	long long tmin = 0, tmax = (long long)range * 4096;

	CarLocal(cp, ox, oz, &os, &of);

	// direction in local axes (still scaled by 4096)
	ex = cp->hd.where.t[0] + dx;	// helper point one "4096 unit" along the ray
	ez = cp->hd.where.t[2] + dz;
	{
		int d = cp->hd.direction;
		int sn = RSIN(d), cs = RCOS(d);
		df = FIXEDH(dx * sn + dz * cs);		// 0..4096
		ds = FIXEDH(dx * cs - dz * sn);
	}
	(void)ex; (void)ez;

	// slab test, t in units where 4096 = 1 world unit along the ray
	{
		int axis;
		for (axis = 0; axis < 2; axis++)
		{
			long long o = axis ? of : os;
			long long dd = axis ? df : ds;
			long long h = axis ? hl : hw;

			if (dd == 0)
			{
				if (o < -h || o > h)
					return -1;
			}
			else
			{
				long long t1 = (-h - o) * 4096 * 4096 / dd;
				long long t2 = (h - o) * 4096 * 4096 / dd;
				if (t1 > t2) { long long t = t1; t1 = t2; t2 = t; }
				if (t1 > tmin) tmin = t1;
				if (t2 < tmax) tmax = t2;
				if (tmin > tmax)
					return -1;
			}
		}
	}

	// starting inside the car (e.g. standing in the door): ignore
	if (tmin == 0)
		return -1;

	return (int)(tmin / 4096);
}

static void ScarePedestrians(int sx, int sz)
{
	LPPEDESTRIAN p;

	for (p = pUsedPeds; p; p = p->pNext)
	{
		int dx, dz;

		if (p == player[0].pPed || p->pedType != CIVILIAN)
			continue;

		dx = p->position.vx - sx;
		dz = p->position.vz - sz;

		if (ABS(dx) > PISTOL_SCARE_RANGE || ABS(dz) > PISTOL_SCARE_RANGE)
			continue;

		if (player[0].pPed)
		{
			// on foot: dive away from Tanner
			bAvoidTanner = 1;
			SetupCivJump(p, NULL);
			bAvoidTanner = 0;
		}
		else if (player[0].playerCarId >= 0)
		{
			// in the car (Tanner has no pedestrian then): dive away from the car
			SetupCivJump(p, &car_data[player[0].playerCarId]);
		}
	}
}

extern int MapHeight(VECTOR* pos);

// height of the aim line (12 degrees down) at distance t, never below the ground
static int AimHeightAt(int oy, int t, VECTOR* at)
{
	VECTOR g;
	int y = oy + t * 870 / 4096;	// tan(12 deg) ~ 0.21
	int ground;

	g.vx = at->vx; g.vy = 0; g.vz = at->vz;
	ground = -MapHeight(&g);

	return (y > ground - 10) ? ground - 10 : y;
}

// one shot from (ox, oy, oz) in direction ang (yaw); excludeCar = the shooter's own car (-1 = none)
static void FireFrom(int ox, int oy, int oz, int ang, int excludeCar)
{
	int dx, dz;
	int i, best = -1, bestT = PISTOL_RANGE;
	int wallT = PISTOL_RANGE;
	VECTOR hit, vel;

	dx = RSIN(ang);
	dz = RCOS(ang);

	// walls: step along the ray until something blocks the line
	{
		VECTOR a, b;
		int t;

		a.vx = ox; a.vy = -oy; a.vz = oz;

		for (t = 512; t <= PISTOL_RANGE; t += 512)
		{
			b.vx = ox + FIXEDH(dx * t);
			b.vy = -oy;
			b.vz = oz + FIXEDH(dz * t);

			if (!lineClear(&a, &b))
			{
				wallT = t - 256;
				break;
			}

			a = b;
		}
	}

	// cars
	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		int r, t;

		if (cp->controlType == CONTROL_TYPE_NONE || !cp->ap.carCos || i == excludeCar)
			continue;

		(void)r;
		t = RayHitCar(cp, ox, oz, dx, dz, PISTOL_RANGE);

		if (t >= 0 && t < bestT && t < wallT)
		{
			bestT = t;
			best = i;
		}
	}

	vel.vx = -FIXEDH(dx * 40);
	vel.vy = -20;
	vel.vz = -FIXEDH(dz * 40);

	if (best >= 0)
	{
		CAR_DATA* cp = &car_data[best];

		hit.vx = ox + FIXEDH(dx * bestT);
		hit.vz = oz + FIXEDH(dz * bestT);
		hit.vy = AimHeightAt(oy, bestT, &hit);

		// keep the impact on the car body (between roof and ground)
		if (hit.vy < -cp->hd.where.t[1] - 300)
			hit.vy = -cp->hd.where.t[1] - 300;

		{
			extern int MaxPlayerDamage[2];
			extern int gModTestMode;
			int region = CarDamageRegion(cp, hit.vx, hit.vz);
			int before = cp->totalDamage;
			int value = MaxPlayerDamage[0] / PISTOL_HITS_TO_BURN / 2 + 1;	// ApplyDamage x2 for AI cars

			ApplyDamage(cp, region, value, 0);

			// player car / pursuers are scaled differently in ApplyDamage: top up the rest
			if (cp->totalDamage - before < MaxPlayerDamage[0] / PISTOL_HITS_TO_BURN)
			{
				int add = MaxPlayerDamage[0] / PISTOL_HITS_TO_BURN - (cp->totalDamage - before);
				cp->totalDamage = (cp->totalDamage + add < 0xffff) ? cp->totalDamage + add : 0xffff;
			}

			cp->ap.needsDenting = 1;

			if (gModTestMode)
				printf("PISTOLHIT car=%d region=%d total=%d/%d\n", best, region, cp->totalDamage, MaxPlayerDamage[0]);
		}

		Setup_Sparks(&hit, &vel, 8, 0);
	}
	else if (wallT < PISTOL_RANGE)
	{
		hit.vx = ox + FIXEDH(dx * wallT);
		hit.vz = oz + FIXEDH(dz * wallT);
		hit.vy = AimHeightAt(oy, wallT, &hit);

		Setup_Sparks(&hit, &vel, 5, 0);
	}
	else
	{
		hit.vx = ox + FIXEDH(dx * PISTOL_RANGE);
		hit.vz = oz + FIXEDH(dz * PISTOL_RANGE);
		hit.vy = AimHeightAt(oy, PISTOL_RANGE, &hit);
	}

	// tracer from the muzzle to the impact (GTA III style), visible for 2 frames
	g_tracerEnd = hit;
	g_tracerFrames = 2;

	g_flashFrames = 3;

	PlayShotSound();
	ScarePedestrians(ox, oz);

	// cops within hearing range react, whichever way they are facing
	{
		int c;

		for (c = 0; c < MAX_CARS; c++)
		{
			CAR_DATA* cop = &car_data[c];
			int hx, hz;

			if (!((cop->controlType == CONTROL_TYPE_PURSUER_AI && cop->ai.p.dying == 0) || (cop->controlFlags & CONTROL_FLAG_COP)))
				continue;

			hx = cop->hd.where.t[0] - ox;
			hz = cop->hd.where.t[2] - oz;

			if (ABS(hx) < PISTOL_HEAR_RANGE && ABS(hz) < PISTOL_HEAR_RANGE &&
				(long long)hx * hx + (long long)hz * hz < (long long)PISTOL_HEAR_RANGE * PISTOL_HEAR_RANGE)
			{
				gCopsHeardShot = PISTOL_HEAR_FRAMES;

				{
					extern int gModTestMode;
					if (gModTestMode)
						printf("PISTOL cop %d heard the shot at %d units\n", c, SquareRoot0((hx >> 4) * (hx >> 4) + (hz >> 4) * (hz >> 4)) << 4);
				}
				break;
			}
		}
	}

	// felony (NoteFelony zeroes it for Tanner on foot and needs cops to see you): raise it directly,
	// on the car you sit in or on Tanner
	{
		short* fel = (player[0].playerType == 1 && player[0].playerCarId >= 0)
			? &car_data[player[0].playerCarId].felonyRating : &pedestrianFelony;

		if (*fel < FELONY_MAX_VALUE - PISTOL_FELONY)
			*fel += PISTOL_FELONY;
		else
			*fel = FELONY_MAX_VALUE;
	}
}

static void Fire(LPPEDESTRIAN tanner)
{
	int oy = g_haveMuzzle ? g_lastMuzzle.vy : tanner->position.vy - 60;	// shot height = muzzle (up is negative)

	// no auto-aim: the shot goes straight where Tanner faces (his choice, 8 nov 2026)
	FireFrom(tanner->position.vx, oy, tanner->position.vz, tanner->dir.vy - 2048, -1);
}

// [mod test] fire without a key press
void Pistol_TestFire(void)
{
	if (player[0].pPed)
		Fire(player[0].pPed);
}

// called once per frame from ProcessTannerPad
void Pistol_Update(LPPEDESTRIAN tanner)
{
	static int prevG = 0, prevF = 0;
	int pressedG, pressedF;

	pressedG = KeyPressed(SDL_SCANCODE_G, &prevG);
	KeyPressed(SDL_SCANCODE_F, &prevF);
	pressedF = prevF;	// held = autofire (fires every PISTOL_FIRE_INTERVAL frames)

	if (GameType != GAME_TAKEADRIVE || !tanner || player[0].dying || tanner != player[0].pPed)
	{
		gPistolDrawn = 0;
		return;
	}

	if (g_cooldown > 0)
		g_cooldown--;

	// pistol away and no cop sees you: felony drains fast (~4 s from full)
	{
		extern int CopsCanSeePlayer;

		if (!gPistolDrawn && !CopsCanSeePlayer && pedestrianFelony > 0)
		{
			pedestrianFelony -= PISTOL_FELONY_DRAIN;

			if (pedestrianFelony < 0)
				pedestrianFelony = 0;
		}
	}

	if (pressedG)
	{
		gPistolDrawn ^= 1;
		SetPlayerMessage(0, (char*)(gPistolDrawn ? "Pistol out (F = shoot)" : "Pistol away"), 1, 1);
	}

	if (pressedF && gPistolDrawn && g_cooldown == 0)
	{
		Fire(tanner);
		g_cooldown = PISTOL_FIRE_INTERVAL;
	}
}

// called during drawing (after pedestrians). The muzzle flash is now drawn at the
// gun barrel inside Pistol_DrawHeld; nothing to do here anymore.
void Pistol_Draw(void)
{
}

// ---- held pistol: a tiny low-poly box gun in Tanner's right hand ----
// Called from newShowTanner (motion_c.c) once the skeleton joint positions are known.
// jHand/jElbow/jFingers are joint positions relative to Tanner; base = Tanner pos - camera.

extern void Apply_Inv_CameraMatrix(VECTOR* v);
extern MATRIX identity;

static void Norm(int* x, int* y, int* z, int len)
{
	int l = SquareRoot0((*x) * (*x) + (*y) * (*y) + (*z) * (*z));

	if (l == 0) { *x = len; *y = 0; *z = 0; return; }

	*x = *x * len / l;
	*y = *y * len / l;
	*z = *z * len / l;
}

// Project points the way the game does (ShowLight): view-space via the inverse camera
// matrix, identity rotation, translation = first point, and write the screen coords
// straight into the primitive so PGXP can match them.
static void ToView(VECTOR* camRel, VECTOR* out)
{
	*out = *camRel;
	Apply_Inv_CameraMatrix(out);
}

static int OtFromZ(int z, int bias)
{
	int ot = (z >> 3) + bias;
	if (ot < 1) ot = 1;
	if (ot > 0x1000) ot = 0x1000;
	return ot;
}

static void GunQuad(VECTOR* p0, VECTOR* p1, VECTOR* p2, VECTOR* p3, int shade)
{
	POLY_F4* poly;
	VECTOR v[4];
	SVECTOR sv[4];
	long z0, z1, z2, z3;
	int k;

	ToView(p0, &v[0]); ToView(p1, &v[1]); ToView(p2, &v[2]); ToView(p3, &v[3]);

	for (k = 0; k < 4; k++)
	{
		if (v[k].vz < 40)
			return;	// behind / too close to the camera

		sv[k].vx = v[k].vx - v[0].vx;
		sv[k].vy = v[k].vy - v[0].vy;
		sv[k].vz = v[k].vz - v[0].vz;
	}

	poly = (POLY_F4*)current->primptr;
	setPolyF4(poly);

	if (shade < 0) shade = 0;
	poly->r0 = shade; poly->g0 = shade; poly->b0 = shade + 6;

	gte_SetRotMatrix(&identity);
	gte_SetTransVector(&v[0]);

	// F4 vertex order: 0,1 top edge, 2,3 bottom edge -> p0, p1, p3, p2
	gte_ldv3(&sv[0], &sv[1], &sv[3]);
	gte_rtpt();
	gte_stsxy3(&poly->x0, &poly->x1, &poly->x2);
	gte_stsz3(&z0, &z1, &z2);

	gte_ldv0(&sv[2]);
	gte_rtps();
	gte_stsxy(&poly->x3);
	gte_stsz(&z3);

	addPrim(current->ot + OtFromZ(MAX(MAX(z0, z1), MAX(z2, z3)), -3), poly);
	current->primptr += sizeof(POLY_F4);
}

static void GunBox(VECTOR* c, int ux, int uy, int uz, int px, int py, int pz, int qx, int qy, int qz,
	int len0, int len1, int hw, int hh, int shade)
{
	// 8 corners: along u from len0..len1, half width hw along p, half height hh along q
	VECTOR v[8];
	int i;

	for (i = 0; i < 8; i++)
	{
		int a = (i & 1) ? len1 : len0;
		int b = (i & 2) ? hw : -hw;
		int d = (i & 4) ? hh : -hh;

		v[i].vx = c->vx + (ux * a + px * b + qx * d) / 4096;
		v[i].vy = c->vy + (uy * a + py * b + qy * d) / 4096;
		v[i].vz = c->vz + (uz * a + pz * b + qz * d) / 4096;
	}

	// 4 long sides + front cap (no culling: draw all, the OT sorts them)
	GunQuad(&v[0], &v[1], &v[3], &v[2], shade);			// -q side
	GunQuad(&v[4], &v[5], &v[7], &v[6], shade + 14);		// +q side (top, lighter)
	GunQuad(&v[0], &v[1], &v[5], &v[4], shade + 6);		// -p side
	GunQuad(&v[2], &v[3], &v[7], &v[6], shade + 6);		// +p side
	GunQuad(&v[1], &v[3], &v[7], &v[5], shade - 8);		// muzzle cap
}

static void FlashTri(VECTOR* a, VECTOR* b, VECTOR* c, int r, int g, int bl)
{
	POLY_F3* poly;
	VECTOR v[3];
	SVECTOR sv[3];
	long z0, z1, z2;
	int k;

	ToView(a, &v[0]); ToView(b, &v[1]); ToView(c, &v[2]);

	for (k = 0; k < 3; k++)
	{
		if (v[k].vz < 40)
			return;

		sv[k].vx = v[k].vx - v[0].vx;
		sv[k].vy = v[k].vy - v[0].vy;
		sv[k].vz = v[k].vz - v[0].vz;
	}

	poly = (POLY_F3*)current->primptr;
	setPolyF3(poly);
	poly->r0 = r; poly->g0 = g; poly->b0 = bl;

	gte_SetRotMatrix(&identity);
	gte_SetTransVector(&v[0]);
	gte_ldv3(&sv[0], &sv[1], &sv[2]);
	gte_rtpt();
	gte_stsxy3(&poly->x0, &poly->x1, &poly->x2);
	gte_stsz3(&z0, &z1, &z2);

	addPrim(current->ot + OtFromZ(MIN(MIN(z0, z1), z2), -6), poly);
	current->primptr += sizeof(POLY_F3);
}

// tracer ribbon: bright near the gun, fading to orange at the impact
static void DrawTracer(VECTOR* a, VECTOR* b)
{
	POLY_G4* poly;
	VECTOR v[4], w[4];
	SVECTOR sv[4];
	long z0, z1, z2, z3;
	int tx = b->vx - a->vx, tz = b->vz - a->vz;
	int px = -tz, pz = tx, py0 = 0, k;

	Norm(&px, &py0, &pz, 4096);

	// ribbon width: 2 at the gun, 6 at the far end (world units)
	w[0].vx = a->vx + px * 2 / 4096; w[0].vy = a->vy; w[0].vz = a->vz + pz * 2 / 4096;
	w[1].vx = a->vx - px * 2 / 4096; w[1].vy = a->vy; w[1].vz = a->vz - pz * 2 / 4096;
	w[2].vx = b->vx + px * 6 / 4096; w[2].vy = b->vy; w[2].vz = b->vz + pz * 6 / 4096;
	w[3].vx = b->vx - px * 6 / 4096; w[3].vy = b->vy; w[3].vz = b->vz - pz * 6 / 4096;

	for (k = 0; k < 4; k++)
	{
		ToView(&w[k], &v[k]);

		if (v[k].vz < 40)
			return;

		sv[k].vx = v[k].vx - v[0].vx;
		sv[k].vy = v[k].vy - v[0].vy;
		sv[k].vz = v[k].vz - v[0].vz;
	}

	poly = (POLY_G4*)current->primptr;
	setPolyG4(poly);

	poly->r0 = 255; poly->g0 = 250; poly->b0 = 210;
	poly->r1 = 255; poly->g1 = 250; poly->b1 = 210;
	poly->r2 = 255; poly->g2 = 160; poly->b2 = 50;
	poly->r3 = 255; poly->g3 = 160; poly->b3 = 50;

	gte_SetRotMatrix(&identity);
	gte_SetTransVector(&v[0]);
	gte_ldv3(&sv[0], &sv[1], &sv[2]);
	gte_rtpt();
	gte_stsxy3(&poly->x0, &poly->x1, &poly->x2);
	gte_stsz3(&z0, &z1, &z2);
	gte_ldv0(&sv[3]);
	gte_rtps();
	gte_stsxy(&poly->x3);
	gte_stsz(&z3);

	addPrim(current->ot + OtFromZ(MIN(MIN(z0, z1), MIN(z2, z3)), -4), poly);
	current->primptr += sizeof(POLY_G4);
}

// star-shaped fire at the muzzle: long spike forward, short spikes around
static void DrawMuzzleFlash(VECTOR* m, int ux, int uy, int uz, int px, int py, int pz, int qx, int qy, int qz)
{
	int i;
	int rot = (CameraCnt * 1237) & 0xfff;	// spin per frame so it flickers

	for (i = 0; i < 6; i++)
	{
		int ang = rot + i * 682;
		int cs = RCOS(ang), sn = RSIN(ang);
		int cs2 = RCOS(ang + 341), sn2 = RSIN(ang + 341);
		int len = (i & 1) ? 16 : 28;			// alternating spike length
		VECTOR tip, s0, s1;

		// radial direction in the p/q plane, tilted forward along u
		tip.vx = m->vx + (ux * 12 + (px * cs + qx * sn) / 4096 * len) / 4096;
		tip.vy = m->vy + (uy * 12 + (py * cs + qy * sn) / 4096 * len) / 4096;
		tip.vz = m->vz + (uz * 12 + (pz * cs + qz * sn) / 4096 * len) / 4096;

		s0.vx = m->vx + ((px * cs2 + qx * sn2) / 4096 * 5) / 4096;
		s0.vy = m->vy + ((py * cs2 + qy * sn2) / 4096 * 5) / 4096;
		s0.vz = m->vz + ((pz * cs2 + qz * sn2) / 4096 * 5) / 4096;

		s1.vx = m->vx - ((px * cs2 + qx * sn2) / 4096 * 5) / 4096;
		s1.vy = m->vy - ((py * cs2 + qy * sn2) / 4096 * 5) / 4096;
		s1.vz = m->vz - ((pz * cs2 + qz * sn2) / 4096 * 5) / 4096;

		FlashTri(&s0, &s1, &tip, 255, (i & 1) ? 150 : 210, 40);
	}

	// long forward spike + bright core
	{
		VECTOR tip, s0, s1;

		tip.vx = m->vx + ux * 50 / 4096;
		tip.vy = m->vy + uy * 50 / 4096;
		tip.vz = m->vz + uz * 50 / 4096;

		s0.vx = m->vx + px * 7 / 4096; s0.vy = m->vy + py * 7 / 4096; s0.vz = m->vz + pz * 7 / 4096;
		s1.vx = m->vx - px * 7 / 4096; s1.vy = m->vy - py * 7 / 4096; s1.vz = m->vz - pz * 7 / 4096;
		FlashTri(&s0, &s1, &tip, 255, 240, 160);

		s0.vx = m->vx + qx * 7 / 4096; s0.vy = m->vy + qy * 7 / 4096; s0.vz = m->vz + qz * 7 / 4096;
		s1.vx = m->vx - qx * 7 / 4096; s1.vy = m->vy - qy * 7 / 4096; s1.vz = m->vz - qz * 7 / 4096;
		FlashTri(&s0, &s1, &tip, 255, 240, 160);
	}
}

void Pistol_DrawHeld(SVECTOR* jHand, SVECTOR* jElbow, VECTOR* base)
{
	VECTOR hand;
	int ux, uy, uz, px, py, pz, qx, qy, qz;

	MATRIX savedGte;

	if (!gPistolDrawn)
		return;

	// the bones are drawn right after this with the same GTE state: save it, restore at the end
	gte_ReadRotMatrix(&savedGte);

	hand.vx = base->vx + jHand->vx;
	hand.vy = base->vy + jHand->vy;
	hand.vz = base->vz + jHand->vz;

	// barrel direction = forearm direction (elbow -> hand)
	ux = jHand->vx - jElbow->vx;
	uy = jHand->vy - jElbow->vy;
	uz = jHand->vz - jElbow->vz;
	Norm(&ux, &uy, &uz, 4096);

	{
		extern int gModTestMode;
		if (gModTestMode == 2)
		{
			LPPEDESTRIAN t = player[0].pPed;
			int fa = t ? t->dir.vy - 2048 : 0;
			int fx = RSIN(fa), fz = RCOS(fa);
			printf("AIMDBG rot=(%d,%d,%d) fwd=%d side=%d up=%d\n", gAimRot[0].vx, gAimRot[0].vy, gAimRot[0].vz,
				(ux * fx + uz * fz) / 4096, (ux * fz - uz * fx) / 4096, -uy);
		}
	}

	// side vector p = u x up, height vector q = p x u
	px = -uz; py = 0; pz = ux;
	Norm(&px, &py, &pz, 4096);

	qx = (py * uz - pz * uy) / 4096;
	qy = (pz * ux - px * uz) / 4096;
	qz = (px * uy - py * ux) / 4096;
	Norm(&qx, &qy, &qz, 4096);

	// q must point up (up is negative y here), so the grip (-q) hangs down
	if (qy > 0)
	{
		qx = -qx; qy = -qy; qz = -qz;
	}

	// barrel: slightly behind the hand to well in front of it
	GunBox(&hand, ux, uy, uz, px, py, pz, qx, qy, qz, -3, 24, 3, 4, 18);

	// muzzle (barrel end), remembered in world space for the next shot's height
	{
		VECTOR m;
		m.vx = hand.vx + ux * 26 / 4096;
		m.vy = hand.vy + uy * 26 / 4096;
		m.vz = hand.vz + uz * 26 / 4096;

		g_lastMuzzle.vx = m.vx + camera_position.vx;
		g_lastMuzzle.vy = m.vy + camera_position.vy;
		g_lastMuzzle.vz = m.vz + camera_position.vz;
		g_haveMuzzle = 1;

		if (g_tracerFrames > 0)
		{
			VECTOR e;
			e.vx = g_tracerEnd.vx - camera_position.vx;
			e.vy = g_tracerEnd.vy - camera_position.vy;
			e.vz = g_tracerEnd.vz - camera_position.vz;

			DrawTracer(&m, &e);
			g_tracerFrames--;
		}

		if (g_flashFrames > 0)
		{
			DrawMuzzleFlash(&m, ux, uy, uz, px, py, pz, qx, qy, qz);
			g_flashFrames--;
		}
	}

	// grip: short block hanging from the back of the barrel, along -q
	{
		VECTOR g;
		g.vx = hand.vx + (ux * 2 - qx * 7) / 4096;
		g.vy = hand.vy + (uy * 2 - qy * 7) / 4096;
		g.vz = hand.vz + (uz * 2 - qz * 7) / 4096;

		GunBox(&g, qx, qy, qz, px, py, pz, ux, uy, uz, -6, 6, 3, 3, 12);
	}

	gte_SetRotMatrix(&savedGte);
	gte_SetTransMatrix(&savedGte);
}


// [mod] sprint: either Shift key while running forward on foot
int gSprintTest = 0;	// forced on by -modtest

int Sprint_Held(void)
{
#ifndef PSX
	const Uint8* ks = SDL_GetKeyboardState(NULL);
	if (ks && (ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT]))
		return 1;
#endif
	return gSprintTest;
}

// ---- REDRIVER2 mac mod: shooting from the car (drive-by) ----
// F (held = autofire) while driving in Take a Ride. Look left (A / L2) = arm out of the
// left window, look right (D / R2) = out of the right window, otherwise forward out of
// the driver's window. Tanner isn't drawn in the car: only his arm and the pistol.

#include "pad.h"

static int g_carArmFrames = 0;		// arm visible for a moment after shooting
static VECTOR g_carShoulder, g_carHand;	// world (camera convention: up = negative y)
static int g_carAimYaw = 0;
int gCarShootTest = 0;				// -modtest: 1 = left, 2 = right, 3 = forward, 4 = back

static void CarArmGeometry(CAR_DATA* cp, int side)
{
	int d = cp->hd.direction;
	int fx = RSIN(d), fz = RCOS(d);		// forward
	int rx = RCOS(d), rz = -RSIN(d);	// right
	int hw = cp->ap.carCos->colBox.vx;
	int hl = cp->ap.carCos->colBox.vz;
	int hh = cp->ap.carCos->colBox.vy;
	int cx = cp->hd.where.t[0], cy = -cp->hd.where.t[1], cz = cp->hd.where.t[2];
	int sideSign = (side == 1) ? 1 : -1;		// forward/back shots: driver's (left) window
	int ux, uz;

	// shoulder just inside the window, a bit ahead of the car centre, at window height
	g_carShoulder.vx = cx + FIXEDH(rx * (hw - 40) * sideSign) + FIXEDH(fx * hl / 8);
	g_carShoulder.vz = cz + FIXEDH(rz * (hw - 40) * sideSign) + FIXEDH(fz * hl / 8);
	g_carShoulder.vy = cy - hh * 3 / 2;

	if (side == 0)
	{
		// forward: arm leans out of the driver's window and points ahead
		ux = fx; uz = fz;
		g_carHand.vx = g_carShoulder.vx + FIXEDH(rx * 60 * sideSign) + FIXEDH(fx * 70);
		g_carHand.vz = g_carShoulder.vz + FIXEDH(rz * 60 * sideSign) + FIXEDH(fz * 70);
	}
	else if (side == 2)
	{
		// backwards: arm out of the driver's window, pointing behind the car
		ux = -fx; uz = -fz;
		g_carHand.vx = g_carShoulder.vx - FIXEDH(rx * 60) - FIXEDH(fx * 70);
		g_carHand.vz = g_carShoulder.vz - FIXEDH(rz * 60) - FIXEDH(fz * 70);
	}
	else
	{
		ux = rx * sideSign; uz = rz * sideSign;
		g_carHand.vx = g_carShoulder.vx + FIXEDH(ux * 100);
		g_carHand.vz = g_carShoulder.vz + FIXEDH(uz * 100);
	}

	g_carHand.vy = g_carShoulder.vy + 6;
	g_carAimYaw = ratan2(ux, uz);
}

// called once per frame from the game loop
void Pistol_CarFrame(void)
{
	static int prevF = 0;
	CAR_DATA* cp;
	int side, fire;

	KeyPressed(SDL_SCANCODE_F, &prevF);
	fire = prevF || gCarShootTest;

	if (GameType != GAME_TAKEADRIVE || player[0].playerType != 1 || player[0].playerCarId < 0)
	{
		g_carArmFrames = 0;
		return;
	}

	cp = &car_data[player[0].playerCarId];

	if (!cp->ap.carCos)
		return;

	if (g_cooldown > 0)
		g_cooldown--;

	if (gCarShootTest)
		side = (gCarShootTest == 1) ? -1 : (gCarShootTest == 2) ? 1 : (gCarShootTest == 4) ? 2 : 0;
	else if ((Pads[0].mapped & MPAD_L3) || ((Pads[0].mapped & MPAD_L2) && (Pads[0].mapped & MPAD_R2)))
		side = 2;	// looking back (S, or A+D together)
	else if ((Pads[0].mapped & MPAD_L2) && !(Pads[0].mapped & MPAD_R2))
		side = -1;
	else if ((Pads[0].mapped & MPAD_R2) && !(Pads[0].mapped & MPAD_L2))
		side = 1;
	else
		side = 0;

	CarArmGeometry(cp, side);

	if (fire)
	{
		g_carArmFrames = 30;

		if (g_cooldown == 0)
		{
			VECTOR muzzle;

			muzzle.vx = g_carHand.vx + FIXEDH(RSIN(g_carAimYaw) * 26);
			muzzle.vz = g_carHand.vz + FIXEDH(RCOS(g_carAimYaw) * 26);
			muzzle.vy = g_carHand.vy;

			FireFrom(muzzle.vx, muzzle.vy, muzzle.vz, g_carAimYaw, player[0].playerCarId);
			{
				extern int gModTestMode;
				if (gModTestMode)
				{
					CAR_DATA* dcp = &car_data[player[0].playerCarId];
					int d = dcp->hd.direction;
					int dx = g_tracerEnd.vx - muzzle.vx, dz = g_tracerEnd.vz - muzzle.vz;
					printf("CARSHOT side=%d fwd=%d right=%d\n", side,
						FIXEDH(dx * RSIN(d) + dz * RCOS(d)), FIXEDH(dx * RCOS(d) - dz * RSIN(d)));
				}
			}
			g_cooldown = PISTOL_FIRE_INTERVAL;
		}
	}
}

// called during drawing: arm + pistol (+ flash and tracer) out of the window
void Pistol_CarDraw(void)
{
	VECTOR sh, hand, m;
	int ux, uy, uz, px, py, pz, qx, qy, qz, len;
	MATRIX savedGte;

	if (g_carArmFrames <= 0 || player[0].playerType != 1)
		return;

	g_carArmFrames--;

	gte_ReadRotMatrix(&savedGte);

	sh.vx = g_carShoulder.vx - camera_position.vx;
	sh.vy = g_carShoulder.vy - camera_position.vy;
	sh.vz = g_carShoulder.vz - camera_position.vz;

	hand.vx = g_carHand.vx - camera_position.vx;
	hand.vy = g_carHand.vy - camera_position.vy;
	hand.vz = g_carHand.vz - camera_position.vz;

	// arm: dark suit sleeve from shoulder to hand
	ux = hand.vx - sh.vx; uy = hand.vy - sh.vy; uz = hand.vz - sh.vz;
	len = SquareRoot0(ux * ux + uy * uy + uz * uz);
	Norm(&ux, &uy, &uz, 4096);
	px = -uz; py = 0; pz = ux;
	Norm(&px, &py, &pz, 4096);
	qx = 0; qy = -4096; qz = 0;		// up

	GunBox(&sh, ux, uy, uz, px, py, pz, qx, qy, qz, 0, len, 9, 9, 22);

	// hand (skin)
	GunBox(&hand, ux, uy, uz, px, py, pz, qx, qy, qz, -4, 8, 6, 6, 120);

	// pistol along the aim direction
	ux = RSIN(g_carAimYaw); uy = 0; uz = RCOS(g_carAimYaw);
	px = -uz; py = 0; pz = ux;
	GunBox(&hand, ux, uy, uz, px, py, pz, qx, qy, qz, -3, 24, 3, 4, 18);

	{
		VECTOR g;
		g.vx = hand.vx + (ux * 2 - qx * 7) / 4096;
		g.vy = hand.vy + (uy * 2 - qy * 7) / 4096;
		g.vz = hand.vz + (uz * 2 - qz * 7) / 4096;
		GunBox(&g, qx, qy, qz, px, py, pz, ux, uy, uz, -6, 6, 3, 3, 12);
	}

	m.vx = hand.vx + ux * 26 / 4096;
	m.vy = hand.vy;
	m.vz = hand.vz + uz * 26 / 4096;

	if (g_tracerFrames > 0)
	{
		VECTOR e;
		e.vx = g_tracerEnd.vx - camera_position.vx;
		e.vy = g_tracerEnd.vy - camera_position.vy;
		e.vz = g_tracerEnd.vz - camera_position.vz;

		DrawTracer(&m, &e);
		g_tracerFrames--;
	}

	if (g_flashFrames > 0)
	{
		DrawMuzzleFlash(&m, ux, uy, uz, px, py, pz, qx, qy, qz);
		g_flashFrames--;
	}

	gte_SetRotMatrix(&savedGte);
	gte_SetTransMatrix(&savedGte);
}
