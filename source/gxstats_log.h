/****************************************************************************
 * DeSmuMEWii - gxstats_log.h
 *
 * Task hw-measure: a harness-less sink for the harness_profile_emit[f]() stat
 * lines (the GX gate/bail counters: -DDSB_STATS "dsa"/"dsaw"/"dsb"/"dsbw"...,
 * -DDSA_GXGEOM_TEXSTATS "gxds3dstats ... gates:", -DDSLZ_STATS, ...), so they can
 * be collected on real hardware. -DDESMUME_GXSTATS routes those lines into a RAM
 * buffer that gxStatsFrameTick() appends to sd:/gxstats.log once per 60 frames,
 * each block preceded by a "# gxstats frame=<n>" marker. Under -DDESMUME_HARNESS
 * the harness keeps the lines (PKT_PROFILE) and this is a no-op.
 *
 * Emulation-thread only (the stat emitters all run there). Compiled out
 * otherwise.
 ***************************************************************************/
#ifndef DESMUME_GXSTATS_LOG_H
#define DESMUME_GXSTATS_LOG_H

#if defined(DESMUME_GXSTATS) && !defined(DESMUME_HARNESS)
void gxStatsFrameTick(void);
#else
static inline void gxStatsFrameTick(void) {}
#endif

#endif // DESMUME_GXSTATS_LOG_H
