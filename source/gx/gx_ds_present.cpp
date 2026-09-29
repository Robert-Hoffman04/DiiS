// gx_ds_present.cpp - see gx_ds_present.h (Task directpresent).
#include "gx_ds_present.h"
#include "../GPU.h"
#include <gccore.h>

const void *g_gxDsPresentPending[2] = { NULL, NULL };

static const int kW = 256, kH = 192;

void gxDsPresentSet(int slot, const void *buf)
{
	g_gxDsPresentPending[slot] = buf;
}

void gxDsPresentReleaseBuffer(const void *buf)
{
	for (int s = 0; s < 2; ++s)
		if (g_gxDsPresentPending[s] == buf) gxDsPresentResolveSlotImpl(s);
}

void gxDsPresentDropAll()
{
	g_gxDsPresentPending[0] = g_gxDsPresentPending[1] = NULL;
}

bool gxDsPresentAllOpaque(const void *buf)
{
	DCInvalidateRange((void *)buf, kW * kH * 2);
	const u32 *p = (const u32 *)buf;
	u32 acc = 0x80008000u;
	for (int i = 0; i < kW * kH / 2; i += 4)
		acc &= p[i] & p[i + 1] & p[i + 2] & p[i + 3];
	return acc == 0x80008000u;
}

// The old engine readback (gxUnswizzle16bpp + gxDsBRgb5a3ToNds, MASTER_BRIGHT off) fused
// into one pass over the 4x4 RGB5A3 tiles; same per-pixel result.
void gxDsPresentResolveSlotImpl(int slot)
{
	const u16 *src = (const u16 *)g_gxDsPresentPending[slot];
	g_gxDsPresentPending[slot] = NULL;
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
