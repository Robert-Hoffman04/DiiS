#include "gx_ds_3d_render.h"
#include "gx_rendermode.h"
#include "gx_swizzle.h"
#include "../gfx3d.h"
#include "../MMU.h"
#include <gccore.h>
#include <malloc.h>
#include <math.h>
#include <string.h>
#ifdef DSA_GXGEOM_DEBUGWHY
#include "../harness/harness.h"
#include "../harness/harness_profile.h"
#endif

extern MMU_struct MMU;

// DS screen dimensions -- duplicated from gx_ds_engine_impl.inc's kDsBScreenW/H (not
// visible from here; both are just the fixed 256x192 DS screen size, not expected to
// ever differ) rather than pulling in that whole translation unit's static state.
static const int kScreenW = 256;
static const int kScreenH = 192;
// GX samples pixel centres, the DS rasterizer pixel corners (see gxDs3dRenderAccurate).
static const float kGxDs3dSampleOffset = 0.5f;

// POLYGON_ATTR shading mode, bits 4-5: 0 modulate, 1 decal, 2 toon/highlight, 3 shadow.
static inline int gxDs3dPolyMode(const POLY &p) { return (int)((p.polyAttr >> 4) & 3); }
// TEXIMAGE_PARAM format, bits 26-28 (see POLY::isTranslucent's own use of this same shift).
static inline int gxDs3dTexFormat(const POLY &p) { return (int)((p.texParam >> 26) & 7); }

// GxFast transform for one polygon. GX_LoadProjectionMtx keeps only 6 entries and
// hard-wires w = -z_in (perspective) or w = 1 (orthographic), and GX's clip-space Z
// range is [-w, 0] where the DS's is [-w, w]. So a general DS projection P can't be
// loaded as-is. Instead P's x, y and w rows are folded into the (affine) position
// matrix together with the modelview, and GX gets a canonical projection that only
// remaps depth: z_gx/w = (z_ds/w - 1)/2, which makes GX's depth equal the DS's
// (nz+1)/2, i.e. the same value gxDs3dRenderAccurate() feeds. Works for any
// P = perspective * affine; anything else is not representable and bails.
struct GxDs3dFastXform {
	Mtx pos;
	Mtx44 proj;
	u8 projType;
};

// DS matrices are column-major: element (row r, col c) = m[4*c + r].
static inline float gxDs3dM(const float *m, int r, int c) { return m[4 * c + r]; }

static bool gxDs3dFastXformBuild(const POLY &p, GxDs3dFastXform &x)
{
	const float *P = p.projMatrix, *MV = p.mvMatrix;
	const float eps = 1e-4f;
	if (fabsf(gxDs3dM(MV, 3, 0)) > eps || fabsf(gxDs3dM(MV, 3, 1)) > eps ||
	    fabsf(gxDs3dM(MV, 3, 2)) > eps || fabsf(gxDs3dM(MV, 3, 3) - 1.0f) > eps)
		return false;

	float M[3][4];
	memset(x.proj, 0, sizeof(x.proj));
	const bool ortho = fabsf(gxDs3dM(P, 3, 0)) <= eps && fabsf(gxDs3dM(P, 3, 1)) <= eps &&
	                   fabsf(gxDs3dM(P, 3, 2)) <= eps;
	if (ortho) {
		const float w = gxDs3dM(P, 3, 3);
		if (fabsf(w) <= eps) return false;
		for (int c = 0; c < 4; ++c) {
			M[0][c] = gxDs3dM(P, 0, c) / w;
			M[1][c] = gxDs3dM(P, 1, c) / w;
			M[2][c] = gxDs3dM(P, 2, c) / w;
		}
		x.proj[0][0] = 1; x.proj[1][1] = 1;
		x.proj[2][2] = 0.5f; x.proj[2][3] = -0.5f;
		x.proj[3][3] = 1;
		x.projType = GX_ORTHOGRAPHIC;
	} else {
		// P2 must equal alpha*P3 + beta*e_w (depth affine in w).
		int k = 0;
		for (int c = 1; c < 3; ++c)
			if (fabsf(gxDs3dM(P, 3, c)) > fabsf(gxDs3dM(P, 3, k))) k = c;
		const float alpha = gxDs3dM(P, 2, k) / gxDs3dM(P, 3, k);
		float scale = 0;
		for (int c = 0; c < 3; ++c) scale = fmaxf(scale, fabsf(gxDs3dM(P, 2, c)));
		for (int c = 0; c < 3; ++c)
			if (fabsf(gxDs3dM(P, 2, c) - alpha * gxDs3dM(P, 3, c)) > eps * fmaxf(1.0f, scale))
				return false;
		const float beta = gxDs3dM(P, 2, 3) - alpha * gxDs3dM(P, 3, 3);
		for (int c = 0; c < 4; ++c) {
			M[0][c] = gxDs3dM(P, 0, c);
			M[1][c] = gxDs3dM(P, 1, c);
			M[2][c] = -gxDs3dM(P, 3, c);   // z_in = -w_ds, so GX's w = -z_in = w_ds
		}
		x.proj[0][0] = 1; x.proj[1][1] = 1;
		x.proj[2][2] = (1.0f - alpha) * 0.5f;
		x.proj[2][3] = beta * 0.5f;
		x.proj[3][2] = -1;
		x.projType = GX_PERSPECTIVE;
	}
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 4; ++c) {
			float s = (c == 3) ? M[r][3] : 0.0f;
			for (int k = 0; k < 3; ++k)
				s += M[r][k] * gxDs3dM(MV, k, c);
			x.pos[r][c] = s;
		}
	return true;
}

