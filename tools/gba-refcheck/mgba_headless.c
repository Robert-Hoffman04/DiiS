/* Headless mGBA runner used as the GBA reference emulator for
 * docs/PLAN.md §4.3 item 8 / §4.1 reference-first methodology.
 *
 * Drives libmgba's public mCore API directly (mCoreFind/loadROM/runFrame/
 * getPixels/readRegister) -- no Qt, no SDL, no display, no event loop.
 * This is pure I/O plumbing around mGBA's own public library API: it does
 * not modify, patch, or reimplement any mGBA CPU/PPU/APU/timer code.
 *
 * Usage: mgba_headless <rom.gba> <frames> <out.ppm>
 *   Runs <frames> video frames, then dumps the 240x160 framebuffer as a
 *   plain PPM (P6) to <out.ppm> and prints ARM7 register state (r0-r15,
 *   cpsr) plus a few PPU/DISPCNT-adjacent I/O registers to stdout as
 *   "key=value" lines for easy diffing against the Dolphin harness side.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/blip_buf.h>
#include <mgba-util/vfs.h>

/* Matches struct ARMCore's layout closely enough for gprs[]/cpsr access
 * without pulling in the internal arm.h (which drags in decoder tables
 * not needed here). gprs[16] + a union PSR (32-bit) is the core->cpu
 * layout for both GB and GBA platforms in this libmgba version. */
struct MinimalARMCore {
	int32_t gprs[16];
	uint32_t cpsr; /* union PSR is a single 32-bit word in this ABI */
};

static void nullLogger(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	(void)logger; (void)category; (void)level; (void)format; (void)args;
}

