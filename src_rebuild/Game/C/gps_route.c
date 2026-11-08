#include "driver2.h"
#include "dr2roads.h"
#include "players.h"
#include "mission.h"
#include "waypoint.h"
#include "gps_route.h"

#include <stdlib.h>
#include <string.h>

// REDRIVER2 mac mod: GPS route over the road network.
//
// The game keeps the whole city's road network in memory: straights, curves and
// junctions, linked through ConnectIdx / ExitIdx. Each road piece is a graph node;
// a shortest path (Dijkstra) from the player's road to the road nearest the
// waypoint becomes a polyline that overmap.c draws on both maps.
//
// One-way streets are ignored for now (route may go against traffic).

VECTOR gGpsRoutePts[GPS_MAX_POINTS];
int gGpsRouteCount = 0;

static int* g_dist = NULL;
static short* g_prev = NULL;
static int* g_heap = NULL;
static char* g_onPath = NULL;
static int g_nodeCap = 0;

static int g_lastStartNode = -1;
static int g_lastTargetNode = -1;
static VECTOR g_lastWaypoint = { 0 };
static int g_lastLevel = -1;
static int g_framesSinceCalc = 0;

#define NUM_S	NumDriver2Straights
#define NUM_C	NumDriver2Curves
#define NUM_J	NumDriver2Junctions
#define NUM_NODES	(NUM_S + NUM_C + NUM_J)

static int SurfToNode(int surfId)
{
	if (IS_STRAIGHT_SURFACE(surfId))
		return surfId & 0x1FFF;

	if (IS_CURVED_SURFACE(surfId))
		return NUM_S + (surfId & 0x1FFF);

	if (IS_JUNCTION_SURFACE(surfId))
		return NUM_S + NUM_C + (surfId & 0x1FFF);

	return -1;
}

static int CurveRadius(DRIVER2_CURVE* c)
{
	// inner edge = inside * 1024, lanes are 512 wide; take the road centre
	return c->inside * 1024 + ROAD_LANES_COUNT(c) * 512;
}

static int CurveSpan(DRIVER2_CURVE* c)
{
	return (c->end - c->start) & 0xfff;
}

static int NodeCost(int n)
{
	if (n < NUM_S)
		return Driver2StraightsPtr[n].length;

	if (n < NUM_S + NUM_C)
	{
		DRIVER2_CURVE* c = &Driver2CurvesPtr[n - NUM_S];
		// arc length = r * span * 2pi / 4096
		return (int)((long long)CurveRadius(c) * CurveSpan(c) * 6283 / (4096 * 1000));
	}

	return 1024;	// junctions have no geometry; small fixed cost
}

static int NodeNeighbours(int n, int* out)
{
	short* idx;
	int i, cnt = 0;

	if (n < NUM_S)
		idx = Driver2StraightsPtr[n].ConnectIdx;
	else if (n < NUM_S + NUM_C)
		idx = Driver2CurvesPtr[n - NUM_S].ConnectIdx;
	else
		idx = Driver2JunctionsPtr[n - NUM_S - NUM_C].ExitIdx;

	for (i = 0; i < 4; i++)
	{
		int nb = SurfToNode(idx[i]);

		if (nb >= 0 && nb != n)
			out[cnt++] = nb;
	}

	return cnt;
}

static void StraightEnds(DRIVER2_STRAIGHT* s, VECTOR* a, VECTOR* b)
{
	int hx = FIXEDH(RSIN(s->angle) * (s->length / 2));
	int hz = FIXEDH(RCOS(s->angle) * (s->length / 2));

	a->vx = s->Midx - hx; a->vz = s->Midz - hz; a->vy = 0;
	b->vx = s->Midx + hx; b->vz = s->Midz + hz; b->vy = 0;
}

static void CurvePoint(DRIVER2_CURVE* c, int ang, VECTOR* p)
{
	int r = CurveRadius(c);

	p->vx = c->Midx + FIXEDH(RSIN(ang) * r);
	p->vz = c->Midz + FIXEDH(RCOS(ang) * r);
	p->vy = 0;
}

static long long Dist2(VECTOR* a, VECTOR* b)
{
	long long dx = a->vx - b->vx;
	long long dz = a->vz - b->vz;
	return dx * dx + dz * dz;
}