bool gxDs3dGeomFrameSupported()
{
	if (!gfx3d.polylist || !gfx3d.vertlist) {
#ifdef DSA_GXGEOM_DEBUGWHY
		harness_profile_emitf("gxds3dwhy nolist");
#endif
		return false;
	}
	const int polycount = gfx3d.polylist->count;
	if (polycount <= 0) {
#ifdef DSA_GXGEOM_DEBUGWHY
		harness_profile_emitf("gxds3dwhy polycount<=0");
#endif
		return false;
	}
	if (gfx3d.wbuffer) {
#ifdef DSA_GXGEOM_DEBUGWHY
		harness_profile_emitf("gxds3dwhy wbuffer");
#endif
		return false;   // Z-buffer mode only, this first slice (see header comment)
	}
	if (gfx3d.enableClearImage) {
#ifdef DSA_GXGEOM_DEBUGWHY
		harness_profile_emitf("gxds3dwhy clearimage");
#endif
		return false;   // rear-plane per-pixel clear depth (Stage 2) not modelled
	}
	// Antialiasing is deliberately not checked: rasterize.cpp never implements it.
	if (gfx3d.enableEdgeMarking) {
#ifdef DSA_GXGEOM_DEBUGWHY
		harness_profile_emitf("gxds3dwhy edge");
#endif
		return false;   // no polygon-ID infra yet (13f handoff item)
	}
	if (gfx3d.enableFog) {
		// Task 13f-fog (see gx-next-steps-log.md's Task 13f-fog section): a GxFast-only
		// fog post-process (gxDs3dApplyFogFast()) was built this pass -- EFB-Z masking
		// to the geometry-covered region is verified correct, the fog LUT texture and
		// the GX hardware alpha blend are verified correct (a constant-alpha debug
		// build blends the right colour) -- but the single-8-bit-indirect-texture
		// Z-lookup that is supposed to make the blend weight vary per pixel with depth
		// does NOT vary: real-Dolphin A/B on a3_c29 shows the GX output pinned to one
		// constant colour across the whole fogged region while the CPU reference shows
		// a real (if subtle) per-row gradient -- i.e. the indirect stage is reading a
		// fixed offset, not the per-pixel Z8 value, root cause not found this pass
		// (see the log section for what was ruled out: LUT content/tiling, the Z-copy
		// mask, and the blend hardware itself are all independently confirmed working
		// via isolated mutations). Landing this live would trade an exact CPU fallback
		// for a LARGE characterized error (19511/19516 fogged pixels off by up to 83/255
		// per channel on that fixture) -- not the small/bounded kind of gap GxFast is
		// meant to accept, so fog keeps bailing to the CPU rasterizer unconditionally
		// here, same as before this pass, pending a future pass root-causing the
		// indirect-texture wiring. gxDs3dApplyFogFast() and its call site in
		// gx_ds_engine_impl.inc are left in place (compiled in, unreached) rather than
		// deleted, per this codebase's own precedent for a real-but-unfinished GX
		// technique (see item 13d's gx_mask two-pass draw history).
#ifdef DSA_GXGEOM_DEBUGWHY
		harness_profile_emitf("gxds3dwhy fog (13f-fog: indirect Z lookup not yet correct)");
#endif
		return false;
	}

	for (int i = 0; i < polycount; ++i) {
		POLY &p = gfx3d.polylist->list[i];   // isTranslucent() is non-const in POLY
		if (p.isTranslucent()) {
#ifdef DSA_GXGEOM_DEBUGWHY
			harness_profile_emitf("gxds3dwhy translucent i=%d", i);
#endif
			return false;
		}
		if (gxDs3dPolyMode(p) != 0) {
#ifdef DSA_GXGEOM_DEBUGWHY
			harness_profile_emitf("gxds3dwhy polymode i=%d mode=%d", i, gxDs3dPolyMode(p));
#endif
			return false;   // modulate only: decal/toon/highlight need their own TEV, shadow is 13g
		}
		if (gxDs3dTexFormat(p) != 0) {
#ifdef DSA_GXGEOM_DEBUGWHY
			harness_profile_emitf("gxds3dwhy texfmt i=%d fmt=%d", i, gxDs3dTexFormat(p));
#endif
			return false;   // untextured only, this first slice
		}
		if (p.type != 3 && p.type != 4) {
#ifdef DSA_GXGEOM_DEBUGWHY
			harness_profile_emitf("gxds3dwhy type i=%d type=%d", i, p.type);
#endif
			return false;
		}
	}

	// Every vertex must land inside [-1,1] on all three axes after the homogeneous
	// divide -- a sufficient condition for "the DS clipper would not have touched this
	// polygon" (see gx_ds_3d_render.h). Bail conservatively otherwise; clipped N-gons
	// are out of scope this pass.
	for (int i = 0; i < polycount; ++i) {
		const POLY &p = gfx3d.polylist->list[i];
		for (int j = 0; j < p.type; ++j) {
			const VERT &v = gfx3d.vertlist->list[p.vertIndexes[j]];
			if (v.coord[3] <= 0.0f) {
#ifdef DSA_GXGEOM_DEBUGWHY
				harness_profile_emitf("gxds3dwhy w<=0 i=%d j=%d w=%d", i, j, (int)(v.coord[3] * 1000.0f));
#endif
				return false;
			}
			const float nx = v.coord[0] / v.coord[3];
			const float ny = v.coord[1] / v.coord[3];
			const float nz = v.coord[2] / v.coord[3];
			if (nx < -1.0f || nx > 1.0f || ny < -1.0f || ny > 1.0f || nz < -1.0f || nz > 1.0f) {
#ifdef DSA_GXGEOM_DEBUGWHY
				harness_profile_emitf("gxds3dwhy ndc i=%d j=%d nx=%d ny=%d nz=%d", i, j,
				                      (int)(nx * 1000.0f), (int)(ny * 1000.0f), (int)(nz * 1000.0f));
#endif
				return false;
			}
		}
		if (gxRenderModeIsFast()) {
			GxDs3dFastXform x;
			if (!gxDs3dFastXformBuild(p, x)) {
#ifdef DSA_GXGEOM_DEBUGWHY
				harness_profile_emitf("gxds3dwhy fastproj i=%d", i);
#endif
				return false;
			}
		}
	}
#ifdef DSA_GXGEOM_DEBUGWHY
	harness_profile_emitf("gxds3dwhy OK polycount=%d", polycount);
#endif
	return true;
}

