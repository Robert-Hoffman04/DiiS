/****************************************************************************
 * DeSmuMEWii - perf_zones.cpp   (see perf_zones.h)
 *
 * Compiled to nothing unless -DDESMUME_PERFZONES.
 ***************************************************************************/
#include "perf_zones.h"

#ifdef DESMUME_PERFZONES

#include <stdio.h>
#include <string.h>
#include <ogc/lwp_watchdog.h>   // gettime(), ticks_to_microsecs()

#include "harness/harness.h"    // §3.2: PKT_PROFILE sink (self-stubs otherwise)

u64 g_pzAcc[PZ_COUNT];
u64 g_pzHits[PZ_COUNT];

#if defined(JIT_CORE_COST_HISTO) && defined(DESMUME_JIT_ARM7)
extern "C" void jitCoreCostEmit(u32 frame);   // jit/jit_exec.cpp
#endif

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
	"dma", "draw_convert", "draw_present",
};
const char* pzName(int z) { return (z >= 0 && z < PZ_COUNT) ? k_name[z] : "?"; }

//---------------------------------------------------------------------------
// Per-block frame-time breakdown. Row every PZ_BLOCK frames:
//
//   frame,wall_us,other_us,arm9_interp_us,...,draw_us,<same block's hit counts>
//
// wall_us is the sum of all zone us for the block (== real frame wall time
// spent inside the instrumented regions); it will run a hair under the
// benchmark's own block_us because the frame-loop glue outside NDS_exec()/
// Draw() isn't zoned.
//
// Sink (§3.2): with -DDESMUME_HARNESS the row goes out as a PKT_PROFILE frame
// (harness_send routes it to the listener, or to sd:/perfzones.log via the SD
// backend when there's none). Without the harness it's written to
// sd:/perfzones.log directly, exactly as before.
//---------------------------------------------------------------------------
#ifndef DESMUME_PERFZONES_BLOCK
#define DESMUME_PERFZONES_BLOCK 60
#endif

#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)

//---------------------------------------------------------------------------
// §3.3b frame-time percentiles. A ring of the last PZ_FT_RING per-frame
// wall_us values (sampled at the pzFrameTick() site that already banks the
// interval - no extra timebase reads). Reported alongside each CSV block as
// p50/p95/p99/worst, which is what the zone-share averages hide.
//---------------------------------------------------------------------------
#define PZ_FT_RING 120
static u32  s_ftRing[PZ_FT_RING];
static u32  s_ftCount;   // total frames pushed (caps the sort window)
static u32  s_ftHead;

static void pz_ft_push(u32 wall_us)
{
	s_ftRing[s_ftHead] = wall_us;
	s_ftHead = (s_ftHead + 1u) % PZ_FT_RING;
	if (s_ftCount < PZ_FT_RING) s_ftCount++;
}

