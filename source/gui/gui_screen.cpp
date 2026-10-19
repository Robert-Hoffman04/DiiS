/****************************************************************************
 * DiiS - gui_screen.cpp   (see gui.h)
 *
 * Controller input for the GUI, the focus model and the screen stack / run
 * loop. Input reads the same libogc sources as the game loop (WPAD incl.
 * Nunchuk / Classic expansions, PAD, and USB Gecko taps -- geckoinput.h),
 * but maps them to menu buttons instead of DS keys.
 ***************************************************************************/
#include <stdlib.h>
#include <math.h>
#include <wiiuse/wpad.h>

#include "gui_internal.h"
#include "../gekko_utils/geckoinput.h"
#include "../xfb.h"
#include "../log_console.h"

extern GXRModeObj *rmode;
bool sys_exit_requested();   // main.cpp: console Reset / Power, Wiimote power

//---------------------------------------------------------------------------
// Input
//---------------------------------------------------------------------------
#define REPEAT_DELAY 20      // frames before a held direction repeats
#define REPEAT_EVERY 5
#define POINTER_IDLE 180     // frames an analog-moved pointer stays shown

static GuiInput s_in;
static int s_dirFrames[4];
static int s_pointerIdle;
static float s_pfx = GUI_W / 2, s_pfy = GUI_H / 2;

const GuiInput &guiInput() { return s_in; }

// Analog stick (angle in degrees, 0 = up, clockwise; magnitude 0..1) -> direction bits.
static u32 stickDirs(float ang, float mag)
{
	if (mag < 0.5f)
		return 0;
	u32 d = 0;
	if (ang >= 300 || ang <= 60) d |= GUI_BTN_UP;
	if (ang >= 30 && ang <= 150) d |= GUI_BTN_RIGHT;
	if (ang >= 120 && ang <= 240) d |= GUI_BTN_DOWN;
	if (ang >= 210 && ang <= 330) d |= GUI_BTN_LEFT;
	return d;
}

static float axisSpeed(int v)   // GC stick units -> pointer px/frame
{
	const float kDead = 20.0f, kFull = 80.0f, kMax = 4.0f;
	const float a = (float)abs(v);
	if (a <= kDead)
		return 0.0f;
	const float t = a >= kFull ? 1.0f : (a - kDead) / (kFull - kDead);
	return (v < 0 ? -t : t) * kMax;
}