// Shared vertex-color / no-texture TEV+channel setup for both producers.
static void gxDs3dSetupCommonState()
{
	GX_SetCullMode(GX_CULL_NONE);
	// GX's clipper stays on: it only appeared to reject everything while Z was fed
	// the wrong way round (verified: enabling/disabling it gives identical output on
	// a3_c27/a3_c28 now), and gxDs3dGeomFrameSupported() keeps every vertex in-frustum.
	GX_SetClipMode(GX_CLIP_ENABLE);
	// Both producers write depth = (nz+1)/2 * 0xFFFFFF: near 0, far max (the ortho
	// matrix's -z_eye cancels gxDs3dRenderAccurate()'s negation; see
	// gxDs3dFastXformBuild() for Fast). The CPU rasterizer rejects when
	// depth >= dest, i.e. passes on LESS. (The previous pass tried GEQUAL, which is
	// the wrong direction -- that, not the EFB clear, is why draws vanished.)
	GX_SetZMode(GX_TRUE, GX_LESS, GX_TRUE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
#ifdef DSA_GXGEOM_MUTATE_NOZ
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);   // mutation: depth test must matter on a3_c28
#endif
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetDither(GX_FALSE);

	// Vertex color only (material color already baked per-vertex by the DS geometry
	// engine/SetVertex, opaque scope this pass so alpha is always 255) -- no lighting,
	// no texture, one TEV stage passing the raster color straight through.
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
}

