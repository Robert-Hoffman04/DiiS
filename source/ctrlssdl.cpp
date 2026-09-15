/* 
    Copyright (C) 2007 Pascal Giard
    Copyright (C) 2012 DeSmuMEWii team

    This file is part of DeSmuMEWii

    DeSmuMEWii is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    DeSmuMEWii is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DeSmuMEWii; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "ctrlssdl.h"
#include "harness/harness.h"
#include "saves.h"
#include "SPU.h"
#include "NDSSystem.h"

//u16 keyboard_cfg[NB_KEYS];
//u16 joypad_cfg[NB_KEYS];
u16 wiimote_cfg[NB_KEYS];
u16 gamecube_cfg[NB_KEYS];
//u16 nbr_joy;
cursor_status cursor;

// Bottom (touch) screen quad geometry, set by do_screen_layout() in main.cpp
// (FrontEnd.h). WPAD_SetVRes() reports the IR pointer in full Wii video-mode
// pixel space, not DS touch-screen space, so the IR override below has to
// invert the same transform draw_thread() uses to place the quad.
extern int bottomX, bottomY;
extern float scalex, scaley;

/* Keypad key names */
const char *key_names[NB_KEYS] =
{
  "A", "B", "Select", "Start",
  "Right", "Left", "Up", "Down",
  "R", "L", "X", "Y",
  "Debug", "Boost"
};

/* Default joypad configuration 
const u16 default_joypad_cfg[NB_KEYS] =
  { 1,  // A
    0,  // B
    5,  // select
    8,  // start
    256, // Right -- Start cheating abit...
    256, // Left
    512, // Up
    512, // Down  -- End of cheating.
    7,  // R
    6,  // L
    4,  // X
    3,  // Y
    -1, // DEBUG
    -1  // BOOST
  };*/

/*const u16 plain_keyboard_cfg[NB_KEYS] =
{
    'x',         // A
    'z',         // B
    'y',         // select
    'u',         // start
    'l',         // Right
    'j',         // Left
    'i',         // Up
    'k',         // Down
    'w',         // R
    'q',         // L
    's',         // X
    'a',         // Y
    'p',         // DEBUG
    'o'          // BOOST
};*/

const u32 default_wiimote_cfg[NB_KEYS] =
{
	WPAD_CLASSIC_BUTTON_A,             // A
	WPAD_CLASSIC_BUTTON_B,             // B
	WPAD_CLASSIC_BUTTON_MINUS,         // select
	WPAD_CLASSIC_BUTTON_PLUS,          // start
	WPAD_CLASSIC_BUTTON_RIGHT,         // Right
	WPAD_CLASSIC_BUTTON_LEFT,          // Left
	WPAD_CLASSIC_BUTTON_UP,            // Up
	WPAD_CLASSIC_BUTTON_DOWN,          // Down
	WPAD_CLASSIC_BUTTON_FULL_R,        // R
	WPAD_CLASSIC_BUTTON_FULL_L,        // L
	WPAD_CLASSIC_BUTTON_X,             // X
	WPAD_CLASSIC_BUTTON_Y,             // Y
	WPAD_CLASSIC_BUTTON_ZL,            // DEBUG
	WPAD_CLASSIC_BUTTON_ZR             // BOOST
};

const u32 default_gamecube_cfg[NB_KEYS] =
{
    PAD_BUTTON_A,                      // A
    PAD_BUTTON_B,                      // B
    PAD_BUTTON_RIGHT,                  // select
    PAD_BUTTON_START,                  // start
    0,                                 // Right
    0,                                 // Left
    0,                                 // Up
    0,                                 // Down
    PAD_TRIGGER_R,                     // R
    PAD_TRIGGER_L,                     // L
    PAD_BUTTON_X,                      // X
    PAD_BUTTON_Y,                      // Y
    0,                                 // DEBUG
    0                                  // BOOST
};

/* Load default joystick and keyboard configurations */
void load_default_config(const u16 kbCfg[])
{
  memcpy(keyboard_cfg, kbCfg, sizeof(keyboard_cfg));
//  memcpy(joypad_cfg, default_joypad_cfg, sizeof(joypad_cfg));
  memcpy(wiimote_cfg, default_wiimote_cfg, sizeof(wiimote_cfg));
  memcpy(gamecube_cfg, default_gamecube_cfg, sizeof(gamecube_cfg));
}

/* Set cursor coordinates */
static void set_cursor_coord(signed long x,signed long y)
{
  if(x<0) x = 0; else if(x>255) x = 255;
  if(y<0) y = 0; else if(y>192) y = 192;
  cursor.x = x;
  cursor.y = y;
}

