/*
    Copyright (C) 2026 DeSmuMEWii team

    This file is part of DeSmuMEWii

    DeSmuMEWii is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    DeSmuMEWii is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DeSmuMEWii; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

//------------------------------------------------------------------------------
// GX2DBG - MAIN-engine text BG layers as GX-sampled textures (Step 5.1a).
//
// Each armed text BG layer is baked into a "resolved plane" texture in its
// native size (BGSize, 256/512 px, always pow2): the tilemap resolved against
// tileset + palette, one GX RGB5A3 texel per BG pixel, 0x0000 == transparent.
// The bake happens on the core thread inside GXMerge_Present and only when the
// GXDirty epochs say the backing VRAM/palette actually changed.  GX then draws
// the plane every frame as a GX_REPEAT-wrapped quad with (HOFS,VOFS) folded
// into the texcoord matrix - the hardware does the (x+hofs)&mask wrap and the
// composite/blend that the CPU per-pixel path used to.
//
// THREADING: bake + descriptor update run on the core thread (GXMerge_Present,
// under vidmutex).  The draw thread only reads the latched GXTexObj.
//------------------------------------------------------------------------------

#ifndef GX2DBG_H
#define GX2DBG_H

#include <gctypes.h>
#include <gccore.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GPU;

// Total MEM1 the baked planes may claim (a layer that would exceed it stays on
// the CPU path).  4 * 512*512*2 == 2 MB worst case; cap below that.
#define GX2DBG_BUDGET (1536 * 1024)

// Called from GXMerge_Set2DBG().  false teardown frees every plane.
void GX2DBG_SetEnabled(bool on);
bool GX2DBG_Enabled(void);

// Core thread, at each engine's line 0: re-bake any armed text BG layer of that
// engine (eng = gpu->core) whose source bytes changed.  Cheap when nothing is
// dirty.
void GX2DBG_FrameUpdate(struct GPU *gpu);

// Core thread, once per frame after both engines' FrameUpdate: clears the 5.0
// per-page dirty flags (clear-on-consume epoch).
void GX2DBG_EndFrame(void);

// Is engine `eng` (0 MAIN / 1 SUB) layer `num` (0..3) a valid baked plane now?
bool GX2DBG_LayerReady(int eng, int num);

// Latched texture object for a ready layer (draw thread).  w/h out = plane size.
GXTexObj *GX2DBG_LayerTex(int eng, int num, u16 *w, u16 *h);

// Step 5.2: non-affine tiled sprites.  Core thread at line 0.
void GX2DBG_ObjFrameUpdate(struct GPU *gpu);
// Every enabled OBJ on this engine is a bakeable front sprite (else CPU path).
// Recorder gate - reads this frame's freshly built list (core thread).
bool GX2DBG_ObjGXable(int eng);
// Core thread, GXMerge_Present: latch the sprite list for the draw thread.
void GX2DBG_ObjLatch(void);
int  GX2DBG_ObjCount(int eng);
// Draw thread: latched sprite i (0..count-1, OAM order) - tex + rect + flags.
GXTexObj *GX2DBG_ObjGet(int eng, int i, s16 *x, s16 *y, u16 *w, u16 *h,
                        u8 *prio, u8 *semi, u8 *hflip, u8 *vflip);

// Machine reset / video teardown.
void GX2DBG_Reset(void);

#ifdef __cplusplus
}
#endif

#endif  // GX2DBG_H