static long long DistToSegment2(VECTOR* p, VECTOR* a, VECTOR* b)
{
	long long abx = b->vx - a->vx, abz = b->vz - a->vz;
	long long apx = p->vx - a->vx, apz = p->vz - a->vz;
	long long len2 = abx * abx + abz * abz;
	long long t;
	VECTOR q;

	if (len2 == 0)
		return Dist2(p, a);

	t = ((apx * abx + apz * abz) * 4096) / len2;

	if (t < 0) t = 0;
	if (t > 4096) t = 4096;

	q.vx = a->vx + (int)(abx * t / 4096);
	q.vz = a->vz + (int)(abz * t / 4096);

	return Dist2(p, &q);
}

// nearest straight or curve to a world position
static int NearestRoadNode(VECTOR* pos)
{
	int i, best = -1;
	long long bestD = 0x7fffffffffffffffLL;
	VECTOR a, b;

	for (i = 0; i < NUM_S; i++)
	{
		long long d;
		StraightEnds(&Driver2StraightsPtr[i], &a, &b);
		d = DistToSegment2(pos, &a, &b);

		if (d < bestD) { bestD = d; best = i; }
	}

	for (i = 0; i < NUM_C; i++)
	{
		DRIVER2_CURVE* c = &Driver2CurvesPtr[i];
		int span = CurveSpan(c);
		int k;

		// sample the arc in 4 pieces
		CurvePoint(c, c->start, &a);

		for (k = 1; k <= 4; k++)
		{
			long long d;
			CurvePoint(c, c->start + span * k / 4, &b);
			d = DistToSegment2(pos, &a, &b);

			if (d < bestD) { bestD = d; best = NUM_S + i; }

			a = b;
		}
	}

	return best;
}

static int EnsureBuffers(void)
{
	int n = NUM_NODES;

	if (n <= 0)
		return 0;

	if (n > g_nodeCap)
	{
		free(g_dist); free(g_prev); free(g_heap); free(g_onPath);

		g_dist = (int*)malloc(sizeof(int) * n);
		g_prev = (short*)malloc(sizeof(short) * n);
		g_heap = (int*)malloc(sizeof(int) * n * 4 + 64);
		g_onPath = (char*)malloc(n);
		g_nodeCap = n;
	}

	return g_dist && g_prev && g_heap && g_onPath;
}

// binary min-heap of (dist, node) packed as node only, keyed by g_dist (lazy deletion)
static int g_heapSize;

static void HeapPush(int node)
{
	int i = g_heapSize++;

	g_heap[i] = node;

	while (i > 0)
	{
		int p = (i - 1) / 2;

		if (g_dist[g_heap[p]] <= g_dist[g_heap[i]])
			break;

		int t = g_heap[p]; g_heap[p] = g_heap[i]; g_heap[i] = t;
		i = p;
	}
}

static int HeapPop(void)
{
	int top = g_heap[0];
	int i = 0;

	g_heap[0] = g_heap[--g_heapSize];

	for (;;)
	{
		int l = i * 2 + 1, r = l + 1, m = i;

		if (l < g_heapSize && g_dist[g_heap[l]] < g_dist[g_heap[m]]) m = l;
		if (r < g_heapSize && g_dist[g_heap[r]] < g_dist[g_heap[m]]) m = r;

		if (m == i)
			break;

		int t = g_heap[m]; g_heap[m] = g_heap[i]; g_heap[i] = t;
		i = m;
	}

	return top;
}

static void AddPoint(VECTOR* p)
{
	if (gGpsRouteCount >= GPS_MAX_POINTS)
		return;

	gGpsRoutePts[gGpsRouteCount++] = *p;
}

// both ends of a straight or curve
static void RoadEnds(int node, VECTOR* a, VECTOR* b)
{
	if (node < NUM_S)
	{
		StraightEnds(&Driver2StraightsPtr[node], a, b);
	}
	else
	{
		DRIVER2_CURVE* c = &Driver2CurvesPtr[node - NUM_S];
		CurvePoint(c, c->start, a);
		CurvePoint(c, c->start + CurveSpan(c), b);
	}
}