/* Update NDS keypad */
void update_keypad(u16 keys)
{
#ifdef WORDS_BIGENDIAN
  MMU.ARM9_REG[0x130] = ~keys & 0xFF;
  MMU.ARM9_REG[0x131] = (~keys >> 8) & 0x3;
  MMU.ARM7_REG[0x130] = ~keys & 0xFF;
  MMU.ARM7_REG[0x131] = (~keys >> 8) & 0x3;
#else
  ((u16 *)MMU.ARM9_REG)[0x130>>1] = ~keys & 0x3FF;
  ((u16 *)MMU.ARM7_REG)[0x130>>1] = ~keys & 0x3FF;
#endif
  /* Update X and Y buttons */
  MMU.ARM7_REG[0x136] = ( ~( keys >> 10) & 0x3 ) | (MMU.ARM7_REG[0x136] & ~0x3);

  // GBA mode: this Wii-frontend path (DSExec() -> update_keypad()) writes
  // straight into MMU.ARM9_REG/ARM7_REG, the DS-native KEYINPUT backing --
  // never into MMU.GBA_IOREG, which is what the isGBA memory-map guard
  // routes every ARM7 access to (and what gba_keypad.cpp's IRQ check reads).
  // NDS_applyFinalInput() (NDSSystem.cpp) already does this same mirror for
  // its own input path; this frontend bypasses that path entirely (calls
  // update_keypad() directly instead of NDS_setPad()+begin/endProcessingInput),
  // so without this a GBA game's KEYINPUT never moves and no button (Start
  // included) has any effect. Bit layout is identical between the two --
  // GBA KEYINPUT is bit-for-bit the same active-low A/B/Select/Start/Right/
  // Left/Up/Down/R/L encoding as the DS one `keys` already targets above.
  if (gameInfo.isGBA)
    T1WriteWord(MMU.GBA_IOREG, 0x130, (u16)((~keys & 0x3FF) | 0xFC00));
}

/* Retrieve current NDS keypad */
u16 get_keypad( void)
{
  u16 keypad;
  keypad = ~MMU.ARM7_REG[0x136];
  keypad = (keypad & 0x3) << 10;
#ifdef WORDS_BIGENDIAN
  keypad |= ~(MMU.ARM9_REG[0x130] | (MMU.ARM9_REG[0x131] << 8)) & 0x3FF;
#else
  keypad |= ~((u16 *)MMU.ARM9_REG)[0x130>>1] & 0x3FF;
#endif
  return keypad;
}

#define CHECK_KEY(key, exp1, exp2) { \
	if(exp1 || exp2) \
		ADD_KEY( *keypad, KEYMASK_(key)); \
	else \
		RM_KEY( *keypad, KEYMASK_(key)); \
}

/* Nunchuk specific */
void Check_Nunchuk(WPADData *wpad, u16 *keypad)
{
	int	held_nunchuck = 1;
	u8 nunchuk_dir[4] = {0,0,0,0};

	// left
	if((wpad->exp.nunchuk.js.ang>=270-45 && wpad->exp.nunchuk.js.ang<=270+45) && wpad->exp.nunchuk.js.mag>=0.9)
		nunchuk_dir[0] = held_nunchuck;

	// right
	if((wpad->exp.nunchuk.js.ang>=90-45 && wpad->exp.nunchuk.js.ang<=90+45) && wpad->exp.nunchuk.js.mag>=0.9)
		nunchuk_dir[1] = held_nunchuck;

	// up
	if((wpad->exp.nunchuk.js.ang>=360-45 || wpad->exp.nunchuk.js.ang<=45) && wpad->exp.nunchuk.js.mag>=0.9)
		nunchuk_dir[2] = held_nunchuck;

	// down
	if((wpad->exp.nunchuk.js.ang>=180-45 && wpad->exp.nunchuk.js.ang<=180+45) && wpad->exp.nunchuk.js.mag>=0.9)
		nunchuk_dir[3] = held_nunchuck;

	// up/left
	if((wpad->exp.nunchuk.js.ang>=315-20 && wpad->exp.nunchuk.js.ang<=315+20) && wpad->exp.nunchuk.js.mag>=0.9)
		nunchuk_dir[0] = nunchuk_dir[2] = held_nunchuck;

	//up/right
	if((wpad->exp.nunchuk.js.ang>=45-20 && wpad->exp.nunchuk.js.ang<=45+20) && wpad->exp.nunchuk.js.mag>=0.9)
		nunchuk_dir[1] = nunchuk_dir[2] = held_nunchuck;

	//down/right
	if((wpad->exp.nunchuk.js.ang>=135-20 && wpad->exp.nunchuk.js.ang<=135+20) && wpad->exp.nunchuk.js.mag>=0.9)
		nunchuk_dir[1] = nunchuk_dir[3] = held_nunchuck;

	//down/left
	if((wpad->exp.nunchuk.js.ang>=225-20 && wpad->exp.nunchuk.js.ang<=225+20) && wpad->exp.nunchuk.js.mag>=0.9)
		nunchuk_dir[0] = nunchuk_dir[3] = held_nunchuck;

	CHECK_KEY(KEY_RIGHT, nunchuk_dir[1], 0);
	CHECK_KEY(KEY_LEFT,  nunchuk_dir[0], 0);
	CHECK_KEY(KEY_UP,    nunchuk_dir[2], 0);
	CHECK_KEY(KEY_DOWN,  nunchuk_dir[3], 0);

}

