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

#if defined(JIT_CORE_COST_HISTO) && defined(DESMUME_JIT)
extern "C" void jitCoreCostEmit(u32 frame);   // jit/jit_exec.cpp
#endif

static PerfZone s_cur  = PZ_OTHER;
static u64      s_last = 0;      // timebase of the last bank(); 0 => not started

#ifdef DESMUME_PERFZONES_PMC
//---------------------------------------------------------------------------
// Task hw-measure: per-zone Broadway (PowerPC 750CL) performance-monitor counts.
//
// Source: IBM PowerPC 750CL RISC Microprocessor User's Manual v1.0 (2007-08-08),
// ch. 11 "Performance Monitor": MMCR0 = SPR 952, MMCR1 = SPR 956, PMC1..4 = SPR
// 953/954/957/958 (supervisor mtspr/mfspr; libogc runs everything in supervisor
// mode, and only touches these SPRs in PPCEarlyInit (zeroes them) and the unused
// SYS_Start/Stop/Reset/DumpPMC). MMCR0[19-25] PMC1SELECT, MMCR0[26-31] PMC2SELECT,
// MMCR1[0-4] PMC3SELECT, MMCR1[5-9] PMC4SELECT (IBM bit numbering, bit 0 = MSB).
// MMCR0's control bits (DIS/DP/DU/DMS/DMR/ENINT...) stay 0: count always, in every
// mode, no overflow interrupt. Event encodings, Tables 11-5..11-8:
//   PMC1 2 = instructions completed (not folded branches)
//   PMC2 5 = L1 I-cache misses, 7 = L2 I-side misses, 15 = I-fetch L1 miss cycles
//   PMC3 5 = L1 D-cache misses (no cache ops), 7 = L2 data misses, 15 = L1 load-miss cycles
//   PMC4 1 = processor cycles, 8 = mispredicted branches ("reserved for future use"
//        in the 750CL table: may read 0), 6 = DTLB table-search cycles
// PMC1 counts instructions in every set so each block normalizes per instruction;
// cycles are also exactly timebase * 12 on the Wii (729 MHz core, 60.75 MHz TB), so
// set 0's cycle counter doubles as a sanity check. The set rotates every block.
//
// The counters are global, not per thread: a zone also absorbs whatever other
// threads (video/audio) ran while it was current -- exactly like its time.
// Dolphin only implements the "processor cycles" select (plus loads/stores and
// FPU instructions): everything else reads 0 there.
//---------------------------------------------------------------------------
u64 g_pzPmc[PZ_COUNT][4];
static u32 s_pmcLast[4];
static u32 s_pmcSet;
#define PZ_PMC_SETS 3
static const u8 k_pmcSel[PZ_PMC_SETS][4] = {
	{ 2, 5, 5, 1 },     // instr, l1i miss, l1d miss, cycles
	{ 2, 7, 7, 8 },     // instr, l2 i-miss, l2 d-miss, branch mispredicts
	{ 2, 15, 15, 6 },   // instr, i-miss cycles, l1 load-miss cycles, dtlb search cycles
};

static inline u32 pmcRead(int i)
{
	u32 v;
	switch (i) {
	case 0: asm volatile("mfspr %0, 953" : "=r"(v)); break;
	case 1: asm volatile("mfspr %0, 954" : "=r"(v)); break;
	case 2: asm volatile("mfspr %0, 957" : "=r"(v)); break;
	default: asm volatile("mfspr %0, 958" : "=r"(v)); break;
	}
	return v;
}

// Freeze, select event set `set`, zero the counters, start.
static void pmcProgram(u32 set)
{
	const u8 *s = k_pmcSel[set];
	const u32 mmcr0 = ((u32)s[0] << 6) | s[1];
	const u32 mmcr1 = ((u32)s[2] << 27) | ((u32)s[3] << 22);
	asm volatile("mtspr 952, %0" :: "r"(0));
	asm volatile("mtspr 956, %0" :: "r"(mmcr1));
	asm volatile("mtspr 953, %0\n\tmtspr 954, %0\n\tmtspr 957, %0\n\tmtspr 958, %0" :: "r"(0));
	for (int i = 0; i < 4; i++) s_pmcLast[i] = 0;
	s_pmcSet = set;
	asm volatile("mtspr 952, %0" :: "r"(mmcr0));
}
#endif