void guiPollInput()
{
	PAD_ScanPads();
	WPAD_ScanPads();
	GECKO_Update();

	const u32 wh = WPAD_ButtonsHeld(0);
	const u32 ph = PAD_ButtonsHeld(0) | GECKO_ButtonsHeld();
	WPADData *wd = WPAD_Data(0);

	u32 held = 0;
	if ((wh & (WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A)) || (ph & PAD_BUTTON_A)) held |= GUI_BTN_A;
	if ((wh & (WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B)) || (ph & PAD_BUTTON_B)) held |= GUI_BTN_B;
	if ((wh & (WPAD_BUTTON_1 | WPAD_CLASSIC_BUTTON_X)) || (ph & PAD_BUTTON_X)) held |= GUI_BTN_X;
	if ((wh & (WPAD_BUTTON_2 | WPAD_CLASSIC_BUTTON_Y)) || (ph & PAD_BUTTON_Y)) held |= GUI_BTN_Y;
	if ((wh & (WPAD_CLASSIC_BUTTON_FULL_L | WPAD_CLASSIC_BUTTON_ZL)) || (ph & PAD_TRIGGER_L)) held |= GUI_BTN_L;
	if ((wh & (WPAD_CLASSIC_BUTTON_FULL_R | WPAD_CLASSIC_BUTTON_ZR)) || (ph & PAD_TRIGGER_R)) held |= GUI_BTN_R;
	if ((wh & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) || (ph & PAD_BUTTON_START)) held |= GUI_BTN_PLUS;
	if ((wh & (WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_MINUS)) || (ph & PAD_TRIGGER_Z)) held |= GUI_BTN_MINUS;
	// GC has no Home: Z+L+R together, as in game.
	if ((wh & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) ||
	    ((ph & (PAD_TRIGGER_Z | PAD_TRIGGER_L | PAD_TRIGGER_R)) == (PAD_TRIGGER_Z | PAD_TRIGGER_L | PAD_TRIGGER_R)))
		held |= GUI_BTN_HOME;

	u32 dirs = 0;
	if ((wh & (WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP)) || (ph & PAD_BUTTON_UP)) dirs |= GUI_BTN_UP;
	if ((wh & (WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN)) || (ph & PAD_BUTTON_DOWN)) dirs |= GUI_BTN_DOWN;
	if ((wh & (WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_LEFT)) || (ph & PAD_BUTTON_LEFT)) dirs |= GUI_BTN_LEFT;
	if ((wh & (WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_RIGHT)) || (ph & PAD_BUTTON_RIGHT)) dirs |= GUI_BTN_RIGHT;
	const int sx = PAD_StickX(0), sy = PAD_StickY(0);
	if (sx > 40) dirs |= GUI_BTN_RIGHT;
	if (sx < -40) dirs |= GUI_BTN_LEFT;
	if (sy > 40) dirs |= GUI_BTN_UP;
	if (sy < -40) dirs |= GUI_BTN_DOWN;
	if (wd->exp.type == WPAD_EXP_NUNCHUK)
		dirs |= stickDirs(wd->exp.nunchuk.js.ang, wd->exp.nunchuk.js.mag);
	if (wd->exp.type == WPAD_EXP_CLASSIC)
		dirs |= stickDirs(wd->exp.classic.ljs.ang, wd->exp.classic.ljs.mag);
	held |= dirs;

	u32 down = held & ~s_in.held & ~(GUI_BTN_UP | GUI_BTN_DOWN | GUI_BTN_LEFT | GUI_BTN_RIGHT);
	static const u32 kDir[4] = { GUI_BTN_UP, GUI_BTN_DOWN, GUI_BTN_LEFT, GUI_BTN_RIGHT };
	for (int i = 0; i < 4; i++) {
		if (!(dirs & kDir[i])) {
			s_dirFrames[i] = 0;
			continue;
		}
		const int n = s_dirFrames[i]++;
		if (n == 0 || (n >= REPEAT_DELAY && (n - REPEAT_DELAY) % REPEAT_EVERY == 0))
			down |= kDir[i];
	}
	s_in.down = down;
	s_in.held = held;

	// Pointer: Wiimote IR when it's on the screen; else the GC C-stick /
	// Classic right stick move it, and it hides again after a while or as
	// soon as the D-pad is used.
	if (wd->ir.valid) {
		s_pfx = (wd->ir.x - XFB_PAD) * GUI_W / 640;   // IR spans the XFB_W-wide line; the canvas its middle 640
		s_pfy = wd->ir.y * GUI_H / rmode->viHeight;
		s_pointerIdle = POINTER_IDLE;
	} else {
		float dx = axisSpeed(PAD_SubStickX(0)), dy = -axisSpeed(PAD_SubStickY(0));
		if (wd->exp.type == WPAD_EXP_CLASSIC && wd->exp.classic.rjs.mag > 0.2f) {
			const float a = wd->exp.classic.rjs.ang * (float)M_PI / 180.0f, m = wd->exp.classic.rjs.mag;
			dx += sinf(a) * m * 4.0f;
			dy -= cosf(a) * m * 4.0f;
		}
		if (dx != 0.0f || dy != 0.0f) {
			s_pfx += dx;
			s_pfy += dy;
			s_pointerIdle = POINTER_IDLE;
		} else if (s_pointerIdle > 0) {
			s_pointerIdle--;
		}
		if (dirs)
			s_pointerIdle = 0;
	}
	if (s_pfx < 0) s_pfx = 0;
	if (s_pfx > GUI_W - 1) s_pfx = GUI_W - 1;
	if (s_pfy < 0) s_pfy = 0;
	if (s_pfy > GUI_H - 1) s_pfy = GUI_H - 1;
	s_in.px = (int)s_pfx;
	s_in.py = (int)s_pfy;
	const bool wasOn = s_in.pointerOn;
	s_in.pointerOn = s_pointerIdle > 0;
	// IR jitters by a pixel or so; only a real move counts (it re-focuses).
	static int hx, hy;
	s_in.pointerMoved = s_in.pointerOn && (!wasOn || abs(s_in.px - hx) + abs(s_in.py - hy) >= 2);
	if (s_in.pointerMoved) {
		hx = s_in.px;
		hy = s_in.py;
	}
}