// Found this pass: VERT::color is the DS geometry engine's native 6-bit-per-channel
// material color (0-63, see gfx3d.h's GFX3D_5TO6/rasterize.cpp's shader.materialColor
// clamp to [0,63]) -- feeding it directly to GX_Color4u8 (which expects 0-255) made
// every triangle render ~4x too dark. Standard 6-to-8-bit bit-replication expansion
// (matches this codebase's other N-bit-to-8-bit conventions, e.g. gfx3d.h's
// material_5bit_to_8bit table for the 5-bit case).
static inline u8 gxDs3d6To8(u8 v6) { return (u8)((v6 << 2) | (v6 >> 4)); }

static inline void gxDs3dSendColor(const VERT &v)
{
#ifdef DSA_GXGEOM_PROBE
	// Debug: paint each producer a flat marker colour so a capture shows exactly
	// which pixels GX drew (Accurate magenta, Fast cyan).
	(void)v;
	if (gxRenderModeIsFast()) GX_Color4u8(0, 255, 255, 255);
	else                      GX_Color4u8(255, 0, 255, 255);
#else
	GX_Color4u8(gxDs3d6To8(v.color[0]), gxDs3d6To8(v.color[1]), gxDs3d6To8(v.color[2]), 255);
#endif
}

static void gxDs3dLoadScreenOrtho()
{
	Mtx44 proj;
	guOrtho(proj, 0, (f32)kScreenH, 0, (f32)kScreenW, 0, 1);
	GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
	Mtx mv;
	guMtxIdentity(mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
}

// Seeds the EFB's Z with the DS CLEAR_DEPTH value (the CPU rasterizer's
// clearFragment.depth), independent of whatever GX_SetCopyClear or the 2D pass left
// there. Needs gxDs3dLoadScreenOrtho() bound; colour untouched.
static void gxDs3dClearDepth()
{
	const float z = -(float)(gfx3d.clearDepth & 0xFFFFFF) / 16777215.0f;
	GX_SetColorUpdate(GX_FALSE);
	GX_SetAlphaUpdate(GX_FALSE);
	GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position3f32(0, 0, z);                           GX_Color4u8(0, 0, 0, 255);
		GX_Position3f32((f32)kScreenW, 0, z);               GX_Color4u8(0, 0, 0, 255);
		GX_Position3f32((f32)kScreenW, (f32)kScreenH, z);   GX_Color4u8(0, 0, 0, 255);
		GX_Position3f32(0, (f32)kScreenH, z);               GX_Color4u8(0, 0, 0, 255);
	GX_End();
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
#ifdef DSA_GXGEOM_MUTATE_NOZ
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
#else
	GX_SetZMode(GX_TRUE, GX_LESS, GX_TRUE);
#endif
}

// ---------------------------------------------------------------------------------
// GxAccurate: CPU replicates rasterize.cpp's own homogeneous-divide + viewport
// transform exactly (same formulas, same order), so the resulting screen-space
// position is byte-for-byte what the CPU rasterizer would compute for the same
// vertex. GX is fed plain screen-pixel coordinates under the same orthographic
// screen-space projection already used for 2D content elsewhere in this codebase
// (gxDsBSetup2DState's guOrtho(0,kDsBScreenH,0,kDsBScreenW,0,1) pattern) and an
// identity PNMTX -- see gx_ds_3d_render.h's deviation note for why this replaces
// the design doc's original "feed GX a caller-supplied clip-space w" mechanism
// (GX has no such vertex attribute).
// ---------------------------------------------------------------------------------
void gxDs3dRenderAccurate()
{
	gxDs3dSetupCommonState();
	gxDs3dLoadScreenOrtho();
	gxDs3dClearDepth();

	const int polycount = gfx3d.polylist->count;
#ifdef DSA_GXGEOM_DEBUGWHY
	harness_profile_emitf("gxds3dACC enter polycount=%d", polycount);
#endif
	for (int i = 0; i < polycount; ++i) {
		const POLY &p = gfx3d.polylist->list[i];
		VIEWPORT vp;
		vp.decode(p.viewport);
#ifdef DSA_GXGEOM_DEBUGWHY
		harness_profile_emitf("gxds3dACC poly i=%d type=%d vp=%d,%d,%d,%d", i, p.type, vp.x, vp.y, vp.width, vp.height);
#endif

		GX_Begin(p.type == 4 ? GX_QUADS : GX_TRIANGLES, GX_VTXFMT0, p.type);
		for (int j = 0; j < p.type; ++j) {
			const VERT &v = gfx3d.vertlist->list[p.vertIndexes[j]];
			// rasterize.cpp's own formulas, verbatim (see gfx3d.h/rasterize.cpp's
			// clipping+viewport step): homogeneous divide remaps [-1,1] to [0,1],
			// then the polygon's own glViewport rect, then the DS's Y flip
			// (screen row 0 = top; see the .h file / log section for why GX's own
			// clip-space Y convention needs this same flip when GX does the divide
			// itself, which is exactly what makes this an *empirical* confirmation
			// of that finding once the fixture's silhouette lands in the right place).
			float sx = (v.coord[0] + v.coord[3]) / (2.0f * v.coord[3]);
			float sy = (v.coord[1] + v.coord[3]) / (2.0f * v.coord[3]);
			float sz = (v.coord[2] + v.coord[3]) / (2.0f * v.coord[3]);
			sx = sx * (float)vp.width + (float)vp.x;
			sy = sy * (float)vp.height + (float)vp.y;
			sy = 192.0f - sy;
			if (sx < 0.0f) sx = 0.0f; else if (sx > 256.0f) sx = 256.0f;
			if (sy < 0.0f) sy = 0.0f; else if (sy > 192.0f) sy = 192.0f;
			// rasterize.cpp's edge walker (Ceil28_4) samples each pixel at its top-left
			// corner; GX samples at the pixel centre, so shift by +0.5. Deliberately NOT
			// snapped to the CPU's truncated 1/16 grid: measured on a3_c28, that snap
			// makes GX break sample-on-edge ties differently (29 bad px vs 15 unsnapped).
			sx += kGxDs3dSampleOffset;
			sy += kGxDs3dSampleOffset;
			// Found this pass (see gx-next-steps-log.md's Task 13e-precision section):
			// guOrtho(mt,t,b,l,r,n,f)'s "-z axis" convention (per libogc's own gu.h doc
			// comment) means the visible eye-space Z range for near=0,far=1 is [-1,0],
			// NOT [0,1] -- feeding the DS's natural 0=near/1=far sz straight into
			// GX_Position3f32 (as this function did before this pass) puts every vertex
			// OUTSIDE that visible range, so nothing was ever actually rasterized: the
			// whole geometry-overlay draw was silently a no-op on real Dolphin, verified
			// with a forced full-screen probe triangle (0 pixels visible with positive Z,
			// full coverage with negated Z). Negating maps sz's [0,1] onto exactly the
			// [-1,0] guOrtho expects.
			const float gxz = -sz;

#ifdef DSA_GXGEOM_DEBUGWHY
			harness_profile_emitf("gxds3dACC vtx i=%d j=%d sx=%d sy=%d gxz=%d col=%d,%d,%d",
			                      i, j, (int)sx, (int)sy, (int)(gxz * 1000.0f),
			                      v.color[0], v.color[1], v.color[2]);
#endif
			GX_Position3f32(sx, sy, gxz);
			gxDs3dSendColor(v);
		}
		GX_End();
	}
}

// ---------------------------------------------------------------------------------
// GxFast: GX's transform hardware does the multiply + perspective divide against
// VERT::objcoord, using the polygon's own mvMatrix/projMatrix snapshot (a later
// polygon can have different matrices) folded per gxDs3dFastXformBuild(). No Y flip:
// GX and the DS both put NDC y=+1 at the top of the viewport. The DS viewport
// (bottom-left origin) becomes a GX viewport (top-left origin).
// ---------------------------------------------------------------------------------
void gxDs3dRenderFast()
{
	gxDs3dSetupCommonState();
	gxDs3dLoadScreenOrtho();
	gxDs3dClearDepth();

	const int polycount = gfx3d.polylist->count;
	float lastMv[16], lastProj[16];
	u32 lastVp = 0;
	bool haveLast = false;

	for (int i = 0; i < polycount; ++i) {
		const POLY &p = gfx3d.polylist->list[i];

		if (!haveLast || p.viewport != lastVp) {
			VIEWPORT vp;
			vp.decode(p.viewport);
			GX_SetViewport((f32)vp.x + kGxDs3dSampleOffset,
			               (f32)(kScreenH - vp.y - vp.height) + kGxDs3dSampleOffset,
			               (f32)vp.width, (f32)vp.height, 0, 1);
			lastVp = p.viewport;
		}
		if (!haveLast || memcmp(lastMv, p.mvMatrix, sizeof(lastMv)) != 0 ||
		    memcmp(lastProj, p.projMatrix, sizeof(lastProj)) != 0) {
			GxDs3dFastXform x;
			if (!gxDs3dFastXformBuild(p, x))
				continue;   // unreachable: gxDs3dGeomFrameSupported() checked every poly
			GX_LoadPosMtxImm(x.pos, GX_PNMTX0);
			GX_SetCurrentMtx(GX_PNMTX0);
			GX_LoadProjectionMtx(x.proj, x.projType);

			memcpy(lastMv, p.mvMatrix, sizeof(lastMv));
			memcpy(lastProj, p.projMatrix, sizeof(lastProj));
			haveLast = true;
		}

		GX_Begin(p.type == 4 ? GX_QUADS : GX_TRIANGLES, GX_VTXFMT0, p.type);
		for (int j = 0; j < p.type; ++j) {
			const VERT &v = gfx3d.vertlist->list[p.vertIndexes[j]];
			GX_Position3f32(v.objcoord[0], v.objcoord[1], v.objcoord[2]);
			gxDs3dSendColor(v);
		}
		GX_End();
	}
}

// ---------------------------------------------------------------------------------
// Task 13f-fog: fog post-process, GxFast only (see gx_ds_3d_render.h's comment on
// gxDs3dApplyFogFast() and gx-13f-design.md section 2 for the full precision writeup).
// ---------------------------------------------------------------------------------

// Mirrors rasterize.cpp's own fast fogTable[] build (~line 1268) exactly -- this is the
// CPU's fully-resolved 15-bit-indexed LUT; GX never redoes the density interpolation
// itself, it only samples this already-correct table (downsampled to 8 bits below).
static u8 s_fogTable[32768];

static void gxDs3dFogBuildTable()
{
	const u8 *fogDensity = MMU.MMU_MEM[ARMCPU_ARM9][0x40] + 0x360;
	const int increment = ((1 << 10) >> gfx3d.fogShift);
	const int incrementDivShift = 10 - gfx3d.fogShift;
	u32 fogOffset = gfx3d.fogOffset;
	if (fogOffset > 32768) fogOffset = 32768;
	u32 iMin = ((1u + 1u) << incrementDivShift) + fogOffset + 1u - (u32)increment;
	u32 iMax = ((32u + 1u) << incrementDivShift) + fogOffset + 1u - (u32)increment;
	if (iMin > 32768) iMin = 32768;
	if (iMax > 32768) iMax = 32768;
	if (iMin > iMax) iMin = iMax;   // defensive; CPU asserts this never happens
	memset(s_fogTable, fogDensity[0], iMin);
	for (u32 i = iMin; i < iMax; i++) {
		int num = (int)(i - fogOffset) + (increment - 1);
		int j = (num >> incrementDivShift) - 1;
		if (j < 1) j = 1;
		if (j > 31) j = 31;
		u32 value = (u32)(num & ~(increment - 1)) + fogOffset;
		u32 diff = value - i;
		s_fogTable[i] = (u8)(((u32)diff * fogDensity[j - 1] + (u32)((u32)increment - diff) * fogDensity[j]) >> incrementDivShift);
	}
	memset(s_fogTable + iMax, fogDensity[31], 32768 - iMax);
}

// 256-entry indirect-lookup LUT: texel k holds the blend weight (0-255, i.e. the CPU's
// 0-128 fog value with the 127->128 snap applied, rescaled *2 so it can be fed straight
// to GX's hardware alpha blender instead of a manual TEV lerp -- see gxDs3dApplyFogFast).
// Indexed by the TOP 8 BITS of the GX Z-buffer's 24-bit depth, i.e. one entry per 128
// consecutive real fogIndex values (fogIndex = depth>>9 is 15-bit) -- the precision loss
// gx-13f-design.md section 2 characterizes as the real, accepted GxFast error source.
// I8's GX block shape is 8 wide x 4 tall (gx_texformat.h) -- a texture's stored bytes
// are tiled into 8x4 blocks, not row-major linear, so the LUT's actual texture height
// must be a multiple of 4 and its bytes swizzled with gxSwizzle8bpp (found this pass:
// a naive height=1 texture with a flat memcpy silently sampled as all-zero/garbage on
// real Dolphin -- same lesson class as the GX_TF_A8-vs-I8 sample-format trap this
// codebase already learned, just the tiling half of it instead of the format half).
// Four identical rows (v is always 0 at sample time, see gxDs3dApplyFogFast) satisfy
// the block-height requirement without adding a second real dimension of data.
static const int kFogLutH = 4;
static u8 s_fogLut[256];
static u8 s_fogLutLinear[256 * kFogLutH];
static GXTexObj s_fogLutTex;
static void *s_fogLutTexData = nullptr;
static bool s_fogLutValid = false;
// Cache key: only rebuild the (somewhat expensive, 32768-entry) table when something it
// depends on actually changed -- FOG_TABLE's 32 density bytes, fogOffset, fogShift, or
// fogColor (color doesn't affect the table, but is cheap to fold into the same key so a
// single memcmp covers "does the LUT texture need re-uploading" completely).
static u8 s_fogKey[32 + 4 + 4 + 4];

static void gxDs3dFogEnsureLut()
{
	u8 key[sizeof(s_fogKey)];
	memcpy(key, MMU.MMU_MEM[ARMCPU_ARM9][0x40] + 0x360, 32);
	memcpy(key + 32, &gfx3d.fogOffset, 4);
	memcpy(key + 36, &gfx3d.fogShift, 4);
	memcpy(key + 40, &gfx3d.fogColor, 4);

	if (s_fogLutValid && memcmp(key, s_fogKey, sizeof(key)) == 0)
		return;

	gxDs3dFogBuildTable();
	for (int k = 0; k < 256; ++k) {
		u32 idx = (u32)k * 128 + 64;
		if (idx > 32767) idx = 32767;
		u32 fog = s_fogTable[idx];
		if (fog == 127) fog = 128;
		u32 weight = fog * 2;
		if (weight > 255) weight = 255;
		s_fogLut[k] = (u8)weight;
	}

	for (int row = 0; row < kFogLutH; ++row)
		memcpy(s_fogLutLinear + row * 256, s_fogLut, 256);

	if (!s_fogLutTexData) {
		u32 size = GX_GetTexBufferSize(256, kFogLutH, GX_TF_I8, GX_FALSE, 0);
		s_fogLutTexData = memalign(32, size);
	}
	if (s_fogLutTexData) {
		gxSwizzle8bpp(s_fogLutLinear, (u8 *)s_fogLutTexData, 8, 4, 256, kFogLutH);
		DCFlushRange(s_fogLutTexData, GX_GetTexBufferSize(256, kFogLutH, GX_TF_I8, GX_FALSE, 0));
		GX_InitTexObj(&s_fogLutTex, s_fogLutTexData, 256, kFogLutH, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjFilterMode(&s_fogLutTex, GX_NEAR, GX_NEAR);
	}
	memcpy(s_fogKey, key, sizeof(key));
	s_fogLutValid = true;
}

// EFB Z-copy target: top 8 bits of the 24-bit GX Z-buffer, one texel per screen pixel.
// GX_TF_Z8 (0x11) is a "for texture copy" Z format, not one of the CTF_* families the
// gx_mask.h lesson was written about, but the same low-nibble-only trap applies: 0x11's
// low nibble is 0x1 = GX_TF_I8, so the texture object must be told to SAMPLE as GX_TF_I8
// even though the EFB copy itself specifies GX_TF_Z8 as the destination format.
static GXTexObj s_fogZTex;
static void *s_fogZTexData = nullptr;

static void gxDs3dFogCopyZ(u16 w, u16 h)
{
	if (!s_fogZTexData) {
		u32 size = GX_GetTexBufferSize(w, h, GX_TF_I8, GX_FALSE, 0);
		s_fogZTexData = memalign(32, size);
	}
	if (!s_fogZTexData) return;

	GX_SetTexCopySrc(0, 0, w, h);
	GX_SetTexCopyDst(w, h, GX_TF_Z8, GX_FALSE);
	GX_CopyTex(s_fogZTexData, GX_FALSE);
	GX_PixModeSync();
	GX_InvalidateTexAll();

	GX_InitTexObj(&s_fogZTex, s_fogZTexData, w, h, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&s_fogZTex, GX_NEAR, GX_NEAR);
}

void gxDs3dApplyFogFast()
{
	gxDs3dFogEnsureLut();
	gxDs3dFogCopyZ(kScreenW, kScreenH);

	// gxDs3dRenderFast() leaves whichever per-polygon perspective matrix its last
	// polygon used loaded (GX_PNMTX0 / the projection register) -- this quad is fed
	// plain 0..256/0..192 screen-pixel positions and needs the same identity-PNMTX
	// screen-space ortho gxDs3dRenderAccurate() uses, not whatever GxFast left bound.
	// Found this pass: without this call the quad's positions go through the wrong
	// (arbitrary, per-polygon) transform and land outside the clip volume, so the
	// fog draw was a silent, invisible no-op -- verified via a PEEK probe showing the
	// unfogged raster colour unchanged after this function ran.
	gxDs3dLoadScreenOrtho();

	// Mask: only pixels the geometry pass actually wrote get fogged. gxDs3dClearDepth()
	// seeded every pixel in this region to the same reference Z before the geometry draw;
	// any pixel a triangle covered now holds a smaller (nearer) Z than that reference
	// (the geometry pass's own GX_LESS test guarantees this), so comparing the incoming
	// quad's Z (fed the same reference value) GX_GREATER against the buffer catches
	// exactly "something nearer was drawn here" without needing a polygon-ID buffer.
	const float clearZ = -(float)(gfx3d.clearDepth & 0xFFFFFF) / 16777215.0f;

	GX_LoadTexObj(&s_fogZTex, GX_TEXMAP0);
	GX_LoadTexObj(&s_fogLutTex, GX_TEXMAP1);

	GX_SetNumTexGens(2);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);

	GX_SetNumIndStages(1);
	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP0);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);
	// ds = 0.5 * (Z8-128) * 2^-7 : maps Z8 in [0,255] to a LUT texcoord offset spanning
	// almost exactly [0,1) around the base (0.5,0) coordinate fed via TEXCOORD1 below --
	// see gx-next-steps-log.md's Task 13f-fog section for the derivation.
	f32 indMtx[2][3] = { { 0.5f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
	GX_SetIndTexMatrix(GX_ITM_0, indMtx, -7);

	GX_SetNumChans(0);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
#ifdef DSA_GXGEOM_MUTATE_FOGNOINDIRECT
	GX_SetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_S, GX_ITM_OFF,
	                   GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
#else
	GX_SetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_S, GX_ITM_0,
	                   GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
#endif

	GXColor fogGx;
	fogGx.r = (u8)(GFX3D_5TO6(gfx3d.fogColor & 0x1F) << 2);
	fogGx.g = (u8)(GFX3D_5TO6((gfx3d.fogColor >> 5) & 0x1F) << 2);
	fogGx.b = (u8)(GFX3D_5TO6((gfx3d.fogColor >> 10) & 0x1F) << 2);
	fogGx.a = 255;
	GX_SetTevKColor(GX_KCOLOR0, fogGx);
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);

	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
#ifdef DSA_GXGEOM_MUTATE_FOGCONSTALPHA
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1_2);   // debug: constant 1/2 alpha, no texture/indirect involved
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
#else
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
#endif
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

