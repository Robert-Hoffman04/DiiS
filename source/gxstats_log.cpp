/****************************************************************************
 * DeSmuMEWii - gxstats_log.cpp   (see gxstats_log.h)
 *
 * Compiled to nothing unless -DDESMUME_GXSTATS without -DDESMUME_HARNESS.
 ***************************************************************************/
#include "gxstats_log.h"

#if defined(DESMUME_GXSTATS) && !defined(DESMUME_HARNESS)

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "harness/harness_profile.h"

#ifndef DESMUME_GXSTATS_BLOCK
#define DESMUME_GXSTATS_BLOCK 60
#endif

// One block's lines. A full buffer is written out early rather than dropping lines.
static char s_buf[32768];
static u32  s_len;
static u32  s_frame;
static bool s_truncated;   // first write of the run: "w" (fresh log), then "a"

static void gxs_flush(void)
{
	FILE *f = fopen("sd:/gxstats.log", s_truncated ? "a" : "w");
	s_truncated = true;
	if (!f) { s_len = 0; return; }
	fprintf(f, "# gxstats frame=%u\n", (unsigned)s_frame);
	if (s_len) fwrite(s_buf, 1, s_len, f);
	fclose(f);
	s_len = 0;
}

void harness_profile_emit(const char *text)
{
	if (!text || !*text) return;
	u32 n = (u32)strlen(text);
	if (n + 1 > sizeof(s_buf)) return;
	if (s_len + n + 1 > sizeof(s_buf)) gxs_flush();
	memcpy(s_buf + s_len, text, n);
	s_len += n;
	if (text[n - 1] != '\n') s_buf[s_len++] = '\n';
}

void harness_profile_emitf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0) return;
	harness_profile_emit(buf);
}

void gxStatsFrameTick(void)
{
	if (++s_frame % DESMUME_GXSTATS_BLOCK == 0) gxs_flush();
}

#endif