static int Calculate(int start, int target, VECTOR* from, VECTOR* to)
{
	int n = NUM_NODES;
	int i, nb[4];
	int maxHeap = n * 4;
	static int path[GPS_MAX_POINTS];
	int pathLen = 0;

	for (i = 0; i < n; i++)
	{
		g_dist[i] = 0x7fffffff;
		g_prev[i] = -1;
		g_onPath[i] = 0;
	}

	g_heapSize = 0;
	g_dist[start] = 0;
	HeapPush(start);

	while (g_heapSize > 0)
	{
		int u = HeapPop();
		int cnt, k, cu;

		if (u == target)
			break;

		cu = NodeCost(u);
		cnt = NodeNeighbours(u, nb);

		for (k = 0; k < cnt; k++)
		{
			int v = nb[k];
			int nd = g_dist[u] + (cu + NodeCost(v)) / 2 + 1;

			if (nd < g_dist[v])
			{
				g_dist[v] = nd;
				g_prev[v] = u;

				if (g_heapSize < maxHeap)
					HeapPush(v);
			}
		}
	}

	// target not connected to the network the player is on (e.g. a dirt track that
	// isn't linked in the road data): find the pair (reachable piece r, piece t of the
	// target's own little network) that lie closest together. Route = start..r over
	// the roads, a short straight hop r->t, then t..target over the track.
	if (g_dist[target] == 0x7fffffff)
	{
		static char* reach = NULL;
		static short* prevStart = NULL;
		static int cap = 0;
		long long bestD = 0x7fffffffffffffffLL;
		int bestR = -1, bestT = -1, r, t, k;
		static int seq[GPS_MAX_POINTS];
		int seqLen = 0;

		if (cap < n)
		{
			free(reach); free(prevStart);
			reach = (char*)malloc(n);
			prevStart = (short*)malloc(sizeof(short) * n);
			cap = n;
		}

		for (i = 0; i < n; i++)
		{
			reach[i] = g_dist[i] != 0x7fffffff;
			prevStart[i] = g_prev[i];
			g_dist[i] = 0x7fffffff;
			g_prev[i] = -1;
		}

		// second search, from the target over its own network
		g_heapSize = 0;
		g_dist[target] = 0;
		HeapPush(target);

		while (g_heapSize > 0)
		{
			int u = HeapPop();
			int cnt = NodeNeighbours(u, nb), cu = NodeCost(u);

			for (k = 0; k < cnt; k++)
			{
				int v = nb[k];
				int nd = g_dist[u] + (cu + NodeCost(v)) / 2 + 1;

				if (nd < g_dist[v])
				{
					g_dist[v] = nd;
					g_prev[v] = u;
					if (g_heapSize < maxHeap)
						HeapPush(v);
				}
			}
		}

		// closest pair of road pieces (by their end points)
		for (t = 0; t < NUM_S + NUM_C; t++)
		{
			VECTOR ta, tb;

			if (g_dist[t] == 0x7fffffff)
				continue;

			RoadEnds(t, &ta, &tb);

			for (r = 0; r < NUM_S + NUM_C; r++)
			{
				VECTOR ra, rb;
				long long d;

				if (!reach[r])
					continue;

				RoadEnds(r, &ra, &rb);
				d = MIN(MIN(Dist2(&ra, &ta), Dist2(&ra, &tb)), MIN(Dist2(&rb, &ta), Dist2(&rb, &tb)));

				if (d < bestD)
				{
					bestD = d;
					bestR = r;
					bestT = t;
				}
			}
		}

		if (bestR < 0)
			return 0;

		// start .. bestR (walk prevStart back from bestR, then reverse)
		for (i = bestR; i != -1 && seqLen < GPS_MAX_POINTS; i = prevStart[i])
			seq[seqLen++] = i;

		for (k = 0; k < seqLen / 2; k++)
		{
			int tmp = seq[k]; seq[k] = seq[seqLen - 1 - k]; seq[seqLen - 1 - k] = tmp;
		}

		// bestT .. target (g_prev points towards the target)
		for (i = bestT; i != -1 && seqLen < GPS_MAX_POINTS; i = g_prev[i])
			seq[seqLen++] = i;

		// the polyline code below walks path[] from the end (start) to index 0 (target)
		for (k = 0; k < seqLen; k++)
			path[k] = seq[seqLen - 1 - k];

		pathLen = seqLen;
	}
	else
	{
		// walk back
		for (i = target; i != -1 && pathLen < GPS_MAX_POINTS; i = g_prev[i])
			path[pathLen++] = i;
	}

	// build polyline from start to target.
	// Only roads (straights/curves) have geometry; junctions are just links.
	{
		static int roads[GPS_MAX_POINTS];
		int R = 0, k;
		int startOnRoad = path[pathLen - 1] < NUM_S + NUM_C;

		for (i = pathLen - 1; i >= 0; i--)
		{
			g_onPath[path[i]] = 1;

			if (path[i] < NUM_S + NUM_C)
				roads[R++] = path[i];
		}

		gGpsRouteCount = 0;
		AddPoint(from);

		for (k = 0; k < R; k++)
		{
			VECTOR e0, e1, entry, exitp;
			int node = roads[k];
			int reversed = 0;

			RoadEnds(node, &e0, &e1);

			// orient: exit = the end nearest to the next road; for the last road
			// entry = the end nearest to the previous point
			if (k < R - 1)
			{
				VECTOR n0, n1;
				RoadEnds(roads[k + 1], &n0, &n1);

				long long d0 = MIN(Dist2(&e0, &n0), Dist2(&e0, &n1));
				long long d1 = MIN(Dist2(&e1, &n0), Dist2(&e1, &n1));

				reversed = d0 < d1;	// e0 is the exit -> travel e1 -> e0
			}
			else
			{
				VECTOR* last = &gGpsRoutePts[gGpsRouteCount - 1];
				reversed = Dist2(last, &e1) < Dist2(last, &e0);
			}

			entry = reversed ? e1 : e0;
			exitp = reversed ? e0 : e1;

			if (k == 0 && startOnRoad)
			{
				// the player is on this road: only draw what's ahead
				if (R > 1)
					AddPoint(&exitp);
				continue;
			}

			AddPoint(&entry);

			if (node >= NUM_S)
			{
				DRIVER2_CURVE* c = &Driver2CurvesPtr[node - NUM_S];
				int span = CurveSpan(c);
				int st, steps = 6;
				VECTOR p;

				for (st = 1; st < steps; st++)
				{
					int t = reversed ? steps - st : st;
					CurvePoint(c, c->start + span * t / steps, &p);
					AddPoint(&p);
				}
			}

			if (k < R - 1)
				AddPoint(&exitp);
		}

		AddPoint(to);
	}

	return 1;
}

