/*
    harness_wire.h - device-side mirror of tools/harness-control/wire.py.

    Framed packet protocol (plan §2). Fixed 10-byte big-endian header:

        magic (4) | version (1) | packet_type (1) | payload_length (u32) | payload

    The Wii is big-endian, so the u32 length goes on the wire as-is.
    KEEP THIS IN SYNC WITH wire.py.
*/
#ifndef HARNESS_WIRE_H
#define HARNESS_WIRE_H

#include <gctypes.h>

#define HARNESS_WIRE_MAGIC0 'D'
#define HARNESS_WIRE_MAGIC1 'S'
#define HARNESS_WIRE_MAGIC2 'M'
#define HARNESS_WIRE_MAGIC3 'H'
#define HARNESS_WIRE_VERSION 1
#define HARNESS_WIRE_HEADER_LEN 10

enum {
	HARNESS_PKT_LOG     = 0x01, // device -> host  UTF-8 text line
	HARNESS_PKT_FRAME   = 0x02, // device -> host  framebuffer capture
	HARNESS_PKT_INPUT   = 0x03, // host -> device  geckoinput alphabet bytes
	HARNESS_PKT_CTRL    = 0x04, // both            short text command
	HARNESS_PKT_PROFILE = 0x05, // device -> host  zone / cache-stat rows
	HARNESS_PKT_CRASH   = 0x06, // device -> host  register dump + backtrace
	HARNESS_PKT_ASSERT  = 0x07, // device -> host  pass/fail bit + message
};

// FRAME payload sub-header formats (see wire.py FRAME_FMT_*).
enum {
	HARNESS_FRAME_FMT_RGB565   = 0,
	HARNESS_FRAME_FMT_RGBA8888 = 1,
	HARNESS_FRAME_FMT_BGRA8888 = 2,
};

#endif // HARNESS_WIRE_H
