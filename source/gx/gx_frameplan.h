/*
    gx_frameplan.h - Stage 0 (Frame Analysis), see nds-wii-render-pipeline.md.

    Not a snapshot taken before the frame starts: built from a trace of
    dirty memory ranges and register writes accumulated as the frame's CPU
    cycles run (HDraw/HBlank per scanline), finalized once per frame,
    before the first GX call for that frame. This file is the pure,
    engine-agnostic data structures only (dirty-page bitmap, scanline-band
    boundary tracker, hot-feature flags) - host-testable with no NDS/GBA/GX
    dependency. Wiring these into actual write sites is emulator-specific
    glue layered on top; see source/MMU.cpp's GBA VRAM/palette/OAM write
    funnel and source/gba_ppu.cpp's I/O register write funnel for the first
    (GBA PPU vertical-slice) instance of that wiring.

    Design decision carried over from the doc: the dirty-range trace here
    is the *sole* dirty-tracking mechanism Stage 1 (resource sync) reads -
    there is no separate dirty-flag subsystem alongside it.
*/
#ifndef GX_FRAMEPLAN_H
#define GX_FRAMEPLAN_H

#include "types.h"

// ---------------------------------------------------------------------
// Dirty-page bitmap: one bit per `pageSize`-byte page of a fixed-size
// linear region (VRAM, palette RAM, OAM, ...). One type serves every
// region this engine tracks (GBA's 96KB VRAM, 1KB palette, 1KB OAM today;
// the DS's larger banks later) by parameterizing region/page size at
// init() rather than hardcoding either.
// ---------------------------------------------------------------------
class GxDirtyBitmap {
public:
	// kMaxPages bounds every region/pageSize combination this engine
	// actually uses (see call sites) with comfortable headroom; init()
	// asserts (via clamping to 0 pages, i.e. a no-op tracker) rather than
	// overrunning m_bits if a future caller picks too fine a granularity.
	static const u32 kMaxPages = 1024;

	void init(u32 regionSize, u32 pageSize);
	void clear();
	void markRange(u32 offset, u32 size);
	bool isPageDirty(u32 pageIndex) const;
	u32 pageCount() const { return m_pageCount; }
	u32 pageSize() const { return m_pageSize; }
	bool anyDirty() const { return m_anyDirty; }

private:
	u32 m_bits[kMaxPages / 32] = {};
	u32 m_pageCount = 0;
	u32 m_pageSize = 0;
	bool m_anyDirty = false;
};

// ---------------------------------------------------------------------
// Scanline-band boundary tracking: which scanlines saw an explicit,
// layout-affecting register rewrite this frame. finalize() turns that
// boundary set into the doc's "contiguous scanline ranges sharing
// identical 2D state" band list. Continuous per-scanline hardware
// auto-increment (e.g. affine reference-point accumulation) is not a
// boundary source - only an explicit CPU rewrite is, since the
// auto-increment case is handled by updating a texture-matrix uniform,
// not by re-banding (see nds-wii-render-pipeline.md Stage 4).
// ---------------------------------------------------------------------
class GxBandTracker {
public:
	static const int kMaxBoundaries = 64;
	static const int kMaxBands = kMaxBoundaries + 1;

	void beginFrame();

	// Records a band boundary at `line` (a register write landed here that
	// changes how a later scanline in this same frame must be drawn
	// differently from this one). Boundaries at line 0 are redundant (the
	// frame start is always the first band's start) and are ignored.
	// Silently drops boundaries past kMaxBoundaries - see the .cpp comment
	// for why that's a safe degrade, not a correctness bug.
	void recordChangeAtLine(int line);

	// Builds the [start,end) scanline ranges for a `totalLines`-line
	// frame from the boundaries recorded this frame. Returns the band
	// count (always >= 1); `outStarts`/`outEnds` must each have room for
	// kMaxBands entries.
	int finalize(int totalLines, int outStarts[], int outEnds[]) const;

	int boundaryCount() const { return m_count; }

private:
	int m_boundaries[kMaxBoundaries] = {};
	int m_count = 0;
};

// ---------------------------------------------------------------------
// Hot-feature flags: which optional Stage 4 (and, for DS Engine A later,
// Stage 3/3.5) subsystems are active this frame. Set by whichever code
// already understands the relevant register semantics (kept out of this
// file to keep it engine-agnostic - see gba_ppu.cpp's classifier for the
// GBA PPU instance), read by the draw-plan consumer to skip inactive
// subsystems' work entirely.
// ---------------------------------------------------------------------
enum GxHotFeature {
	GXHOT_OBJWIN = 0,
	GXHOT_MOSAIC,
	GXHOT_BLEND,
	GXHOT_SHADOW,    // DS Engine A only - not wired until the 3D-layer slice
	GXHOT_FOG,       // DS Engine A only - not wired until the 3D-layer slice
	GXHOT_EDGEMARK,  // DS Engine A only - not wired until the 3D-layer slice
	GXHOT_CAPTURE,   // DS Engine A only - not wired until the 3D-layer slice
	GXHOT_COUNT
};

class GxFramePlan {
public:
	// Clears the accumulated trace (dirty bitmaps, band boundaries, hot
	// flags) for a new frame. Call once at the start of the frame's CPU
	// execution - everything accumulated before the *next* call is this
	// frame's trace, ready for Stage 1/the band consumer to read once the
	// frame's CPU cycles finish and before the first GX call.
	void beginFrame();

	void setHot(GxHotFeature f, bool v) { m_hot[f] = v; }
	bool isHot(GxHotFeature f) const { return m_hot[f]; }

	GxDirtyBitmap vram;
	GxDirtyBitmap palette;
	GxDirtyBitmap oam;
	GxBandTracker bands;

private:
	bool m_hot[GXHOT_COUNT] = {};
};

#endif // GX_FRAMEPLAN_H