void GpsRoute_Clear(void)
{
	gGpsRouteCount = 0;
	g_lastStartNode = -1;
	g_lastTargetNode = -1;
}

void GpsRoute_Update(void)
{
	VECTOR from;
	int surf, start;

	if (!gWaypointSet || NUM_NODES <= 0 || !EnsureBuffers())
	{
		GpsRoute_Clear();
		return;
	}

	from.vx = player[0].pos[0];
	from.vy = 0;
	from.vz = player[0].pos[2];

	// waypoint or level changed -> new target
	if (g_lastLevel != GameLevel ||
		g_lastWaypoint.vx != gWaypointPos.vx || g_lastWaypoint.vz != gWaypointPos.vz)
	{
		g_lastLevel = GameLevel;
		g_lastWaypoint = gWaypointPos;
		g_lastTargetNode = NearestRoadNode(&gWaypointPos);
		g_lastStartNode = -1;
	}

	if (g_lastTargetNode < 0)
		return;

	g_framesSinceCalc++;

	{
		VECTOR p = from;
		p.vy = player[0].pos[1];
		surf = GetSurfaceIndex(&p);
	}

	start = SurfToNode(surf);

	// still on the same road piece as last calculation: keep the route, move the start point
	if (start >= 0 && start == g_lastStartNode && gGpsRouteCount > 0)
	{
		gGpsRoutePts[0] = from;
		return;
	}

	// drove onto the next road piece of the route: recalculate right away so the
	// part already driven disappears (cheap: one Dijkstra per road piece)
	if (start >= 0 && g_lastStartNode >= 0 && g_onPath[start])
		g_framesSinceCalc = 1000;

	// off-road or off-route: recalculate, but not every frame
	if (g_lastStartNode >= 0 && g_framesSinceCalc < 15)
	{
		gGpsRoutePts[0] = from;
		return;
	}

	if (start < 0)
		start = NearestRoadNode(&from);

	if (start < 0)
		return;

	g_framesSinceCalc = 0;
	g_lastStartNode = start;

	if (!Calculate(start, g_lastTargetNode, &from, &gWaypointPos))
		gGpsRouteCount = 0;
}
