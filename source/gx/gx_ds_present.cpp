// gx_ds_present.cpp - see gx_ds_present.h (Task directpresent).
#include "gx_ds_present.h"
#include "../GPU.h"
#include <gccore.h>
#include <malloc.h>
#include "../perf_zones.h"   // Task pzones: mbright / readback zones (no-op otherwise)
#include "gx_fence.h"
#include <ogc/lwp.h>
#include <ogc/machine/processor.h>   // _CPU_ISR_Disable/Restore

const void *g_gxDsPresentPending[2] = { NULL, NULL };
static u16 s_presentFence[2];   // Task gpu-overlap: the copy that fills each pending slot's buffer

// Task gpu-overlap: gx_fence.h.
u16 g_gxFenceNext, g_gxFence3d, g_gxFencePresent;

void gxFenceWait(u16 t)
{
	if (gxFenceReached(t)) return;
	PZ_SUB_SCOPE(PZ_GX_FENCE);
	while (!gxFenceReached(t)) {}
}

// Task host-zone: interrupt-driven sleep. The usleep(50) poll this replaces woke the
// higher-priority draw_thread ~every 50 us for the whole GPU backlog, preempting the emulation
// thread each time (+1.2 ms/frame in zone `host` on hardware). Now the PE token interrupt
// (draw-sync callback, fired for every GX_SetDrawSync token the GPU reaches) wakes it once.
static lwpq_t s_fenceQueue = LWP_TQUEUE_NULL;
static void gxFenceSyncCb(u16)
{
	LWP_ThreadBroadcast(s_fenceQueue);
}

void gxFenceWaitSleep(u16 t)
{
	if (gxFenceReached(t)) return;
	if (s_fenceQueue == LWP_TQUEUE_NULL) {
		LWP_InitQueue(&s_fenceQueue);
		GX_SetDrawSyncCallback(gxFenceSyncCb);
	}
	u32 level;
	_CPU_ISR_Disable(level);   // same pattern as libogc's GX_WaitDrawDone: no lost wakeup
	while (!gxFenceReached(t)) LWP_ThreadSleep(s_fenceQueue);
	_CPU_ISR_Restore(level);
}

static const int kW = 256, kH = 192;

void gxDsPresentSet(int slot, const void *buf, u16 fence)
{
	g_gxDsPresentPending[slot] = buf;
	s_presentFence[slot] = fence;
}

void gxDsPresentReleaseBuffer(const void *buf, int keepSlot)
{
	// Task lazyfix: keepSlot is about to be overwritten whole by the copy being released for,
	// so its stale contents are dropped, not converted (they were already presented).
	for (int s = 0; s < 2; ++s)
		if (g_gxDsPresentPending[s] == buf) {
			if (s == keepSlot) g_gxDsPresentPending[s] = NULL;
			else gxDsPresentResolveSlotImpl(s);
		}
}

void gxDsPresentDropAll()
{
	g_gxDsPresentPending[0] = g_gxDsPresentPending[1] = NULL;
}

// Task mbright: MASTER_BRIGHT on the GPU, exact.
// The CPU applies it per line to the finished 15-bit colour, channel by channel:
// up c + (31-c)*f/16, down c - c*f/16 (GPU.cpp's float tables, truncated). The EFB holds 8-bit
// channels x whose top 5 bits (x >> 3) are exactly what the RGB5A3 copy keeps, so the fade is
// a 256-entry lookup per channel. Each channel is copied out as GX_CTF_R8/G8/B8 (8-bit, I8
// tile layout) and sampled as CI8 through a TLUT whose entry x is the grey opaque RGB5A3 colour
// fade(x >> 3); three TEV stages mask one channel each with a konst colour (K = 255 is an
// exact multiply by 1) and add them, and the result overwrites the EFB (no blend). Each channel
// lands as (v << 3) | (v >> 2), the same 8 bits an RGB5A3 texel decodes to, so the following
// copy holds exactly fade(c) and the EFB matches what the old CPU-built present texture showed.
// (An RGB565 TLUT is NOT equivalent: green expands from 6 bits, (v << 3) | (v >> 3), and Dolphin
// presents the copy from its unquantized GPU-side EFB copy, so the TV output was off by one LSB
// of 8 in green while every GPU_screen capture still matched.)
// One quad per run of lines with the same (mode, factor): a mid-frame change is per line.
static const int kMbTexBytes = kW * kH;   // one 8-bit channel
static u8 *s_mbChan[3];
static GXTexObj s_mbTex[3];
static u16 s_mbTlutData[32][256] ATTRIBUTE_ALIGN(32);   // [(mode-1)*16 + fac-1]
static GXTlutObj s_mbTlut[32];
static bool s_mbInit = false;
static const u8 kMbTlutName = GX_TLUT5;   // GX_TLUT0..5 are unused on the DS path (engine impl header comment)