static inline void bank(void)
{
	u64 now = gettime();
	if (s_last) g_pzAcc[s_cur] += now - s_last;
	s_last = now;
#ifdef DESMUME_PERFZONES_PMC
	// 32-bit wrapping deltas: the counters keep counting through the OV bit.
	const u32 p0 = pmcRead(0), p1 = pmcRead(1), p2 = pmcRead(2), p3 = pmcRead(3);
	u64 *a = g_pzPmc[s_cur];
	a[0] += p0 - s_pmcLast[0]; a[1] += p1 - s_pmcLast[1];
	a[2] += p2 - s_pmcLast[2]; a[3] += p3 - s_pmcLast[3];
	s_pmcLast[0] = p0; s_pmcLast[1] = p1; s_pmcLast[2] = p2; s_pmcLast[3] = p3;
#endif
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
	"a_cpu", "b_cpu", "a_gx", "b_gx", "a_bakebg", "a_bakeobj", "b_bakebg", "b_bakeobj", "bakebd",
	"readback", "gxwait", "vidlock", "g3rec", "g3replay", "g3acc", "g3prep", "g3tex",
	"g3gate", "a_3dscan", "a_3dtex",
	"efb_copy", "mbright", "a_3dgeomtex", "sched", "host",
	"j9_exec", "j9m_io", "j9m_gxfifo", "j9m_vram", "j9m_palobj", "j9m_wram", "j9m_other", "j9_fb",
	"j7_exec", "j7_mem", "j7_fb",
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

// Task hw-measure: with -DDESMUME_PERFZONES_PMC the header and every row gain
// ",pmcset" plus four columns per zone (<zone>_pmc1..4 = PMC1..PMC4 counts for the
// block); which events those are depends on the row's pmcset (the "# pmc sets"
// comment line lists them). Readers find zones by the _us/_hits suffixes.
#ifdef DESMUME_PERFZONES_PMC
#define PZ_LINE_MAX 8192
#define PZ_PMC_COMMENT "# pmc sets: 0=instr,l1imiss,l1dmiss,cycles 1=instr,l2imiss,l2dmiss,bmispred 2=instr,imisscyc,ldmisscyc,dtlbcyc\n"
static int pz_fmt_pmc_header(char *line, int n, int cap)
{
	if (n >= 0 && n < cap) n += snprintf(line + n, cap - n, ",pmcset");
	for (int i = 0; i < PZ_COUNT && n > 0 && n < cap; i++)
		n += snprintf(line + n, cap - n, ",%s_pmc1,%s_pmc2,%s_pmc3,%s_pmc4", pzName(i), pzName(i), pzName(i), pzName(i));
	return n;
}
static int pz_fmt_pmc_row(char *line, int n, int cap)
{
	if (n >= 0 && n < cap) n += snprintf(line + n, cap - n, ",%u", (unsigned)s_pmcSet);
	for (int i = 0; i < PZ_COUNT && n > 0 && n < cap; i++)
		n += snprintf(line + n, cap - n, ",%llu,%llu,%llu,%llu",
			(unsigned long long)g_pzPmc[i][0], (unsigned long long)g_pzPmc[i][1],
			(unsigned long long)g_pzPmc[i][2], (unsigned long long)g_pzPmc[i][3]);
	return n;
}
#else
#define PZ_LINE_MAX 2048
#define PZ_PMC_COMMENT ""
static inline int pz_fmt_pmc_header(char *, int n, int) { return n; }
static inline int pz_fmt_pmc_row(char *, int n, int) { return n; }
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
	static char line[PZ_LINE_MAX];
	int n = snprintf(line, sizeof(line),
		"# desmumewii perfzones  block=%d\n" PZ_PMC_COMMENT "frame,wall_us", DESMUME_PERFZONES_BLOCK);
	for (int i = 0; i < PZ_COUNT && n > 0 && n < (int)sizeof(line); i++)
		n += snprintf(line + n, sizeof(line) - n, ",%s_us", pzName(i));
	for (int i = 0; i < PZ_COUNT && n > 0 && n < (int)sizeof(line); i++)
		n += snprintf(line + n, sizeof(line) - n, ",%s_hits", pzName(i));
	n = pz_fmt_pmc_header(line, n, sizeof(line));
	harness_profile_emit(line);
}

