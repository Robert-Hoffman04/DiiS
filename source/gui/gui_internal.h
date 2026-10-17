/****************************************************************************
 * DiiS - gui_internal.h
 *
 * Shared between the GUI's own files (gui_draw.cpp, gui_text.cpp,
 * gui_screen.cpp); screens use gui.h only.
 ***************************************************************************/
#ifndef DIIS_GUI_INTERNAL_H
#define DIIS_GUI_INTERNAL_H

#include "gui.h"

#define GUI_VTXFMT GX_VTXFMT7   // unused by the emulator's GX paths

// Texture bind. Mask textures (I4/I8: coverage) take their colour from the
// vertex and alpha = texture * vertex; RGBA textures are modulated by it.
void guiBindMask(GXTexObj *tex);
void guiBindRgba(GXTexObj *tex);

GuiColor guiApplyAlpha(GuiColor c);    // global alpha folded in

static inline void guiVtx(f32 x, f32 y, GuiColor c, f32 s, f32 t)
{
	GX_Position2f32(x, y);
	GX_Color4u8(c.r, c.g, c.b, c.a);
	GX_TexCoord2f32(s, t);
}

// Inside a GX_Begin(GX_QUADS, GUI_VTXFMT, 4 * n).
static inline void guiQuadVerts(f32 x0, f32 y0, f32 x1, f32 y1, GuiColor c, f32 s0, f32 t0, f32 s1, f32 t1)
{
	guiVtx(x0, y0, c, s0, t0);
	guiVtx(x1, y0, c, s1, t0);
	guiVtx(x1, y1, c, s1, t1);
	guiVtx(x0, y1, c, s0, t1);
}

void guiTextInit();
void guiPollInput();

#endif // DIIS_GUI_INTERNAL_H