void guiWarpPointer(int x, int y)
{
	s_pfx = (float)x;
	s_pfy = (float)y;
	s_pointerIdle = POINTER_IDLE;
	s_in.px = x;
	s_in.py = y;
	s_in.pointerOn = s_in.pointerMoved = true;
}

//---------------------------------------------------------------------------
// Focus
//---------------------------------------------------------------------------
GuiFocus::GuiFocus() : focusId(-1), hlValid(false) {}

void GuiFocus::clear()
{
	items.clear();
	groups.clear();
}

int GuiFocus::addGroup(const GuiRect &view, int scrollStep)
{
	Group g;
	g.view = view;
	g.step = scrollStep;
	g.scroll = 0;
	g.scrollCur = 0.0f;
	g.contentH = 0;
	groups.push_back(g);
	return (int)groups.size() - 1;
}

void GuiFocus::add(int group, int id, const GuiRect &r, bool enabled)
{
	Item it = { id, group, r, enabled };
	items.push_back(it);
	Group &g = groups[group];
	if (r.y + r.h > g.contentH)
		g.contentH = r.y + r.h;
	if (it.id == focusId) {
		g.scroll = clampScroll(g, g.scroll);
		ensureVisible(it);
		g.scrollCur = (float)g.scroll;
	}
}

int GuiFocus::find(int id) const
{
	for (size_t i = 0; i < items.size(); i++)
		if (items[i].id == id)
			return (int)i;
	return -1;
}

int GuiFocus::focused() const { return find(focusId) >= 0 ? focusId : -1; }

int GuiFocus::clampScroll(const Group &g, int s) const
{
	if (!g.step)
		return 0;
	int max = g.contentH - g.view.h;
	if (max < 0)
		max = 0;
	max = (max + g.step - 1) / g.step * g.step;
	return s < 0 ? 0 : s > max ? max : s;
}

void GuiFocus::ensureVisible(const Item &it)
{
	Group &g = groups[it.group];
	if (!g.step)
		return;
	int s = g.scroll;
	if (it.r.y < s)
		s = it.r.y / g.step * g.step;
	else if (it.r.y + it.r.h > s + g.view.h)
		s = (it.r.y + it.r.h - g.view.h + g.step - 1) / g.step * g.step;
	g.scroll = clampScroll(g, s);
}

GuiRect GuiFocus::screenOf(const Item &it, bool target) const
{
	const Group &g = groups[it.group];
	const int s = target ? g.scroll : (int)(g.scrollCur + 0.5f);
	return GuiRect(g.view.x + it.r.x, g.view.y + it.r.y - s, it.r.w, it.r.h);
}

GuiRect GuiFocus::screenRect(int id) const
{
	const int i = find(id);
	return i < 0 ? GuiRect() : screenOf(items[i], false);
}

bool GuiFocus::visible(int id) const
{
	const int i = find(id);
	return i >= 0 && !screenOf(items[i], false).intersect(groups[items[i].group].view).empty();
}

void GuiFocus::setFocus(int id, bool snap)
{
	const int i = find(id);
	if (i < 0)
		return;
	focusId = id;
	ensureVisible(items[i]);
	if (snap) {
		Group &g = groups[items[i].group];
		g.scrollCur = (float)g.scroll;
		hlValid = false;
	}
}

void GuiFocus::scrollBy(int group, int steps)
{
	Group &g = groups[group];
	if (!g.step)
		return;
	g.scroll = clampScroll(g, g.scroll + steps * g.step);
	// Keep the focus inside the view: nearest item of the group that is.
	const int cur = find(focusId);
	if (cur < 0 || items[cur].group != group)
		return;
	const Item &c = items[cur];
	if (c.r.y >= g.scroll && c.r.y + c.r.h <= g.scroll + g.view.h)
		return;
	int best = -1, bestD = 0;
	for (size_t i = 0; i < items.size(); i++) {
		const Item &it = items[i];
		if (it.group != group || !it.enabled || it.r.y < g.scroll || it.r.y + it.r.h > g.scroll + g.view.h)
			continue;
		const int d = abs(it.r.x - c.r.x) + abs(it.r.y - c.r.y);
		if (best < 0 || d < bestD) {
			best = (int)i;
			bestD = d;
		}
	}
	if (best >= 0)
		focusId = items[best].id;
}

