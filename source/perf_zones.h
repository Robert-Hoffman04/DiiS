/****************************************************************************
 * DeSmuMEWii - perf_zones.h
 *
 * A tiny wall-clock "where does the frame go" accountant, built for
 * optimization work on the trace JIT. It splits each emulated frame's CPU
 * time between a fixed set of zones (interpreter / JIT execute / JIT build /
 * geometry engine / 3D render / the GX 3D and 2D passes piece by piece / SPU /
 * GX present / sequencer / other) and -DDESMUME_BENCH's frame loop dumps the
 * running totals to sd:/perfzones.log. tools/benchmark/pztable.py prints them.
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
	PZ_GPU_RENDER,    // gpu3D->NDS_3D_Render() - software rasterizer
	PZ_GPU_2D,        // hblank 2D residual: DS Stage-0 scanline hooks (CPU lines are a_cpu/b_cpu); GBA PPU
	PZ_SPU,           // SPU_Emulate_core()
	PZ_DRAW,          // Draw() residual - VI present / vidmutex (convert+present split out below)
	PZ_DMA,           // DmaController::exec() - all 8 channels (was folded into 'other')
	PZ_DRAW_CONVERT,  // Draw() 4x4-swizzle RGB15_REVERSE of both screens + DCFlushRange
	PZ_DRAW_PRESENT,  // draw_thread() GX present - quad draw calls / TEV / scissor
	// Task profile / Task pzones: finer split of gpu_2d / the GX passes (every
	// significant GX-rewrite cost is its own zone; see tools/benchmark/pztable.py).
	PZ_2DA_CPU,       // GPU_RenderLine(Main) at hblank + Engine A lazy-flush replays
	PZ_2DB_CPU,       // GPU_RenderLine(Sub)  at hblank + Engine B lazy-flush replays
	PZ_2DA_GX,        // gxDsEngineARenderFrame() residual: plan, gates, band draw submission
	PZ_2DB_GX,        // gxDsEngineBRenderFrame() residual
	PZ_2DA_BAKE_BG,   // Engine A BG plane bakes + TLUT-only rebuilds
	PZ_2DA_BAKE_OBJ,  // Engine A OBJ bakes + TLUT-only rebuilds
	PZ_2DB_BAKE_BG,
	PZ_2DB_BAKE_OBJ,
	PZ_2D_BAKE_BD,    // backdrop bakes (both engines)
	PZ_2D_READBACK,   // GPU_screen readback of a GX copy: lazy present resolve + eager MASTER_BRIGHT fallback
	PZ_GX_WAIT,       // GX_DrawDone() in the compositor (GPU drain, Dolphin-timed)
	PZ_VIDLOCK,       // LWP_MutexLock(vidmutex) wait in the compositor
	PZ_GX3D_REC,      // gxDs3dRenderFast() recording (or unrecorded) pass
	PZ_GX3D_REPLAY,   // gxDs3dRenderFast() display-list replay
	PZ_GX3D_ACC,      // gxDs3dRenderAccurate()
	PZ_GX3D_PREP,     // gxDs3dGeomFramePrepare() at VBlank end (gate + plan), textures split out
	PZ_GX3D_TEX,      // gxDs3dPrepareTextures() texture conversion / upload
	PZ_GX3D_GATE,     // gxDs3dGeomFrameSupported() line-191 gate
	PZ_2DA_3DSCAN,    // gxDsA3dScan(): CPU 3D layer scan (line holes / alpha)
	PZ_2DA_3DTEX,     // gxDsA3dEnsureTex(): the CPU 3D layer's texture bake
	PZ_EFB_COPY,      // compositor EFB->texture copy + present-state restore
	PZ_MBRIGHT,       // gxDsPresentMasterBright() GPU MASTER_BRIGHT pass
	PZ_2DA_3DGEOMTEX, // gxDsA3dRenderGeomTex(): GX 3D pass drawn to a texture (a3 bake)
	PZ_SCHED,         // NDS_exec() loop: IRQ dispatch, findNext(), hstart/hblank glue, armInnerLoop() residual
	PZ_HOST,          // frame-loop glue outside NDS_exec()/Draw(): input, harness, bench tick
	// JIT split: carved out of arm9_jit / arm7_jit, which keep only the dispatcher
	// (block lookup, compile trigger, pipeline re-prime). *_exec is time inside
	// ExecuteJITTrace() (compiled code + trampoline); the slow-memory C calls and
	// in-block interpreter fallbacks made from compiled code are split out of it.
	PZ_JIT9_EXEC,     // ARM9 compiled code + trampoline
	PZ_JIT9_M_IO,     // ARM9 slowRead/slowWrite: I/O (0x04, not GX FIFO ports)
	PZ_JIT9_M_GXFIFO, // ARM9 slowRead/slowWrite: GX command ports 0x04000400-0x040005FF
	PZ_JIT9_M_VRAM,   // ARM9 slowRead/slowWrite: VRAM (0x06)
	PZ_JIT9_M_PALOBJ, // ARM9 slowRead/slowWrite: palette / OAM (0x05, 0x07)
	PZ_JIT9_M_WRAM,   // ARM9 slowRead/slowWrite: shared WRAM (0x03)
	PZ_JIT9_M_OTHER,  // ARM9 slowRead/slowWrite: anything else (ITCM stores, main RAM straddles, BIOS, slot-2)
	PZ_JIT9_FB,       // ARM9 in-block interpreter fallback (jitInterpFallback)
	PZ_JIT7_EXEC,     // ARM7 compiled code + trampoline
	PZ_JIT7_MEM,      // ARM7 slowRead/slowWrite (all regions)
	PZ_JIT7_FB,       // ARM7 in-block interpreter fallback
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

// Task profile: which kind of 3D frame this emulated frame was (bits, OR'd by the 3D code);
// pzFrameTick() buckets each frame's zone time by it. 1 GxFast record, 2 GxFast replay,
// 4 CPU raster ran, 8 GxAccurate pass.
extern u32 g_pzFrameCls;
#define PZ_SUB_SCOPE(z)  PZ_SCOPE(z)
#define PZ_SUB_CLS(b)    (g_pzFrameCls |= (b))

#else  // !DESMUME_PERFZONES

static inline PerfZone pzGet(void)        { return PZ_OTHER; }
static inline void     pzSet(PerfZone)    {}
static inline void     pzFrameTick(void)  {}
#define PZ_SCOPE(z)     ((void)0)

#endif // DESMUME_PERFZONES

#ifndef PZ_SUB_SCOPE
#define PZ_SUB_SCOPE(z)  ((void)0)
#define PZ_SUB_CLS(b)    ((void)0)
#endif

#endif // DESMUME_PERF_ZONES_H
