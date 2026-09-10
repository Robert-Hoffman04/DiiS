// Lightweight on-screen FPS counter, drawn as GX textured quads in the
// top-left corner (outside the centred DS screen area). See fps_overlay.cpp.
#ifndef FPS_OVERLAY_H
#define FPS_OVERLAY_H

// Build the glyph atlas texture. Call once, after GX_Init() and the rest of
// the GX pipeline setup in init().
void FPSOverlay_Init();

// Call exactly once per emulated NDS frame (from DSExec()) to advance the
// rolling frame-rate measurement.
void FPSOverlay_Tick();

// Draw the counter. Call from draw_thread() while the video mutex is held,
// after the DS screen quads and before GX_DrawDone().
void FPSOverlay_Draw();

#endif // FPS_OVERLAY_H