// How well b lies in direction (dx,dy) of a; -1 = not that way. Smaller is
// better: distance along the direction, plus (for items not lined up with a
// in the other axis) twice the sideways distance.
static int dirScore(const GuiRect &a, const GuiRect &b, int dx, int dy)
{
	const int acx = 2 * a.x + a.w, acy = 2 * a.y + a.h, bcx = 2 * b.x + b.w, bcy = 2 * b.y + b.h;
	int primary, side;
	bool lined;
	if (dx) {
		primary = (bcx - acx) * dx;
		side = abs(bcy - acy);
		lined = b.y < a.y + a.h && a.y < b.y + b.h;
	} else {
		primary = (bcy - acy) * dy;
		side = abs(bcx - acx);
		lined = b.x < a.x + a.w && a.x < b.x + b.w;
	}
	if (primary <= 0)
		return -1;
	return primary + (lined ? 0 : 2 * side);
}

bool GuiFocus::move(int dx, int dy)
{
	const int cur = find(focusId);
	if (cur < 0) {
		for (size_t i = 0; i < items.size(); i++)
			if (items[i].enabled) {
				setFocus(items[i].id);
				return true;
			}
		return false;
	}
	const Item &c = items[cur];
	int best = -1, bestScore = 0;
	// Inside the group, in content coordinates (reaches scrolled-out rows).
	for (size_t i = 0; i < items.size(); i++) {
		const Item &it = items[i];
		if ((int)i == cur || !it.enabled || it.group != c.group)
			continue;
		const int s = dirScore(c.r, it.r, dx, dy);
		if (s >= 0 && (best < 0 || s < bestScore)) {
			best = (int)i;
			bestScore = s;
		}
	}
	// Else the nearest visible item of another group.
	if (best < 0) {
		const GuiRect cs = screenOf(c, false);
		for (size_t i = 0; i < items.size(); i++) {
			const Item &it = items[i];
			if (!it.enabled || it.group == c.group || !visible(it.id))
				continue;
			const int s = dirScore(cs, screenOf(it, false), dx, dy);
			if (s >= 0 && (best < 0 || s < bestScore)) {
				best = (int)i;
				bestScore = s;
			}
		}
	}
	if (best < 0)
		return false;
	setFocus(items[best].id);
	return true;
}

int GuiFocus::hitTest(int x, int y) const
{
	for (size_t i = 0; i < items.size(); i++) {
		const Item &it = items[i];
		if (it.enabled && groups[it.group].view.contains(x, y) && screenOf(it, false).contains(x, y))
			return it.id;
	}
	return -1;
}

GuiRect GuiFocus::highlight() const
{
	if (!hlValid) {
		const int i = find(focusId);
		return i < 0 ? GuiRect() : screenOf(items[i], false);
	}
	return GuiRect((int)(hl[0] + 0.5f), (int)(hl[1] + 0.5f), (int)(hl[2] + 0.5f), (int)(hl[3] + 0.5f));
}

void GuiFocus::tick()
{
	if (find(focusId) < 0)
		for (size_t i = 0; i < items.size(); i++)
			if (items[i].enabled) {
				setFocus(items[i].id, true);
				break;
			}
	for (size_t i = 0; i < groups.size(); i++) {
		Group &g = groups[i];
		const float d = g.scroll - g.scrollCur;
		g.scrollCur = fabsf(d) < 0.5f ? (float)g.scroll : g.scrollCur + d * 0.3f;
	}
	const int i = find(focusId);
	if (i < 0) {
		hlValid = false;
		return;
	}
	const GuiRect t = screenOf(items[i], false);
	const float tv[4] = { (float)t.x, (float)t.y, (float)t.w, (float)t.h };
	for (int k = 0; k < 4; k++)
		hl[k] = hlValid && fabsf(tv[k] - hl[k]) >= 0.5f ? hl[k] + (tv[k] - hl[k]) * 0.4f : tv[k];
	hlValid = true;
}

//---------------------------------------------------------------------------
// Screen stack
//---------------------------------------------------------------------------
#define FADE_FRAMES 10

static std::vector<GuiScreen *> s_stack, s_pushed;
static int s_pops;
static bool s_quit;
static int s_quitCode;

void guiPush(GuiScreen *s) { s_pushed.push_back(s); }
void guiPop() { s_pops++; }
void guiQuit(int code) { s_quit = true; s_quitCode = code; }

#define LAUNCH_FADE_FRAMES 16