static void pz_emit_row(u32 frame, const u64 acc_ticks[PZ_COUNT], const u64 acc_hits[PZ_COUNT])
{
	u64 wall_us = 0;
	for (int i = 0; i < PZ_COUNT; i++) wall_us += ticks_to_microsecs(acc_ticks[i]);

	static char line[PZ_LINE_MAX];
	int n = snprintf(line, sizeof(line), "%u,%llu", frame, (unsigned long long)wall_us);
	for (int i = 0; i < PZ_COUNT && n > 0 && n < (int)sizeof(line); i++)
		n += snprintf(line + n, sizeof(line) - n, ",%llu",
			(unsigned long long)ticks_to_microsecs(acc_ticks[i]));
	for (int i = 0; i < PZ_COUNT && n > 0 && n < (int)sizeof(line); i++)
		n += snprintf(line + n, sizeof(line) - n, ",%llu", (unsigned long long)acc_hits[i]);
	n = pz_fmt_pmc_row(line, n, sizeof(line));
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
	fprintf(f, PZ_PMC_COMMENT "frame,wall_us");
	for (int i = 0; i < PZ_COUNT; i++) fprintf(f, ",%s_us", pzName(i));
	for (int i = 0; i < PZ_COUNT; i++) fprintf(f, ",%s_hits", pzName(i));
	{
		static char pmc[PZ_LINE_MAX];
		pmc[0] = 0;
		pz_fmt_pmc_header(pmc, 0, sizeof(pmc));
		fputs(pmc, f);
	}
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
	{
		static char pmc[PZ_LINE_MAX];
		pmc[0] = 0;
		pz_fmt_pmc_row(pmc, 0, sizeof(pmc));
		fputs(pmc, f);
	}
	fprintf(f, "\n");
	fclose(f);
}

#endif

// Task profile: per-frame-class zone totals for the block just ended, emitted with the block's
// CSV row as "pzcls frame=<f> cls=<bits> n=<frames> wall=<us> <zone>=<us>..." (totals, not means).
u32 g_pzFrameCls;
static u64 s_clsUs[16][PZ_COUNT];
static u64 s_clsWall[16];
static u32 s_clsN[16];
static void pz_cls_add(u32 frame, const u64 t[PZ_COUNT], u64 frame_us)
{
	const u32 c = g_pzFrameCls & 15;
	g_pzFrameCls = 0;
	(void)frame;
	for (int i = 0; i < PZ_COUNT; i++) s_clsUs[c][i] += ticks_to_microsecs(t[i]);
	s_clsWall[c] += frame_us;
	++s_clsN[c];
}
static void pz_cls_emit(u32 frame)
{
	for (int c = 0; c < 16; c++) {
		if (!s_clsN[c]) continue;
		char line[1600];
		int n = snprintf(line, sizeof(line), "pzcls frame=%u cls=%d n=%u wall=%llu", frame, c, s_clsN[c], (unsigned long long)s_clsWall[c]);
		for (int i = 0; i < PZ_COUNT && n > 0 && n < (int)sizeof(line); i++)
			n += snprintf(line + n, sizeof(line) - n, " %s=%llu", pzName(i), (unsigned long long)s_clsUs[c][i]);
		harness_profile_emit(line);
		for (int i = 0; i < PZ_COUNT; i++) s_clsUs[c][i] = 0;
		s_clsWall[c] = 0; s_clsN[c] = 0;
	}
}

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
#ifdef DESMUME_PERFZONES_PMC
		pmcProgram(0);
		memset(g_pzPmc, 0, sizeof(g_pzPmc));
#endif
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
		pz_cls_add(frame, t, frame_us);
	}

	if (frame % DESMUME_PERFZONES_BLOCK == 0) {
		pz_emit_row(frame, acc_ticks, acc_hits);
		pz_emit_percentiles(frame);   // §3.3b
		pz_cls_emit(frame);
#if defined(JIT_CORE_COST_HISTO) && defined(DESMUME_JIT)
		jitCoreCostEmit(frame);       // -DJIT_CORE_COST_HISTO per-core dispatch accounting
#endif
		for (int i = 0; i < PZ_COUNT; i++) { acc_ticks[i] = 0; acc_hits[i] = 0; }
#ifdef DESMUME_PERFZONES_PMC
		// next event set; the counts banked since the last pzHarvest() (the row
		// emission itself) are dropped along with the old set.
		pmcProgram((s_pmcSet + 1) % PZ_PMC_SETS);
		memset(g_pzPmc, 0, sizeof(g_pzPmc));
#endif
	}
}

#endif // DESMUME_PERFZONES
