/*
    gx_rendermode.h - central runtime render-mode switch (nds-wii-render-pipeline.md,
    "Target end state: three runtime-switchable render modes").

    Single small global, not per-engine state: all three GX engine instances (GBA PPU,
    DS Engine A, DS Engine B) read the same mode.

      RenderMode::Software   - every engine's top-level GX entry point bails before any
                                GX work, so the existing CPU compositor (renderScanline() /
                                GPU_RenderLine()) renders every frame, uniformly across all
                                three engines. Runtime equivalent of building with both
                                GBA_FORCE_CPU and DSA_FORCE_CPU/DSB_FORCE_CPU at once -- but
                                those compile-time flags stay as-is (dev/test A/B tools),
                                this is the field-selectable version.
      RenderMode::GxAccurate - DEFAULT. Today's shipped behaviour, unchanged, byte-for-byte:
                                every GX path stays exact-or-bail, falling back to the CPU
                                compositor per-frame/per-layer exactly as it already does.
                                Nothing about this mode's code paths changed by the switch's
                                introduction -- gxRenderModeIsFast() reads false and every
                                gated call site takes its pre-existing bail branch.
      RenderMode::GxFast      - same GX paths, but individually-characterized exact-or-bail
                                gates may loosen to stay on GX instead of bailing to CPU,
                                accepting a small documented error (e.g. task 13d's +-1 LSB
                                translucent-3D-over-2nd-target blend rounding, gated at this
                                switch since this task). Each such loosened case must cite
                                its own log section characterizing exactly what/how much
                                changes -- this switch does not decide which gaps qualify,
                                it only gives them a place to plug in.

    No on-screen UI exists yet (nothing to hook a menu into); for now the only way to
    change this at runtime is the harness PKT_CTRL "rendermode software|accurate|fast"
    command (harness_input.{h,cpp}), intended for test/bench scripts
    (tools/benchmark/capture.py --ctrl-cmd). Real UI wiring is future work.
*/
#ifndef GX_RENDERMODE_H
#define GX_RENDERMODE_H

enum class RenderMode {
	Software,   // CPU compositor only, every engine, no GX work at all
	GxAccurate, // default: today's exact-or-bail behaviour, unchanged
	GxFast,     // GX paths stay engaged through individually-characterized inexact cases
};

RenderMode gxRenderMode();
void gxSetRenderMode(RenderMode mode);

// Convenience for call sites that only care about the Fast-vs-not distinction (the vast
// majority of gated call sites: they don't need to distinguish Software from GxAccurate,
// both of which mean "take the exact-or-bail branch").
static inline bool gxRenderModeIsFast()
{
	return gxRenderMode() == RenderMode::GxFast;
}

static inline bool gxRenderModeIsSoftware()
{
	return gxRenderMode() == RenderMode::Software;
}

#endif // GX_RENDERMODE_H