void process_ctrls_event( u16 *keypad, float nds_screen_size_ratio )
{
	u32 type;
    WPADData *wd_one;
    WPAD_Probe(WPAD_CHAN_ALL, &type);
    wd_one = WPAD_Data(0);

	// USB Gecko / EXI debug-serial input (see gekko_utils/geckoinput.h).
	GECKO_Update();
	// §3.5: advance the transport-agnostic remote-input core in lockstep and
	// fold its level mask in alongside the Gecko's. Self-stubs to 0 without
	// -DDESMUME_HARNESS -DHARNESS_INPUT.
	harness_input_update();
	u32 gecko_h = GECKO_ButtonsHeld() | harness_input_held();

	u32 wpad_h = WPAD_ButtonsHeld(0);
	// d-pad bits are applied through the explicit CHECK_KEY() lines below, so
	// keep them out of pad_h (KEY_SELECT is mapped to PAD_BUTTON_RIGHT).
	u32 pad_h = PAD_ButtonsHeld(0) |
		(gecko_h & ~(PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_UP | PAD_BUTTON_DOWN));

	s32 pad_stickx = PAD_StickX(0);
	s32 pad_sticky = PAD_StickY(0);
	s32 pad_substickx = PAD_SubStickX(0);
	s32 pad_substicky = PAD_SubStickY(0);

	for(int i = FIRST_KEY; i <= LAST_KEY; i++) {
		CHECK_KEY(i, wpad_h & default_wiimote_cfg[i], pad_h & default_gamecube_cfg[i]);
	}

	CHECK_KEY(KEY_RIGHT, (pad_stickx > 20)  || (gecko_h & PAD_BUTTON_RIGHT), wpad_h & WPAD_CLASSIC_BUTTON_RIGHT);
	CHECK_KEY(KEY_LEFT,  (pad_stickx < -20) || (gecko_h & PAD_BUTTON_LEFT),  wpad_h & WPAD_CLASSIC_BUTTON_LEFT);
	CHECK_KEY(KEY_UP,    (pad_sticky > 20)  || (gecko_h & PAD_BUTTON_UP),    wpad_h & WPAD_CLASSIC_BUTTON_UP);
	CHECK_KEY(KEY_DOWN,  (pad_sticky < -20) || (gecko_h & PAD_BUTTON_DOWN),  wpad_h & WPAD_CLASSIC_BUTTON_DOWN);

	// Hack ... remove if this seems stupid ? Or remap the buttons for nunchuk.. IDC I use a CC
	if (wd_one->exp.type == EXP_NUNCHUK)
	{
		Check_Nunchuk(wd_one, keypad); // Nunchuk
		CHECK_KEY(KEY_A, wpad_h & WPAD_NUNCHUK_BUTTON_C , 0);
		CHECK_KEY(KEY_B, wpad_h & WPAD_NUNCHUK_BUTTON_Z , 0);
	}

	if ((wpad_h & WPAD_BUTTON_A) || (pad_h & PAD_TRIGGER_Z))
		cursor.down = true;
	  
	if (!(wpad_h & WPAD_BUTTON_A) && !(pad_h & PAD_TRIGGER_Z)) {
		if(cursor.down) {
			cursor.click = true;
			cursor.down = false;
		}
	}

	if ((wpad_h & WPAD_BUTTON_LEFT) || (pad_substickx < -20)){
		--cursor.x;
	} 

	if ((wpad_h & WPAD_BUTTON_RIGHT) || (pad_substickx > 20)){
		++cursor.x;
	} 

	if ((wpad_h & WPAD_BUTTON_DOWN) || (pad_substicky < -20)) {
		++cursor.y;
	} 
		
	if ((wpad_h & WPAD_BUTTON_UP) || (pad_substicky > 20)){
		--cursor.y;
	}
	// WiiMote cursor co-ords. ir.x/ir.y come back in Wii video-mode pixel
	// space (see WPAD_SetVRes() in main.cpp), while cursor.x/y are DS
	// touch-screen coordinates (0..255 x 0..191) -- invert the quad
	// placement transform from do_screen_layout()/draw_thread() to convert.
	// Previously this assigned ir.x/ir.y straight into cursor.x/y: on any
	// layout where the touch quad isn't pinned to the TV's top-left corner
	// (i.e. every normal layout) that clamps to a fixed corner regardless of
	// where the pointer actually is, and it did so unconditionally every
	// frame -- silently overwriting whatever the GC C-stick/D-pad path above
	// had just set, which is why neither input method appeared to move the
	// cursor at all.
	if (wd_one->ir.valid && scalex != 0.0f && scaley != 0.0f){
		signed long ir_x = (signed long)(wd_one->ir.x / scalex) - bottomX;
		signed long ir_y = (signed long)(wd_one->ir.y / scaley) - bottomY;
		if (ir_x >= 0 && ir_x <= 255 && ir_y >= 0 && ir_y <= 192){
			cursor.x = ir_x;
			cursor.y = ir_y;
		}
	}

	set_cursor_coord( cursor.x, cursor.y );  

}

