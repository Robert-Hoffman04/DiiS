/****************************************************************************
 * DeSmuMEWii - perf_zones.h
 *
 * A tiny wall-clock "where does the frame go" accountant, built for
 * optimization work on the trace JIT. It splits each emulated frame's CPU
 * time between a fixed set of zones (interpreter / JIT execute / JIT build /
 * geometry engine / 3D render / 2D compositor / SPU / GX present / other) and
 * -DDESMUME_BENCH's frame loop dumps the running totals to sd:/perfzones.log.
 *
 * Model: a single "current zone" plus one save slot per call site. pzSet(z)
 * only reads the Wii timebase when the zone actually *changes*, so a run of a
 * million consecutive same-zone dispatches costs one branch each, not a
 * timebase read each. Nesting is handled by the caller saving pzGet() before
 * pzSet() and restoring it after (see PzScope, and jitCompileTrace()).
 *
 * Entirely compiled out unless -DDESMUME_PERFZONES is defined (the benchmark's
 * `profile` mode sets it; see tools/benchmark/). Zero cost, zero symbols
 * otherwise.
 ***************************************************************************/
#ifndef DESMUME_PERF_ZONES_H
#define DESMUME_PERF_ZONES_H

#include "types.h"

enum PerfZone {
	PZ_OTHER = 0,     // sequencer, DMA, MMU housekeeping, IRQ dispatch, glue
	PZ_ARM9_INTERP,   // armcpu_exec<ARMCPU_ARM9>()  (interpreter fallback)
	PZ_ARM9_JIT,      // jitRunArm9(): dispatch + trampoline + ExecuteJITTrace
	PZ_ARM9_BUILD,    // jitCompileTrace() for the ARM9 cache
	PZ_ARM7_INTERP,   // armcpu_exec<ARMCPU_ARM7>()
	PZ_ARM7_JIT,      // jitRunArm7()
	PZ_ARM7_BUILD,    // jitCompileTrace() for the ARM7 cache
	PZ_GPU_GE,        // gfx3d_execute3D() - geometry-engine command FIFO
	PZ_GPU_RENDER,    // gpu3D->NDS_3D_Render() - GXRender / software rasterizer
	PZ_GPU_2D,        // GPU_RenderLine() x2 - 2D compositor / merge line walk
	PZ_SPU,           // SPU_Emulate_core()
	PZ_DRAW,          // Draw() - screen convert + GXMerge_Present + VI present
	PZ_COUNT
};

#ifdef DESMUME_PERFZONES

// running totals since the last pzHarvest(), in Wii-timebase ticks
extern u64 g_pzAcc[PZ_COUNT];
// count of transitions *into* each zone since the last pzHarvest()
extern u64 g_pzHits[PZ_COUNT];

PerfZone pzGet(void);
void     pzSet(PerfZone z);           // bank elapsed to the old zone iff z changed

// snapshot the totals into caller storage and zero them; also banks the
// in-flight interval so the snapshot is complete up to the call.
void pzHarvest(u64 out_ticks[PZ_COUNT], u64 out_hits[PZ_COUNT]);

const char* pzName(int z);

// -DDESMUME_BENCH frame loop calls this once per emulated frame; every 60th
// call it appends a CSV row (ticks, converted to us) to sd:/perfzones.log.
void pzFrameTick(void);

// RAII zone switch that restores the previous zone on scope exit. Used at
// call sites that are entered from more than one parent zone (the GPU/SPU/
// Draw hooks, which run from the sequencer with an ARM zone still current).
struct PzScope {
	PerfZone prev;
	explicit PzScope(PerfZone z) : prev(pzGet()) { pzSet(z); }
	~PzScope() { pzSet(prev); }
};
#define PZ_CONCAT_(a, b) a##b
#define PZ_CONCAT(a, b)  PZ_CONCAT_(a, b)
#define PZ_SCOPE(z)      PzScope PZ_CONCAT(pz_scope_, __LINE__)(z)

#else  // !DESMUME_PERFZONES

static inline PerfZone pzGet(void)        { return PZ_OTHER; }
static inline void     pzSet(PerfZone)    {}
static inline void     pzFrameTick(void)  {}
#define PZ_SCOPE(z)     ((void)0)

#endif // DESMUME_PERFZONES

#endif // DESMUME_PERF_ZONES_H
