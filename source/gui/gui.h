/****************************************************************************
 * DiiS - gui.h
 *
 * GX GUI framework (menu rework phase 2): drawing in the 640x480 canvas
 * main.cpp's init() sets up (guOrtho 0..640 x 0..480, one unit = one canvas
 * pixel), bitmap text, controller input, focus navigation and a screen stack.
 * The GUI owns GX and the XFBs while guiRun() runs (draw_thread isn't
 * running yet), and hands GX back in the present state draw_thread expects
 * when it returns.
 *
 * A screen (phase 3/4 build on this):
 *
 *   class MyScreen : public GuiScreen {
 *       void enter() {                       // pushed (and on guiRefresh())
 *           focus.clear();
 *           int g = focus.addGroup(GuiRect(40, 80, 560, 300), 76);   // scrolls by row
 *           for (i...) focus.add(g, i, GuiRect(col * 80, row * 76, 76, 76));
 *       }
 *       void draw() {
 *           guiRect(...); guiText(...);
 *           for each item: GuiRect r = focus.screenRect(i); ...   // scrolled
 *           guiFocusFrame(focus.highlight());                    // animated highlight
 *       }
 *       void activate(int id) { guiPush(new GameScreen(id)); }   // A / pointer click
 *       bool back() { return true; }                             // B: pop
 *   };
 *   if (guiRun(new MyScreen) == GUI_EXIT) sys_exit();            // Home/Reset/Power
 *
 * Navigation is geometric: D-pad/stick moves to the nearest item in that
 * direction inside the focused group (in content coordinates, so it reaches
 * rows scrolled out of view), else to the nearest visible item of another
 * group. The pointer (Wiimote IR, or GC C-stick / Classic right stick)
 * focuses the item under it; A then activates that item, B goes back.
 ***************************************************************************/
#ifndef DIIS_GUI_H
#define DIIS_GUI_H

#include <gccore.h>
#include <string>
#include <vector>

#define GUI_W 640
#define GUI_H 480

struct GuiRect {
	int x, y, w, h;
	GuiRect() : x(0), y(0), w(0), h(0) {}
	GuiRect(int x_, int y_, int w_, int h_) : x(x_), y(y_), w(w_), h(h_) {}
	bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
	bool empty() const { return w <= 0 || h <= 0; }
	GuiRect inset(int d) const { return GuiRect(x + d, y + d, w - 2 * d, h - 2 * d); }
	GuiRect intersect(const GuiRect &o) const;
};

struct GuiColor {
	u8 r, g, b, a;
};
static inline GuiColor guiRgba(u8 r, u8 g, u8 b, u8 a = 255) { GuiColor c = { r, g, b, a }; return c; }
static inline GuiColor guiWithAlpha(GuiColor c, u8 a) { c.a = (u8)(c.a * a / 255); return c; }

//---------------------------------------------------------------------------
// Drawing (gui_draw.cpp). Integer coordinates are canvas pixels; textures
// are drawn one texel per pixel (or an integer multiple) with a nearest
// filter, so text and icons stay crisp.
//---------------------------------------------------------------------------
void guiBeginFrame(GuiColor clear);       // takes GX over (GUI vertex format, TEV, no Z)
void guiEndFrame();                       // copy to the XFB, flip, wait for vsync
void guiRestoreGx();                      // hand GX back to the emulator's present state

void guiSetAlpha(u8 a);                   // global alpha multiplier for everything drawn (fades)
u8 guiAlpha();

void guiRect(const GuiRect &r, GuiColor c);
void guiRectGradient(const GuiRect &r, GuiColor top, GuiColor bottom);
void guiRoundRect(const GuiRect &r, int radius, GuiColor c);
// Rounded panel: fill, then a borderW-pixel border on top (borderW 0: none).
void guiPanel(const GuiRect &r, int radius, GuiColor fill, GuiColor border, int borderW);
void guiFade(u8 a);                       // black over the whole canvas
void guiTriangle(int x0, int y0, int x1, int y1, int x2, int y2, GuiColor c);

// Clipping (scissor); nests by intersection.
void guiPushClip(const GuiRect &r);
void guiPopClip();

// RGB5A3 image (e.g. a library icon). The source is row-major (LibGame::icon,
// gui_icons.h); w and h must be multiples of 4.
struct GuiImage {
	GXTexObj tex;
	u16 *data;
	int w, h;
	GuiImage() : data(0), w(0), h(0) {}
};
bool guiImageCreate(GuiImage &img, const u16 *rgb5a3, int w, int h);
void guiImageFree(GuiImage &img);
void guiImage(const GuiImage &img, int x, int y, int scale = 1, GuiColor tint = guiRgba(255, 255, 255));

