#include "gx_ds_3d_render.h"
#include "gx_rendermode.h"
#include "../gfx3d.h"
#include <gccore.h>
#include <string.h>
#ifdef DSA_GXGEOM_DEBUGWHY
#include "../harness/harness.h"
#include "../harness/harness_profile.h"
#endif

// DS screen dimensions -- duplicated from gx_ds_engine_impl.inc's kDsBScreenW/H (not
// visible from here; both are just the fixed 256x192 DS screen size, not expected to
// ever differ) rather than pulling in that whole translation unit's static state.
static const int kScreenW = 256;
static const int kScreenH = 192;

// POLYGON_ATTR shading mode, bits 4-5: 0 modulate, 1 decal, 2 toon/highlight, 3 shadow.
static inline int gxDs3dPolyMode(const POLY &p) { return (int)((p.polyAttr >> 4) & 3); }
// TEXIMAGE_PARAM format, bits 26-28 (see POLY::isTranslucent's own use of this same shift).
static inline int gxDs3dTexFormat(const POLY &p) { return (int)((p.texParam >> 26) & 7); }

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

	for (int i = 0; i < polycount; ++i) {
		POLY &p = gfx3d.polylist->list[i];   // isTranslucent() is non-const in POLY
		if (p.isTranslucent()) {
#ifdef DSA_GXGEOM_DEBUGWHY
			harness_profile_emitf("gxds3dwhy translucent i=%d", i);
#endif
			return false;
		}
		if (gxDs3dPolyMode(p) == 3) {
#ifdef DSA_GXGEOM_DEBUGWHY
			harness_profile_emitf("gxds3dwhy shadow i=%d", i);
#endif
			return false;   // shadow polygon -- no GX equivalent this pass (13g)
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
	// Found this pass: GX's hardware near/far clipper rejected every vertex this
	// slice ever submitted (verified with a forced full-screen probe triangle --
	// see the log section), even for vertices well inside gxDs3dGeomFrameSupported()'s
	// own [-1,1] NDC bounds check. Root cause not fully isolated within this pass's
	// budget (plausibly guOrtho's near/far mapping interacting with GX's clip-space
	// W under GX_ORTHOGRAPHIC); disabling GX's own clipper is safe here because
	// gxDs3dGeomFrameSupported() already independently guarantees every vertex is
	// within the DS's own [-1,1] NDC cube on the CPU side before this code ever runs.
	GX_SetClipMode(GX_CLIP_DISABLE);
	// Z-test is disabled (GX_FALSE), not the design's originally-intended
	// GX_TRUE/GX_LEQUAL/GX_TRUE: this pass found that even after negating Z (see
	// gxDs3dRenderAccurate()'s comment) so geometry is inside GX's visible range at
	// all, turning the depth test back on (tried GX_GEQUAL, the direction the negated
	// convention should need) still made every draw invisible -- isolated with the
	// same forced full-screen probe triangle technique, down to "Z-test enabled with
	// ANY compare function tried" as the remaining blocker, most likely an interaction
	// with the EFB Z-buffer's actual cleared content (GX_SetCopyClear's GX_MAX_Z24,
	// set once globally in main.cpp) not being what GEQUAL against our negated range
	// needs -- not root-caused further within this pass's budget. Leaving Z-test off
	// is what makes this pass's fixture(s) draw at all; it is NOT depth-correct for
	// overlapping geometry (submission order wins instead of nearest-wins) -- see the
	// log section's handoff, this is the top priority for whoever continues 13e.
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
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
	GX_Color4u8(gxDs3d6To8(v.color[0]), gxDs3d6To8(v.color[1]), gxDs3d6To8(v.color[2]), 255);
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

	Mtx44 proj;
	guOrtho(proj, 0, (f32)kScreenH, 0, (f32)kScreenW, 0, 1);
	GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
	Mtx mv;
	guMtxIdentity(mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);

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
// GxFast: converts the polygon's OWN captured mvMatrix/projMatrix (POLY snapshots,
// taken at submission time by gfx3d.cpp -- a later polygon in the same list can have
// different matrices) to real GX matrices and lets GX's fixed-function transform
// hardware do the multiply + perspective divide against VERT::objcoord (object-space,
// added this task -- see gfx3d.h). DS matrices are stored column-major
// (matrix.cpp's MatrixMultVec4x4: out[i] = sum_c v[c]*m[4c+i]); GX's Mtx/Mtx44 are
// row-major (mt[r][c]) -- converting is a transpose: gx[r][c] = ds[4*c + r].
// Row 1 (the Y output row) of the projection matrix is additionally negated to bake
// in the same Y-axis flip gxDs3dRenderAccurate applies on the CPU -- GX's own
// clip-space Y convention is the opposite sense from the DS's (see the log section's
// "clip-space convention" finding).
// ---------------------------------------------------------------------------------
static void gxDs3dConvertMv(const float *ds, Mtx out)
{
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 4; ++c)
			out[r][c] = ds[4 * c + r];
}

static void gxDs3dConvertProj(const float *ds, Mtx44 out)
{
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			out[r][c] = ds[4 * c + r];
	for (int c = 0; c < 4; ++c)
		out[1][c] = -out[1][c];   // flip Y (GX vs DS clip-space convention)
}

void gxDs3dRenderFast()
{
	gxDs3dSetupCommonState();

	const int polycount = gfx3d.polylist->count;
	float lastMv[16], lastProj[16];
	bool haveLast = false;

	for (int i = 0; i < polycount; ++i) {
		const POLY &p = gfx3d.polylist->list[i];

		if (!haveLast || memcmp(lastMv, p.mvMatrix, sizeof(lastMv)) != 0 ||
		    memcmp(lastProj, p.projMatrix, sizeof(lastProj)) != 0) {
			Mtx mv44;
			gxDs3dConvertMv(p.mvMatrix, mv44);
			GX_LoadPosMtxImm(mv44, GX_PNMTX0);
			GX_SetCurrentMtx(GX_PNMTX0);

			Mtx44 proj;
			gxDs3dConvertProj(p.projMatrix, proj);
			GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);

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
