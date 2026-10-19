/****************************************************************************
 * DiiS - xfb.h
 *
 * The display framebuffers (XFBs) are XFB_W = 704 wide, which the VI scans
 * out across the full visible width of a TV line (a 640-wide picture leaves
 * a black bar either side). GX's EFB can't be wider than 640, so everything
 * is still rendered at 640 (rmode) and copied into the middle of the XFB
 * (xfbCopyDst); the XFB_PAD columns either side are filled by the CPU, once
 * per screen, since nothing animates there: black in game, the menu's
 * backdrop continued in the menu (gui_draw.cpp).
 ***************************************************************************/
#ifndef DIIS_XFB_H
#define DIIS_XFB_H

#include <gccore.h>

#define XFB_W 704
#define XFB_PAD ((XFB_W - 640) / 2)   // columns either side of the 640 picture
#define XFB_YUYV_BLACK 0x10801080

extern unsigned int *xfb[2];
extern GXRModeObj viMode;   // rmode, XFB_W wide: what the VI and the XFBs use

// GX_CopyDisp() destination: the XFB's middle 640 columns.
static inline void *xfbCopyDst(int i) { return (u8 *)xfb[i] + XFB_PAD * 2; }

// Fill XFB i's edge columns with one colour (YUYV pair).
void xfbFillEdges(int i, u32 yuyv);

#endif // DIIS_XFB_H