// Built-in images.
const GuiImage &guiGenericIcon(bool gba);  // 32x32 DS / GBA icon for games without one
void guiPointer(int x, int y);            // Wii pointer + shadow, hotspot at (x, y)
void guiFocusFrame(const GuiRect &r);     // standard focus highlight around r

//---------------------------------------------------------------------------
// Text (gui_text.cpp): UTF-8, Noto Sans bitmap fonts (tools/gui/mkfont.py).
// Characters the font doesn't have draw as a box; full-width ASCII (U+FF01..)
// folds to ASCII. y is the top of the line box (baseline = y + ascent).
//---------------------------------------------------------------------------
enum GuiFont { GUI_FONT_BODY, GUI_FONT_TITLE };   // 20 px, 28 px semibold
enum GuiAlign { GUI_ALIGN_LEFT, GUI_ALIGN_CENTER, GUI_ALIGN_RIGHT };

int guiFontHeight(GuiFont f);             // line box: ascent + descent
int guiFontAscent(GuiFont f);
int guiFontCap(GuiFont f);                // cap height (for centring)
int guiTextWidth(GuiFont f, const char *utf8);
// One line. x is the left / centre / right edge per align. maxW > 0: cut to
// fit with an ellipsis. Returns the width drawn.
int guiText(GuiFont f, int x, int y, const char *utf8, GuiColor c, GuiAlign align = GUI_ALIGN_LEFT, int maxW = 0);
// One line inside r: aligned horizontally, cap height centred vertically,
// ellipsis if too wide.
void guiTextBox(GuiFont f, const GuiRect &r, const char *utf8, GuiColor c, GuiAlign align = GUI_ALIGN_LEFT);
// Word-wrapped (also at '\n') from the top of r; lines past the bottom are
// dropped. Returns the height used.
int guiTextWrap(GuiFont f, const GuiRect &r, const char *utf8, GuiColor c, GuiAlign align = GUI_ALIGN_LEFT);
// Next code point of a UTF-8 string (advances p; invalid bytes -> U+FFFD, end -> 0).
u32 guiUtf8Next(const char *&p);

//---------------------------------------------------------------------------
// Input (gui_screen.cpp): Wiimote (+Nunchuk), Classic Controller, GameCube
// pad and USB Gecko taps, merged. Directions repeat while held.
//---------------------------------------------------------------------------
enum GuiButton {
	GUI_BTN_A = 1 << 0, GUI_BTN_B = 1 << 1, GUI_BTN_X = 1 << 2, GUI_BTN_Y = 1 << 3,
	GUI_BTN_L = 1 << 4, GUI_BTN_R = 1 << 5, GUI_BTN_PLUS = 1 << 6, GUI_BTN_MINUS = 1 << 7,
	GUI_BTN_HOME = 1 << 8,
	GUI_BTN_UP = 1 << 9, GUI_BTN_DOWN = 1 << 10, GUI_BTN_LEFT = 1 << 11, GUI_BTN_RIGHT = 1 << 12,
};
struct GuiInput {
	u32 down;            // pressed this frame (directions include auto-repeat)
	u32 held;
	bool pointerOn;      // pointer shown: IR on screen, or C-stick moved it recently
	bool pointerMoved;   // moved this frame
	int px, py;          // pointer hotspot, canvas pixels
};
const GuiInput &guiInput();
void guiWarpPointer(int x, int y);  // show the pointer at (x, y) as if a stick moved it there

//---------------------------------------------------------------------------
// Focus model: items (id + rect) in groups. A group has a screen-space view;
// a scrolling group's items are laid out in content coordinates (0,0 = the
// view's top-left at scroll 0) and the view follows the focus, in steps of
// scrollStep pixels (a grid row).
//---------------------------------------------------------------------------
class GuiFocus {
public:
	GuiFocus();
	void clear();                                  // items and groups; keeps the focused id
	int addGroup(const GuiRect &view, int scrollStep = 0);   // scrollStep 0: doesn't scroll
	void add(int group, int id, const GuiRect &r, bool enabled = true);

	int focused() const;                           // id, -1 = none
	bool isFocused(int id) const { return focused() == id; }
	void setFocus(int id, bool snap = false);      // snap: no highlight/scroll animation
	bool move(int dx, int dy);                     // -1/0/+1; false if nothing that way
	int hitTest(int x, int y) const;               // id under a canvas point, -1 = none

