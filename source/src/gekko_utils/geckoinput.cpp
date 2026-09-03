/*
    geckoinput.cpp - see geckoinput.h

    Uses the non-blocking libogc primitives usb_isgeckoalive()/usb_recvbuffer():
    usb_recvbuffer() returns immediately with however many bytes are already in
    the Gecko RX FIFO (0 if none), so it is safe to call from a render loop.
    Dropped bytes just mean a missed key press - resend.
*/

#include <gccore.h>
#include <ogc/usbgecko.h>
#include <string.h>

#include "geckoinput.h"

/* EXI channel the Gecko lives on. Dolphin: Slot B. Matches log_console.cpp. */
#define GECKO_CHAN        1

/* How many GECKO_Update() calls a single received byte stays "held". At the
   ~60 Hz the menus and game loop poll, 6 frames is ~100 ms - long enough for an
   edge-triggered GetInput() and a couple of GetHeld() reads to see it. */
#define GECKO_TAP_FRAMES  6

/* index into the timer table; keep small and dense */
enum {
    GB_A, GB_B, GB_X, GB_Y,
    GB_START,
    GB_UP, GB_DOWN, GB_LEFT, GB_RIGHT,
    GB_Z, GB_L, GB_R,
    GB_COUNT
};

static const unsigned int gb_mask[GB_COUNT] = {
    PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X, PAD_BUTTON_Y,
    PAD_BUTTON_START,
    PAD_BUTTON_UP, PAD_BUTTON_DOWN, PAD_BUTTON_LEFT, PAD_BUTTON_RIGHT,
    PAD_TRIGGER_Z, PAD_TRIGGER_L, PAD_TRIGGER_R
};

static int          g_alive       = 0;
static unsigned int g_frame       = 0;
static unsigned int g_hold_until[GB_COUNT];   /* frame index the tap expires on */
static unsigned int g_held        = 0;        /* level mask, last Update */
static unsigned int g_down        = 0;        /* edge mask, last Update */

static int byte_to_slot(unsigned char c)
{
    switch (c) {
        case 'a': return GB_A;
        case 'b': return GB_B;
        case 'x': return GB_X;
        case 'y': return GB_Y;
        case 's': return GB_START;
        case 'u': return GB_UP;
        case 'd': return GB_DOWN;
        case 'l': return GB_LEFT;
        case 'r': return GB_RIGHT;
        case 'z': return GB_Z;
        case 'L': return GB_L;
        case 'R': return GB_R;
        default:  return -1;      /* whitespace / unknown -> ignore */
    }
}

void GECKO_InputInit(void)
{
    memset(g_hold_until, 0, sizeof(g_hold_until));
    g_frame = 0;
    g_held  = 0;
    g_down  = 0;
    g_alive = usb_isgeckoalive(GECKO_CHAN);
}

void GECKO_Update(void)
{
    unsigned int prev_held = g_held;
    unsigned int held = 0;
    int i;

    if (!g_alive) {
        /* Re-probe occasionally in case the host attached the Gecko late. */
        if ((g_frame++ & 0x3f) == 0)
            g_alive = usb_isgeckoalive(GECKO_CHAN);
        g_held = g_down = 0;
        return;
    }

    g_frame++;

    /* Drain whatever is waiting - non-blocking. */
    for (;;) {
        unsigned char buf[64];
        int n = usb_recvbuffer(GECKO_CHAN, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (i = 0; i < n; i++) {
            int slot = byte_to_slot(buf[i]);
            if (slot >= 0)
                g_hold_until[slot] = g_frame + GECKO_TAP_FRAMES;
        }
        if (n < (int)sizeof(buf))
            break;
    }

    for (i = 0; i < GB_COUNT; i++) {
        if (g_hold_until[i] > g_frame)
            held |= gb_mask[i];
    }

    g_held = held;
    g_down = held & ~prev_held;
}

unsigned int GECKO_ButtonsDown(void) { return g_down; }
unsigned int GECKO_ButtonsHeld(void) { return g_held; }
int          GECKO_Available(void)   { return g_alive; }