int main(int argc, char** argv) {
	if (argc != 4 && argc != 5) {
		fprintf(stderr, "usage: %s <rom.gba> <frames> <out.ppm> [out.pcm]\n", argv[0]);
		fprintf(stderr, "  out.pcm (optional, PLAN.md §4.3 item 3): raw interleaved\n"
			"  stereo s16le PCM, drained via mCore's public getAudioChannel()/\n"
			"  blip_buf API once per runFrame() -- final mixed DirectSound+PSG\n"
			"  output at mGBA's own internal sample rate, not resampled further.\n");
		return 2;
	}
	const char* romPath = argv[1];
	int frames = atoi(argv[2]);
	const char* outPath = argv[3];
	const char* pcmPath = argc == 5 ? argv[4] : NULL;

	static struct mLogger logger = { .log = nullLogger };
	mLogSetDefaultLogger(&logger);

	struct mCore* core = mCoreFind(romPath);
	if (!core) {
		fprintf(stderr, "mCoreFind: unrecognized/unsupported file: %s\n", romPath);
		return 1;
	}
	if (!core->init(core)) {
		fprintf(stderr, "core->init failed\n");
		return 1;
	}
	mCoreInitConfig(core, NULL);

	unsigned width = 0, height = 0;
	core->desiredVideoDimensions(core, &width, &height);
	color_t* vbuf = calloc((size_t)width * height, sizeof(color_t));
	core->setVideoBuffer(core, vbuf, width);

	/* PLAN.md §4.3 item 3 (APU): pure I/O plumbing added on top of the
	 * existing headless driver -- taps mCore's public audio API, no PPU/
	 * CPU/APU code touched. getAudioChannel(core, 0/1) returns the two
	 * blip_t* stereo output buffers (left/right) that every mGBA platform
	 * core feeds its final mixed audio into; draining them with
	 * blip_read_samples() after each runFrame() gives the actual DirectSound
	 * + PSG mixed PCM stream mGBA would hand to a real audio backend. */
	if (pcmPath) {
		core->setAudioBufferSize(core, 8192);
	}

	if (!mCoreLoadFile(core, romPath)) {
		fprintf(stderr, "mCoreLoadFile failed: %s\n", romPath);
		return 1;
	}
	core->reset(core);

	FILE* pcmFile = NULL;
	blip_t* chanL = NULL;
	blip_t* chanR = NULL;
	long totalSamples = 0;
	short minSample = 0, maxSample = 0;
	if (pcmPath) {
		pcmFile = fopen(pcmPath, "wb");
		if (!pcmFile) { fprintf(stderr, "fopen %s failed\n", pcmPath); return 1; }
		chanL = core->getAudioChannel(core, 0);
		chanR = core->getAudioChannel(core, 1);
	}

	for (int i = 0; i < frames; i++) {
		core->runFrame(core);

		if (pcmFile && chanL && chanR) {
			int avail = blip_samples_avail(chanL);
			int availR = blip_samples_avail(chanR);
			if (availR < avail) avail = availR;
			if (avail > 0) {
				short* tmp = malloc(sizeof(short) * 2 * avail);
				/* Interleave: left into even slots, right into odd slots --
				 * blip_read_samples(..., stereo=1) writes every other
				 * element starting at out[0], so offsetting the right
				 * channel's out pointer by 1 short interleaves the two. */
				blip_read_samples(chanL, tmp, avail, 1);
				blip_read_samples(chanR, tmp + 1, avail, 1);
				fwrite(tmp, sizeof(short), (size_t)avail * 2, pcmFile);
				for (int s = 0; s < avail * 2; s++) {
					if (tmp[s] < minSample) minSample = tmp[s];
					if (tmp[s] > maxSample) maxSample = tmp[s];
				}
				totalSamples += avail;
				free(tmp);
			}
		}
	}

	if (pcmFile) {
		fclose(pcmFile);
		fprintf(stderr, "wrote %s (%ld stereo samples, min=%d max=%d)\n",
			pcmPath, totalSamples, minSample, maxSample);
	}

	/* PPM dump (P6, 8-bit RGB). color_t default layout on this build
	 * (32-bit, non-16-bit-color) is byte0=R,byte1=G,byte2=B per
	 * mgba/core/interface.h's M_RGB5_TO_BGR8 + low-bit-fill convention. */
	FILE* f = fopen(outPath, "wb");
	if (!f) { fprintf(stderr, "fopen %s failed\n", outPath); return 1; }
	fprintf(f, "P6\n%u %u\n255\n", width, height);
	for (unsigned y = 0; y < height; y++) {
		for (unsigned x = 0; x < width; x++) {
			color_t c = vbuf[y * width + x];
			unsigned char rgb[3] = {
				(unsigned char)(c & 0xFF),
				(unsigned char)((c >> 8) & 0xFF),
				(unsigned char)((c >> 16) & 0xFF),
			};
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);

	struct MinimalARMCore* cpu = (struct MinimalARMCore*)core->cpu;
	printf("frames=%d\n", frames);
	for (int i = 0; i < 16; i++) {
		printf("r%d=0x%08X\n", i, (uint32_t)cpu->gprs[i]);
	}
	printf("cpsr=0x%08X\n", cpu->cpsr);

	/* A handful of GBA I/O registers via the generic bus-read hook, for
	 * direct comparison with the Dolphin-harness/DeSmuME side's own
	 * register dump (same addresses, same meaning per GBATEK). */
	size_t ioSize = 0;
	void* io = mCoreGetMemoryBlock(core, 0x04000000, &ioSize);
	if (io && ioSize >= 0x60) {
		uint16_t* io16 = (uint16_t*)io;
		printf("DISPCNT=0x%04X\n", io16[0x00 / 2]);
		printf("DISPSTAT=0x%04X\n", io16[0x04 / 2]);
		printf("VCOUNT=0x%04X\n", io16[0x06 / 2]);
		printf("BG0CNT=0x%04X\n", io16[0x08 / 2]);
		printf("KEYINPUT=0x%04X\n", io16[0x130 / 2]);
	}

	core->deinit(core);
	free(vbuf);
	fprintf(stderr, "wrote %s (%ux%u)\n", outPath, width, height);
	return 0;
}
