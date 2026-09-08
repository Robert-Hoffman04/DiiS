/****************************************************************************
 * DeSmuMEWii - perf_zones.cpp   (see perf_zones.h)
 *
 * Compiled to nothing unless -DDESMUME_PERFZONES.
 ***************************************************************************/
#include "perf_zones.h"

#ifdef DESMUME_PERFZONES

#include <stdio.h>
#include <ogc/lwp_watchdog.h>   // gettime(), ticks_to_microsecs()

u64 g_pzAcc[PZ_COUNT];
u64 g_pzHits[PZ_COUNT];

static PerfZone s_cur  = PZ_OTHER;
static u64      s_last = 0;      // timebase of the last bank(); 0 => not started

static inline void bank(void)
{
	u64 now = gettime();
	if (s_last) g_pzAcc[s_cur] += now - s_last;
	s_last = now;
}

PerfZone pzGet(void) { return s_cur; }

void pzSet(PerfZone z)
{
	if (z == s_cur) return;     // the common case: no timebase read
	bank();
	s_cur = z;
	g_pzHits[z]++;
}

void pzHarvest(u64 out_ticks[PZ_COUNT], u64 out_hits[PZ_COUNT])
{
	bank();                     // close the in-flight interval
	for (int i = 0; i < PZ_COUNT; i++) {
		out_ticks[i] = g_pzAcc[i];  g_pzAcc[i]  = 0;
		out_hits[i]  = g_pzHits[i];  g_pzHits[i] = 0;
	}
}

static const char* k_name[PZ_COUNT] = {
	"other",
	"arm9_interp", "arm9_jit", "arm9_build",
	"arm7_interp", "arm7_jit", "arm7_build",
	"gpu_ge", "gpu_render", "gpu_2d",
	"spu", "draw",
};
const char* pzName(int z) { return (z >= 0 && z < PZ_COUNT) ? k_name[z] : "?"; }

//---------------------------------------------------------------------------
// Per-frame dump to sd:/perfzones.log. Row every PZ_BLOCK frames:
//
//   frame,wall_us,other_us,arm9_interp_us,...,draw_us,<same block's hit counts>
//
// wall_us is the sum of all zone us for the block (== real frame wall time
// spent inside the instrumented regions); it will run a hair under the
// benchmark's own block_us because the frame-loop glue outside NDS_exec()/
// Draw() isn't zoned.
//---------------------------------------------------------------------------
#ifndef DESMUME_PERFZONES_BLOCK
#define DESMUME_PERFZONES_BLOCK 60
#endif

void pzFrameTick(void)
{
	static bool started = false;
	static u32  frame   = 0;
	static u64  acc_ticks[PZ_COUNT] = {0};
	static u64  acc_hits [PZ_COUNT] = {0};

	if (!started) {
		started = true;
		FILE* f = fopen("sd:/perfzones.log", "w");
		if (f) {
			fprintf(f, "# desmumewii perfzones  block=%d\n", DESMUME_PERFZONES_BLOCK);
			fprintf(f, "frame,wall_us");
			for (int i = 0; i < PZ_COUNT; i++) fprintf(f, ",%s_us", pzName(i));
			for (int i = 0; i < PZ_COUNT; i++) fprintf(f, ",%s_hits", pzName(i));
			fprintf(f, "\n");
			fclose(f);
		}
		// prime the interval so the first block doesn't count startup time
		u64 t[PZ_COUNT], h[PZ_COUNT];
		pzHarvest(t, h);
	}

	frame++;
	{
		u64 t[PZ_COUNT], h[PZ_COUNT];
		pzHarvest(t, h);
		for (int i = 0; i < PZ_COUNT; i++) { acc_ticks[i] += t[i]; acc_hits[i] += h[i]; }
	}

	if (frame % DESMUME_PERFZONES_BLOCK == 0) {
		u64 wall_us = 0;
		for (int i = 0; i < PZ_COUNT; i++) wall_us += ticks_to_microsecs(acc_ticks[i]);
		FILE* f = fopen("sd:/perfzones.log", "a");
		if (f) {
			fprintf(f, "%u,%llu", frame, (unsigned long long)wall_us);
			for (int i = 0; i < PZ_COUNT; i++)
				fprintf(f, ",%llu", (unsigned long long)ticks_to_microsecs(acc_ticks[i]));
			for (int i = 0; i < PZ_COUNT; i++)
				fprintf(f, ",%llu", (unsigned long long)acc_hits[i]);
			fprintf(f, "\n");
			fclose(f);
		}
		for (int i = 0; i < PZ_COUNT; i++) { acc_ticks[i] = 0; acc_hits[i] = 0; }
	}
}

#endif // DESMUME_PERFZONES
