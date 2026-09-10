/*
    harness_boot.h - boot manifest + multi-ROM playlist + watchdog (plan §3.4).

    A single text file - sd:/harness.cfg (or usb:/harness.cfg, or pushed as a
    companion file over wiiload) - replaces the -DDESMUME_FORCE_ROM /
    -DDESMUME_FORCE_CORE / -DDESMUME_FORCE_USB compile-time flags:

        core=1                 # 1=GX, 2=software raster   (optional)
        usb=0                  # 0=SD, 1=USB               (optional)
        frames_per_rom=1800    # advance after N frames    (0 = never, manual)
        autoload_slot=0        # loadstate_slot() once, early (optional, -1=off)
        frame_every=0          # PKT_FRAME every N frames  (0 = off)
        rom=sd:/DS/ROMS/test1.nds
        rom=sd:/DS/ROMS/test2.nds

    With >=1 rom= line the harness drives the per-ROM boot path in a loop,
    advancing after frames_per_rom or on a PKT_CTRL "next_rom", tagging every
    heartbeat with the current ROM. No manifest -> every accessor is inert and
    main() falls through to the existing FileBrowser path unchanged.

    Follows the perf_zones.h pattern: real bodies under DESMUME_HARNESS &&
    HARNESS_BOOT, static-inline no-ops otherwise, so call sites never #ifdef.
*/
#ifndef HARNESS_BOOT_H
#define HARNESS_BOOT_H

#include <gctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(DESMUME_HARNESS) && defined(HARNESS_BOOT)

// Parse "<mount>/harness.cfg" (mount = "sd:" or "usb:"). Safe to call once,
// early, right where the -DDESMUME_FORCE_ROM branch used to sit. Returns the
// number of rom= entries found (0 = no usable manifest).
int  harness_boot_load(const char *mount);

int  harness_boot_have(void);                 // 1 if a manifest with >=1 rom
const char *harness_boot_rom_path(void);       // current ROM's full path
const char *harness_boot_rom_name(void);       // basename, for report tags
int  harness_boot_rom_index(void);             // 0-based
int  harness_boot_rom_count(void);
u32  harness_boot_frames_per_rom(void);        // 0 = manual advance only
int  harness_boot_core(void);                  // -1 if unset
int  harness_boot_usb(void);                   // -1 if unset
int  harness_boot_autoload_slot(void);         // -1 if unset
u32  harness_boot_frame_every(void);           // 0 = off

// Called by the Execute() playlist loop. Requests the next ROM (next_rom CTRL);
// harness_boot_advance() loads it via NDS_LoadROM and returns 1, or 0 when the
// playlist is exhausted (caller then quits as before).
void harness_boot_request_next(void);
int  harness_boot_next_requested(void);
int  harness_boot_advance(void);

// Emit "rom=<name>" once as a PKT_CTRL so the host tags subsequent output.
void harness_boot_announce(void);

#else // !(_HARNESS && HARNESS_BOOT)

static inline int  harness_boot_load(const char *m) { (void)m; return 0; }
static inline int  harness_boot_have(void) { return 0; }
static inline const char *harness_boot_rom_path(void) { return 0; }
static inline const char *harness_boot_rom_name(void) { return ""; }
static inline int  harness_boot_rom_index(void) { return 0; }
static inline int  harness_boot_rom_count(void) { return 0; }
static inline u32  harness_boot_frames_per_rom(void) { return 0; }
static inline int  harness_boot_core(void) { return -1; }
static inline int  harness_boot_usb(void) { return -1; }
static inline int  harness_boot_autoload_slot(void) { return -1; }
static inline u32  harness_boot_frame_every(void) { return 0; }
static inline void harness_boot_request_next(void) {}
static inline int  harness_boot_next_requested(void) { return 0; }
static inline int  harness_boot_advance(void) { return 0; }
static inline void harness_boot_announce(void) {}

#endif

#ifdef __cplusplus
}
#endif

#endif // HARNESS_BOOT_H
