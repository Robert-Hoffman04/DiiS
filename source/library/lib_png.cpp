/****************************************************************************
 * DiiS - lib_png.cpp   (see lib_png.h)
 ***************************************************************************/
#include "lib_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static inline int paeth(int a, int b, int c)
{
	const int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

bool libPngDecode(const uint8_t *data, size_t len, std::vector<uint8_t> &rgba, int &w, int &h)
{
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	if (len < 8 || memcmp(data, sig, 8)) return false;

	int depth = 0, ctype = -1;
	w = h = 0;
	uint8_t pal[256][4];
	memset(pal, 0xFF, sizeof(pal));
	int trnsGrey = -1;
	std::vector<uint8_t> z;
	for (size_t p = 8; p + 12 <= len;) {
		const uint32_t n = be32(data + p);
		const uint8_t *type = data + p + 4, *body = data + p + 8;
		if (n > len - p - 12) return false;
		if (!memcmp(type, "IHDR", 4) && n >= 13) {
			w = (int)be32(body); h = (int)be32(body + 4);
			depth = body[8]; ctype = body[9];
			if (body[10] || body[11] || body[12]) return false;   // compression, filter, interlace
		} else if (!memcmp(type, "PLTE", 4)) {
			for (uint32_t i = 0; i < n / 3 && i < 256; i++) {
				pal[i][0] = body[i * 3]; pal[i][1] = body[i * 3 + 1]; pal[i][2] = body[i * 3 + 2];
			}
		} else if (!memcmp(type, "tRNS", 4)) {
			if (ctype == 3) for (uint32_t i = 0; i < n && i < 256; i++) pal[i][3] = body[i];
			else if (ctype == 0 && n >= 2) trnsGrey = (body[0] << 8) | body[1];
		} else if (!memcmp(type, "IDAT", 4)) {
			z.insert(z.end(), body, body + n);
		} else if (!memcmp(type, "IEND", 4)) {
			break;
		}
		p += 12 + n;
	}
	if (w <= 0 || h <= 0 || w > 256 || h > 256 || z.empty()) return false;

	int chans;
	switch (ctype) {
	case 0: chans = 1; break;
	case 2: chans = 3; break;
	case 3: chans = 1; break;
	case 4: chans = 2; break;
	case 6: chans = 4; break;
	default: return false;
	}
	const bool lowDepthOk = (ctype == 0 || ctype == 3) && (depth == 1 || depth == 2 || depth == 4);
	if (depth != 8 && !lowDepthOk) return false;

	const int bpp = depth == 8 ? chans : 1;   // filter byte distance
	const size_t stride = ((size_t)w * chans * depth + 7) / 8;
	std::vector<uint8_t> raw((stride + 1) * h);
	uLongf rawLen = raw.size();
	if (uncompress(raw.data(), &rawLen, z.data(), z.size()) != Z_OK || rawLen != raw.size()) return false;

	std::vector<uint8_t> prev(stride, 0), cur(stride);
	rgba.assign((size_t)w * h * 4, 0);
	for (int y = 0; y < h; y++) {
		const uint8_t f = raw[y * (stride + 1)];
		const uint8_t *src = &raw[y * (stride + 1) + 1];
		for (size_t i = 0; i < stride; i++) {
			const int a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0;
			int v = src[i];
			switch (f) {
			case 0: break;
			case 1: v += a; break;
			case 2: v += b; break;
			case 3: v += (a + b) >> 1; break;
			case 4: v += paeth(a, b, c); break;
			default: return false;
			}
			cur[i] = (uint8_t)v;
		}
		for (int x = 0; x < w; x++) {
			uint8_t *o = &rgba[((size_t)y * w + x) * 4];
			if (depth < 8) {
				const int per = 8 / depth, shift = 8 - depth * (x % per + 1);
				const int v = (cur[x / per] >> shift) & ((1 << depth) - 1);
				if (ctype == 3) memcpy(o, pal[v], 4);
				else {
					const uint8_t g = (uint8_t)(v * 255 / ((1 << depth) - 1));
					o[0] = o[1] = o[2] = g; o[3] = v == trnsGrey ? 0 : 255;
				}
				continue;
			}
			const uint8_t *s = &cur[x * chans];
			switch (ctype) {
			case 0: o[0] = o[1] = o[2] = s[0]; o[3] = s[0] == trnsGrey ? 0 : 255; break;
			case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
			case 3: memcpy(o, pal[s[0]], 4); break;
			case 4: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
			case 6: memcpy(o, s, 4); break;
			}
		}
		prev.swap(cur);
	}
	return true;
}

bool libPngLoad(const char *path, std::vector<uint8_t> &rgba, int &w, int &h)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	std::vector<uint8_t> buf;
	uint8_t tmp[4096];
	size_t n;
	while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0 && buf.size() < (1u << 20))
		buf.insert(buf.end(), tmp, tmp + n);
	fclose(f);
	return libPngDecode(buf.data(), buf.size(), rgba, w, h);
}
