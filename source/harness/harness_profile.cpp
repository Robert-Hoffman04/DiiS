/*
    harness_profile.cpp - see harness_profile.h. Compiles to nothing without
    -DDESMUME_HARNESS (verify: `nm ... | grep harness_profile` empty).
*/
#include "harness.h"          // pulls the sub-flag fan-out + harness_transport.h
#include "harness_profile.h"

#ifdef DESMUME_HARNESS

#include "harness_wire.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

void harness_profile_log(const char *line)
{
	if (!line || !*line) return;
	harness_send(HARNESS_PKT_LOG, line, (u32)strlen(line));
}

#ifdef HARNESS_PROFILE

void harness_profile_emit(const char *text)
{
	if (!text || !*text) return;
	harness_send(HARNESS_PKT_PROFILE, text, (u32)strlen(text));
}

void harness_profile_emitf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0) return;
	u32 len = ((size_t)n < sizeof(buf)) ? (u32)n : (u32)(sizeof(buf) - 1);
	harness_send(HARNESS_PKT_PROFILE, buf, len);
}

#endif // HARNESS_PROFILE

#endif // DESMUME_HARNESS