// One frame of the stack, from the topmost opaque screen up; fadeA > 0
// draws fadeCol over it.
static void drawFrame(bool pointer, u8 fadeA, GuiColor fadeCol)
{
	size_t first = s_stack.size() - 1;
	while (first > 0 && !s_stack[first]->opaque())
		first--;
	guiBeginFrame(guiRgba(0, 0, 0));
	for (size_t i = first; i < s_stack.size(); i++)
		s_stack[i]->draw();
	if (pointer)
		guiPointer(s_in.px, s_in.py);
	guiFade(fadeA, fadeCol);
	guiEndFrame();
}

static void focusMoved(GuiScreen *s, int before)
{
	const int now = s->focus.focused();
	if (now != before)
		s->focusChanged(now);
}

int guiRun(GuiScreen *root)
{
	log_console_enable_video(false);   // its retrace callback would draw over the XFB
	s_stack.clear();
	s_pushed.clear();
	s_pops = 0;
	s_quit = false;
	s_quitCode = 0;
	s_stack.push_back(root);
	root->enter();
	root->focus.tick();
	int fade = FADE_FRAMES;
	int result = 0;

	for (;;) {
		guiPollInput();
		const GuiInput &in = s_in;
		if (sys_exit_requested() || (in.down & GUI_BTN_HOME)) {
			result = GUI_EXIT;
			break;
		}

		GuiScreen *top = s_stack.back();
		top->update(in);
		const int before = top->focus.focused();
		if (in.pointerOn && in.pointerMoved) {
			const int id = top->focus.hitTest(in.px, in.py);
			if (id >= 0)
				top->focus.setFocus(id);
		}
		if (in.down & GUI_BTN_UP) top->focus.move(0, -1);
		if (in.down & GUI_BTN_DOWN) top->focus.move(0, 1);
		// Left/Right go to the focused item first (choice controls).
		const int f = top->focus.focused();
		if ((in.down & GUI_BTN_LEFT) && !(f >= 0 && top->step(f, -1))) top->focus.move(-1, 0);
		if ((in.down & GUI_BTN_RIGHT) && !(f >= 0 && top->step(f, 1))) top->focus.move(1, 0);
		focusMoved(top, before);
		if (in.down & GUI_BTN_A) {
			// With the pointer shown, A acts on what it points at (nothing if
			// it points at nothing); otherwise on the focused item.
			const int id = in.pointerOn ? top->focus.hitTest(in.px, in.py) : top->focus.focused();
			if (id >= 0)
				top->activate(id);
		} else if (in.down & GUI_BTN_B) {
			if (top->back())
				guiPop();
		}
		top->focus.tick();

		// From black at start, half-way on a screen change.
		drawFrame(in.pointerOn, (u8)(255 * fade / FADE_FRAMES), guiRgba(0, 0, 0));
		if (fade > 0)
			fade--;

		const bool changed = s_pops || !s_pushed.empty();
		const bool wasOpaque = s_stack.back()->opaque();
		for (; s_pops > 0 && !s_stack.empty(); s_pops--) {
			delete s_stack.back();
			s_stack.pop_back();
			if (!s_stack.empty())
				s_stack.back()->resume();
		}
		s_pops = 0;
		for (size_t i = 0; i < s_pushed.size(); i++) {
			s_stack.push_back(s_pushed[i]);
			s_pushed[i]->enter();
			s_pushed[i]->focus.tick();
		}
		s_pushed.clear();
		// A short fade between full screens; dialogs (not opaque) just appear.
		if (changed && !s_stack.empty() && wasOpaque && s_stack.back()->opaque())
			fade = FADE_FRAMES / 2;
		if (s_quit) {
			// Fade out to black, the game's background (it stays black while
			// the caller loads, until the game's first frame).
			for (int f = 1; f <= LAUNCH_FADE_FRAMES && !s_stack.empty(); f++)
				drawFrame(false, (u8)(255 * f / LAUNCH_FADE_FRAMES), guiRgba(0, 0, 0));
			result = s_quitCode;
			break;
		}
		if (s_stack.empty())
			break;
	}

	for (size_t i = 0; i < s_stack.size(); i++)
		delete s_stack[i];
	for (size_t i = 0; i < s_pushed.size(); i++)
		delete s_pushed[i];
	s_stack.clear();
	s_pushed.clear();
	guiRestoreGx();
	return result;
}