static bool gxDsPresentMbInit()
{
	if (s_mbInit) return true;
	for (int c = 0; c < 3; ++c) {
		if (!s_mbChan[c]) s_mbChan[c] = (u8 *)memalign(32, kMbTexBytes);
		if (!s_mbChan[c]) return false;
		GX_InitTexObjCI(&s_mbTex[c], s_mbChan[c], kW, kH, GX_TF_CI8, GX_CLAMP, GX_CLAMP, GX_FALSE, kMbTlutName);
		GX_InitTexObjFilterMode(&s_mbTex[c], GX_NEAR, GX_NEAR);
	}
	for (int m = 1; m <= 2; ++m) {
		for (int f = 1; f <= 16; ++f) {
			u16 *t = s_mbTlutData[(m - 1) * 16 + f - 1];
			const float idiv16 = ((float)f) / 16;   // same expression as GPU.cpp / gxDsBInitFadeTables
			for (int x = 0; x < 256; ++x) {
				const int c = x >> 3;
				const u8 v = (m == 1) ? (u8)(c + ((31 - c) * idiv16)) : (u8)(c - (c * idiv16));
#ifdef DSP_MB_MUTATE
				const u8 vm = (u8)(v ^ 1);   // mutation: one step off, must change every fade fixture
				t[x] = (u16)(0x8000 | (vm << 10) | (vm << 5) | vm);
#else
				t[x] = (u16)(0x8000 | (v << 10) | (v << 5) | v);   // grey opaque RGB5A3
#endif
			}
			GX_InitTlutObj(&s_mbTlut[(m - 1) * 16 + f - 1], t, GX_TL_RGB5A3, 256);
		}
	}
	DCFlushRange(s_mbTlutData, sizeof(s_mbTlutData));
	s_mbInit = true;
	return true;
}

bool gxDsPresentMasterBright(const u8 *mode, const u8 *fac)
{
	PZ_SCOPE(PZ_MBRIGHT);
	if (!gxDsPresentMbInit()) return false;
	static const u32 kCopyFmt[3] = { GX_CTF_R8, GX_CTF_G8, GX_CTF_B8 };
	GX_SetTexCopySrc(0, 0, kW, kH);
	for (int c = 0; c < 3; ++c) {
		GX_SetTexCopyDst(kW, kH, kCopyFmt[c], GX_FALSE);
		GX_CopyTex(s_mbChan[c], GX_FALSE);
	}
	GX_PixModeSync();
	GX_InvalidateTexAll();

	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetNumTevStages(3);
	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 255, 0, 0, 255 });
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 0, 255, 0, 255 });
	GX_SetTevKColor(GX_KCOLOR2, (GXColor){ 0, 0, 255, 255 });
	for (int c = 0; c < 3; ++c) {
		const u8 st = (u8)(GX_TEVSTAGE0 + c);
		GX_SetTevOrder(st, GX_TEXCOORD0, (u32)(GX_TEXMAP0 + c), GX_COLORNULL);
		GX_SetTevKColorSel(st, (u8)(GX_TEV_KCSEL_K0 + c));
		GX_SetTevKAlphaSel(st, GX_TEV_KASEL_1);
		GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, c ? GX_CC_CPREV : GX_CC_ZERO);
		GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
		GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_LoadTexObj(&s_mbTex[c], (u8)(GX_TEXMAP0 + c));
	}
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);

	int y0 = 0;
	while (y0 < kH) {
		const int key = (mode[y0] != 0 && fac[y0] != 0) ? (mode[y0] - 1) * 16 + fac[y0] - 1 : -1;
		int y1 = y0 + 1;
		while (y1 < kH && ((mode[y1] != 0 && fac[y1] != 0) ? (mode[y1] - 1) * 16 + fac[y1] - 1 : -1) == key) ++y1;
		if (key >= 0) {
			GX_LoadTlut(&s_mbTlut[key], kMbTlutName);
			const f32 t0 = (f32)y0 / kH, t1 = (f32)y1 / kH;
			GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
				GX_Position2f32(0, (f32)y0);         GX_TexCoord2f32(0, t0);
				GX_Position2f32((f32)kW, (f32)y0);   GX_TexCoord2f32(1, t0);
				GX_Position2f32((f32)kW, (f32)y1);   GX_TexCoord2f32(1, t1);
				GX_Position2f32(0, (f32)y1);         GX_TexCoord2f32(0, t1);
			GX_End();
		}
		y0 = y1;
	}

	// Back to gxDsBSetup2DState()'s single-stage REPLACE / blend baseline.
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	return true;
}

// The old engine readback (gxUnswizzle16bpp + gxDsBRgb5a3ToNds, MASTER_BRIGHT off) fused
// into one pass over the 4x4 RGB5A3 tiles; same per-pixel result.
void gxDsPresentResolveSlotImpl(int slot)
{
	PZ_SCOPE(PZ_2D_READBACK);
	const u16 *src = (const u16 *)g_gxDsPresentPending[slot];
	g_gxDsPresentPending[slot] = NULL;
	gxFenceWait(s_presentFence[slot]);   // Task gpu-overlap: the copy may still be in flight
	DCInvalidateRange((void *)src, kW * kH * 2);
	u16 *dst = (u16 *)(GPU_screen + (u32)slot * 192 * 512);
	for (int ty = 0; ty < kH / 4; ++ty) {
		for (int tx = 0; tx < kW / 4; ++tx) {
			for (int r = 0; r < 4; ++r) {
				u16 *out = dst + (u32)(ty * 4 + r) * kW + tx * 4;
				for (int c = 0; c < 4; ++c) {
					const u16 v = *src++;
					u8 R, G, B;
					if (v & 0x8000) {
						R = (v >> 10) & 0x1F; G = (v >> 5) & 0x1F; B = v & 0x1F;
					} else {
						const u8 r4 = (v >> 8) & 0xF, g4 = (v >> 4) & 0xF, b4 = v & 0xF;
						R = (u8)((r4 << 1) | (r4 >> 3)); G = (u8)((g4 << 1) | (g4 >> 3)); B = (u8)((b4 << 1) | (b4 >> 3));
					}
					out[c] = (u16)((B << 10) | (G << 5) | R);
				}
			}
		}
	}
}
