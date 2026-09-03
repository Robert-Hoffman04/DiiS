/*
    geckoinput.h - route controller input into DeSmuME Wii through the
    USB Gecko / EXI "debug serial" channel (EXI channel 1 / memory-card slot B).

    Dolphin emulates a USB Gecko when Slot B is set to "USB Gecko"; it exposes a
    TCP server (default 127.0.0.1:0xd6ec) that a host-side script can connect to.
    Bytes written to that socket arrive here; we translate them into synthetic
    GameCube-pad button presses so the existing GetInput()/GetHeld() paths and
    process_ctrls_event() pick them up without any other changes.

    Wire protocol (one byte per event, case-sensitive):
        a b x y      -> A / B / X / Y
        s            -> START
        u d l r      -> D-pad up / down / left / right
        z            -> Z trigger (in game: touch-screen tap)
        L R          -> L / R triggers
        newline, CR and space are ignored

    Each received byte "taps" that button: it is reported as held for
    GECKO_TAP_FRAMES successive GECKO_Update() calls, then auto-released. Send the
    same byte again to repeat (e.g. "ddddda" to move down five times then pick).

    All masks use libogc PAD_BUTTON_* / PAD_TRIGGER_* bit values so they can be
    AND-ed directly against the pad constants the rest of the code already uses.
*/

#ifndef GECKOINPUT_H
#define GECKOINPUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Detect a USB Gecko on EXI channel 1. Safe to call before video is up. */
void GECKO_InputInit(void);

/* Poll the wire and advance the tap timers. Call once per input-loop iteration
   (next to PAD_ScanPads()/WPAD_ScanPads()). Cheap no-op when no Gecko present. */
void GECKO_Update(void);

/* Buttons that became held during the most recent GECKO_Update() (edge). */
unsigned int GECKO_ButtonsDown(void);

/* Buttons currently held via an unexpired tap (level). */
unsigned int GECKO_ButtonsHeld(void);

/* Non-zero once a Gecko has been detected. */
int GECKO_Available(void);

#ifdef __cplusplus
}
#endif

#endif /* GECKOINPUT_H */