static void pz_emit_percentiles(u32 frame)
{
	u32 n = s_ftCount;
	if (!n) return;

	u32 sorted[PZ_FT_RING];
	for (u32 i = 0; i < n; i++) sorted[i] = s_ftRing[i];
	for (u32 i = 1; i < n; i++) {          // insertion sort, n <= 120
		u32 v = sorted[i], j = i;
		while (j > 0 && sorted[j - 1] > v) { sorted[j] = sorted[j - 1]; j--; }
		sorted[j] = v;
	}
	u32 p50 = sorted[(n * 50) / 100];
	u32 p95 = sorted[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
	u32 p99 = sorted[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1];
	u32 worst = sorted[n - 1];

	char line[160];
	snprintf(line, sizeof(line),
		"frametime frame=%u n=%u p50_us=%u p95_us=%u p99_us=%u worst_us=%u",
		frame, n, p50, p95, p99, worst);
	harness_profile_emit(line);
}

static void pz_emit_header(void)
{
	char line[512];
	int n = snprintf(line, sizeof(line),
		"# desmumewii perfzones  block=%d\nframe,wall_us", DESMUME_PERFZONES_BLOCK);
	for (int i = 0; i < PZ_COUNT && n > 0 && n < (int)sizeof(line); i++)
		n += snprintf(line + n, sizeof(line) - n, ",%s_us", pzName(i));
	for (int i = 0; i < PZ_COUNT && n > 0 && n < (int)sizeof(line); i++)
		n += snprintf(line + n, sizeof(line) - n, ",%s_hits", pzName(i));
	harness_profile_emit(line);
}

static void pz_emit_row(u32 frame, const u64 acc_ticks[PZ_COUNT], const u64 acc_hits[PZ_COUNT])
{
	u64 wall_us = 0;
	for (int i = 0; i < PZ_COUNT; i++) wall_us += ticks_to_microsecs(acc_ticks[i]);

	char line[512];
	int n = snprintf(line, sizeof(line), "%u,%llu", frame, (unsigned long long)wall_us);
	for (int i = 0; i < PZ_COUNT && n > 0 && n < (int)sizeof(line); i++)
		n += snprintf(line + n, sizeof(line) - n, ",%llu",
			(unsigned long long)ticks_to_microsecs(acc_ticks[i]));
	for (int i = 0; i < PZ_COUNT && n > 0 && n < (int)sizeof(line); i++)
		n += snprintf(line + n, sizeof(line) - n, ",%llu", (unsigned long long)acc_hits[i]);
	harness_profile_emit(line);
}

#else  // legacy sd:/perfzones.log sink

static inline void pz_ft_push(u32) {}
static inline void pz_emit_percentiles(u32) {}

static void pz_emit_header(void)
{
	FILE* f = fopen("sd:/perfzones.log", "w");
	if (!f) return;
	fprintf(f, "# desmumewii perfzones  block=%d\n", DESMUME_PERFZONES_BLOCK);
	fprintf(f, "frame,wall_us");
	for (int i = 0; i < PZ_COUNT; i++) fprintf(f, ",%s_us", pzName(i));
	for (int i = 0; i < PZ_COUNT; i++) fprintf(f, ",%s_hits", pzName(i));
	fprintf(f, "\n");
	fclose(f);
}

static void pz_emit_row(u32 frame, const u64 acc_ticks[PZ_COUNT], const u64 acc_hits[PZ_COUNT])
{
	u64 wall_us = 0;
	for (int i = 0; i < PZ_COUNT; i++) wall_us += ticks_to_microsecs(acc_ticks[i]);
	FILE* f = fopen("sd:/perfzones.log", "a");
	if (!f) return;
	fprintf(f, "%u,%llu", frame, (unsigned long long)wall_us);
	for (int i = 0; i < PZ_COUNT; i++)
		fprintf(f, ",%llu", (unsigned long long)ticks_to_microsecs(acc_ticks[i]));
	for (int i = 0; i < PZ_COUNT; i++)
		fprintf(f, ",%llu", (unsigned long long)acc_hits[i]);
	fprintf(f, "\n");
	fclose(f);
}

#endif

void pzFrameTick(void)
{
	static bool started = false;
	static u32  frame   = 0;
	static u64  acc_ticks[PZ_COUNT] = {0};
	static u64  acc_hits [PZ_COUNT] = {0};

	if (!started) {
		started = true;
		pz_emit_header();
		// prime the interval so the first block doesn't count startup time
		u64 t[PZ_COUNT], h[PZ_COUNT];
		pzHarvest(t, h);
	}

	frame++;
	{
		u64 t[PZ_COUNT], h[PZ_COUNT];
		pzHarvest(t, h);
		u64 frame_us = 0;
		for (int i = 0; i < PZ_COUNT; i++) {
			acc_ticks[i] += t[i]; acc_hits[i] += h[i];
			frame_us += ticks_to_microsecs(t[i]);
		}
		pz_ft_push((u32)frame_us);   // §3.3b
	}

	if (frame % DESMUME_PERFZONES_BLOCK == 0) {
		pz_emit_row(frame, acc_ticks, acc_hits);
		pz_emit_percentiles(frame);   // §3.3b
#if defined(JIT_CORE_COST_HISTO) && defined(DESMUME_JIT_ARM7)
		jitCoreCostEmit(frame);       // -DJIT_CORE_COST_HISTO per-core dispatch accounting
#endif
		for (int i = 0; i < PZ_COUNT; i++) { acc_ticks[i] = 0; acc_hits[i] = 0; }
	}
}

#endif // DESMUME_PERFZONES