#ifdef DSA_GXGEOM_MUTATE_FOGNOZMASK
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);   // debug: bypass the geometry Z-mask entirely
#else
	GX_SetZMode(GX_TRUE, GX_GREATER, GX_FALSE);
#endif
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetDither(GX_FALSE);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);

	GX_Begin(GX_QUADS, GX_VTXFMT1, 4);
		GX_Position3f32(0, 0, clearZ);
		GX_TexCoord2f32(0.0f, 0.0f); GX_TexCoord2f32(0.5f, 0.0f);
		GX_Position3f32((f32)kScreenW, 0, clearZ);
		GX_TexCoord2f32(1.0f, 0.0f); GX_TexCoord2f32(0.5f, 0.0f);
		GX_Position3f32((f32)kScreenW, (f32)kScreenH, clearZ);
		GX_TexCoord2f32(1.0f, 1.0f); GX_TexCoord2f32(0.5f, 0.0f);
		GX_Position3f32(0, (f32)kScreenH, clearZ);
		GX_TexCoord2f32(0.0f, 1.0f); GX_TexCoord2f32(0.5f, 0.0f);
	GX_End();

	// Restore what this draw doesn't own the caller relying on (mirrors
	// gxDs3dSetupCommonState's own contract): the next thing to run in
	// gx_ds_engine_impl.inc is gxDsBSetup2DState(), which sets its own num-tex-gens,
	// tev stages, chan count and vertex format from scratch, so nothing further needs
	// resetting here beyond leaving indirect stages at a sane count.
	GX_SetNumIndStages(0);
	GX_SetNumTexGens(1);
}