	bool has(int id) const { return find(id) >= 0; }
	GuiRect screenRect(int id) const;              // after scrolling
	bool visible(int id) const;                    // any part inside its group's view
	const GuiRect &view(int group) const { return groups[group].view; }
	int scroll(int group) const { return (int)groups[group].scrollCur; }
	void scrollBy(int group, int steps);           // e.g. Plus/Minus page; keeps the focus in view
	GuiRect highlight() const;                     // animated rect of the focused item
	void tick();                                   // per frame: animations

private:
	struct Item { int id, group; GuiRect r; bool enabled; };
	struct Group { GuiRect view; int step; int scroll; float scrollCur; int contentH; };
	std::vector<Item> items;
	std::vector<Group> groups;
	int focusId;
	float hl[4];
	bool hlValid;

	int find(int id) const;
	GuiRect screenOf(const Item &it, bool target) const;
	void ensureVisible(const Item &it);
	int clampScroll(const Group &g, int s) const;
};

//---------------------------------------------------------------------------
// Screens and the run loop.
//---------------------------------------------------------------------------
class GuiScreen {
public:
	GuiScreen() {}
	virtual ~GuiScreen() {}
	virtual void enter() {}                        // pushed: build the focus items
	virtual void resume() {}                       // the screen above it was popped
	virtual void update(const GuiInput &in) {}     // every frame, before navigation
	virtual void draw() = 0;
	virtual void activate(int id) {}               // A on the focused item / pointer click
	virtual void focusChanged(int id) {}
	virtual bool back() { return true; }           // B: true = pop this screen
	virtual bool step(int id, int dir) { return false; }   // Left/Right (-1/+1) on the focused item: true = used, no focus move
	virtual bool opaque() const { return true; }   // false: the screen below is drawn first (dialogs)
	GuiFocus focus;
};

//---------------------------------------------------------------------------
// Widgets (gui_widgets.cpp): the menu's palette and standard controls.
//---------------------------------------------------------------------------
static const GuiColor GUI_COL_TEXT = guiRgba(236, 240, 248), GUI_COL_DIM = guiRgba(150, 160, 180);
static const GuiColor GUI_COL_PANEL = guiRgba(30, 36, 52), GUI_COL_PANEL_EDGE = guiRgba(58, 68, 92);
static const GuiColor GUI_COL_ACCENT = guiRgba(56, 120, 200);

void guiButton(const GuiRect &r, const char *label, bool primary);

// Choice (cycle) control: the label on the left, "<  value  >" on the right.
// A screen forwards Left/Right on the focused row from GuiScreen::step() and
// A / a click from activate() (guiChoiceClick: a click on the left arrow
// steps back). An on/off toggle is a choice with two values.
struct GuiChoice {
	std::vector<std::string> values;
	int sel;
	GuiChoice() : sel(0) {}
	void add(const std::string &v) { values.push_back(v); }
	bool step(int dir);                         // wraps; false with fewer than two values
	const char *value() const { return sel >= 0 && sel < (int)values.size() ? values[sel].c_str() : ""; }
};
// labelW: width of the label column (0: the value area takes the right half;
// -1: no label, the value fills the row).
// marked: draw the value in the accent colour (e.g. a per-game override).
void guiChoiceDraw(const GuiRect &r, const char *label, const GuiChoice &c, bool focused, bool marked = false, int labelW = 0);
int guiChoiceClick(const GuiRect &r, int labelW = 0);   // A / click on r: -1 on the left arrow, else +1

// On/off switch row: the label on the left, a switch on the right. A / a
// click flips it (the screen's activate()); Left/Right stay navigation.
void guiToggleDraw(const GuiRect &r, const char *label, bool on, bool focused);

#define GUI_EXIT (-1)   // Home, console Reset/Power or Wiimote power: caller calls sys_exit()

// Runs until guiQuit() or the stack empties (returns 0), or GUI_EXIT. Takes
// ownership of root and of every pushed screen (deleted when popped).
int guiRun(GuiScreen *root);
void guiPush(GuiScreen *s);      // takes effect after this frame
void guiPop();
void guiQuit(int code);

// gui_library.cpp: the menu (Library / Game screens). 0: the user picked
// `rom` to boot, then load savestate `stateSlot` (-1: none); GUI_EXIT: leave.
int guiLibraryRun(std::string &rom, int &stateSlot);

#ifdef DIIS_GUI_DEMO
int guiDemoRun();                // gui_demo.cpp: the framework demo screen
#endif

#endif // DIIS_GUI_H
