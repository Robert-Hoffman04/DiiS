/****************************************************************************
 * DiiS - gui_widgets.cpp   (see gui.h)
 *
 * Standard controls the menu screens are built from: the DS menu backdrop
 * (lined grid, top bar, dialog box), buttons, the choice (cycle) row and the
 * on/off switch.
 ***************************************************************************/
#include "gui.h"

#define ARROW_W 14   // hit/draw width of each "<" / ">" end of a choice

//---------------------------------------------------------------------------
// DS menu backdrop. Measured from the DS menu's bottom-screen background:
// rows alternate 247 / 222 grey, a 1 px 165-grey line every 48 px each way,
// a 3x3 dot on each crossing and a 1 px light gap either side of the
// vertical line for the five rows under each crossing. Drawn in code (at
// GUI_DS_SCALE) so it fills the canvas at any size.
//---------------------------------------------------------------------------
#define BG_CELL 48
#define BG_X0 16   // canvas x of the first vertical line (every 48: 16 px margins both sides)
#define BG_Y0 23   // canvas y of the first horizontal line (as the DS)

static const GuiColor kPaper = guiRgba(247, 247, 247), kPaperLine = guiRgba(222, 222, 222);
static const GuiColor kGrid = guiRgba(165, 165, 165);

void guiDsBackground()
{
	const int s = GUI_DS_SCALE, cell = BG_CELL * s;
	guiRect(GuiRect(0, 0, GUI_W, GUI_H), kPaper);
	for (int y = s; y < GUI_H; y += 2 * s)
		guiRect(GuiRect(0, y, GUI_W, s), kPaperLine);
	for (int gy = BG_Y0 % cell - cell; gy < GUI_H; gy += cell)
		for (int gx = BG_X0 % cell; gx < GUI_W; gx += cell) {
			guiRect(GuiRect(gx - s, gy + 2 * s, s, 5 * s), kPaper);
			guiRect(GuiRect(gx + s, gy + 2 * s, s, 5 * s), kPaper);
		}
	for (int gx = BG_X0 % cell; gx < GUI_W; gx += cell)
		guiRect(GuiRect(gx, 0, s, GUI_H), kGrid);
	for (int gy = BG_Y0 % cell; gy < GUI_H; gy += cell) {
		guiRect(GuiRect(0, gy, GUI_W, s), kGrid);
		for (int gx = BG_X0 % cell; gx < GUI_W; gx += cell)
			guiRect(GuiRect(gx - s, gy - s, 3 * s, 3 * s), kGrid);
	}
}

// The DS menu's top bar (topbar.png), row by row: 16 rows of five colours,
// the steps between bands dithered (even / odd pixels of a row alternate,
// and the pattern flips each row), the last row black.
static const u8 kBarCol[5][3] = {
	{ 138, 162, 178 }, { 113, 146, 162 }, { 105, 138, 162 }, { 97, 130, 154 }, { 0, 0, 0 },
};
static const u8 kBarRows[16][2] = {   // colour of even, odd pixels
	{ 0, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 1, 2 }, { 2, 1 }, { 2, 2 }, { 2, 2 },
	{ 3, 2 }, { 2, 3 }, { 3, 3 }, { 3, 3 }, { 3, 3 }, { 3, 3 }, { 3, 3 }, { 4, 4 },
};

static GuiColor barCol(int i) { return guiRgba(kBarCol[i][0], kBarCol[i][1], kBarCol[i][2]); }

void guiTopBar()
{
	// The bar's first colour fills the safe margin above it.
	guiRect(GuiRect(0, 0, GUI_W, GUI_SAFE_Y), barCol(0));
	for (int i = 0; i < 16; i++) {
		const int y = GUI_SAFE_Y + i;
		guiRect(GuiRect(0, y, GUI_W, 1), barCol(kBarRows[i][0]));
		if (kBarRows[i][1] != kBarRows[i][0])
			for (int x = 1; x < GUI_W; x += 2)
				guiRect(GuiRect(x, y, 1, 1), barCol(kBarRows[i][1]));
	}
}

GuiColor guiDsEdgeColor(int x, int y)
{
	// Outside the canvas the backdrop is only the top bar and, below it,
	// the paper's stripes and horizontal grid lines: the nearest vertical
	// lines (and their dots and gaps) are further out than the edges reach.
	if (y < GUI_SAFE_Y)
		return barCol(0);
	if (y < GUI_TOPBAR_H)
		return barCol(kBarRows[y - GUI_SAFE_Y][x & 1]);
	const int s = GUI_DS_SCALE, cell = BG_CELL * s;
	if (((y - BG_Y0) % cell + cell) % cell < s)
		return kGrid;
	return y % (2 * s) >= s ? kPaperLine : kPaper;
}

// Nine-slice of GUI_GFX_DIALOGBOX (DS pixels): an 8 px corner ring, edges
// and a 32x32 striped fill. Edges / fill repeat 1:1, the last tile cropped.
struct Slice { int x, y, w, h; };
static void sliceTile(const GuiImage &img, const Slice &sl, int x, int y, int w, int h)
{
	const int s = GUI_DS_SCALE;
	for (int ty = 0; ty < h; ty += sl.h)
		for (int tx = 0; tx < w; tx += sl.w)
			guiImagePart(img, sl.x, sl.y, w - tx < sl.w ? w - tx : sl.w, h - ty < sl.h ? h - ty : sl.h,
			             x + tx * s, y + ty * s, s);
}

void guiDialogBox(const GuiRect &r)
{
	static const int C = 8;
	static const Slice TL = { 48, 16, C, C }, TR = { 56, 16, C, C }, BL = { 48, 24, C, C }, BR = { 56, 24, C, C };
	static const Slice TOP = { 32, 0, 32, C }, BOT = { 32, 8, 32, C };
	static const Slice LEFT = { 32, 16, C, 32 }, RIGHT = { 40, 16, C, 32 }, MID = { 0, 0, 32, 32 };
	const GuiImage &img = guiGfx(GUI_GFX_DIALOGBOX);
	const int s = GUI_DS_SCALE;
	// In DS pixels; the height stays even so the stripes stay in phase.
	int w = r.w / s, h = (r.h / s) & ~1;
	if (w < 2 * C) w = 2 * C;
	if (h < 2 * C) h = 2 * C;
	const int x1 = r.x + (w - C) * s, y1 = r.y + (h - C) * s;
	sliceTile(img, MID, r.x + C * s, r.y + C * s, w - 2 * C, h - 2 * C);
	sliceTile(img, TOP, r.x + C * s, r.y, w - 2 * C, C);
	sliceTile(img, BOT, r.x + C * s, y1, w - 2 * C, C);
	sliceTile(img, LEFT, r.x, r.y + C * s, C, h - 2 * C);
	sliceTile(img, RIGHT, x1, r.y + C * s, C, h - 2 * C);
	sliceTile(img, TL, r.x, r.y, C, C);
	sliceTile(img, TR, x1, r.y, C, C);
	sliceTile(img, BL, r.x, y1, C, C);
	sliceTile(img, BR, x1, y1, C, C);
}

// The DS menu's boxes are square with a 1 px dark outline; drawn at 2x.
static void dsBox(const GuiRect &r, GuiColor fill)
{
	guiRect(r, guiRgba(0, 0, 0));
	guiRect(r.inset(GUI_DS_SCALE), fill);
}

void guiButton(const GuiRect &r, const char *label, bool primary)
{
	dsBox(r, primary ? GUI_COL_ACCENT : GUI_COL_PANEL);
	guiTextBox(GUI_FONT_BODY, r, label, primary ? GUI_COL_TEXT_LIGHT : GUI_COL_TEXT, GUI_ALIGN_CENTER);
}

bool GuiChoice::step(int dir)
{
	const int n = (int)values.size();
	if (n < 2)
		return false;
	sel = ((sel + dir) % n + n) % n;
	return true;
}

static GuiRect valueArea(const GuiRect &r, int labelW)
{
	const int x = labelW < 0 ? r.x + 4 : labelW > 0 ? r.x + labelW : r.x + r.w / 2;
	return GuiRect(x, r.y, r.x + r.w - 4 - x, r.h);
}

void guiChoiceDraw(const GuiRect &r, const char *label, const GuiChoice &c, bool focused, bool marked, int labelW)
{
	dsBox(r, focused ? GUI_COL_ROW_FOCUS : GUI_COL_PANEL);
	const GuiRect v = valueArea(r, labelW);
	if (labelW >= 0)
		guiTextBox(GUI_FONT_BODY, GuiRect(r.x + 7, r.y, v.x - r.x - 10, r.h), label, GUI_COL_TEXT);
	const bool cycles = c.values.size() > 1;
	const GuiColor arrow = cycles ? (focused ? GUI_COL_TEXT : GUI_COL_DIM) : guiRgba(200, 200, 200);
	const int cy = v.y + v.h / 2;
	guiTriangle(v.x + 8, cy - 4, v.x + 8, cy + 4, v.x + 4, cy, arrow);
	guiTriangle(v.x + v.w - 8, cy - 4, v.x + v.w - 4, cy, v.x + v.w - 8, cy + 4, arrow);
	guiTextBox(GUI_FONT_BODY, GuiRect(v.x + ARROW_W, v.y, v.w - 2 * ARROW_W, v.h), c.value(),
	           marked ? guiRgba(200, 80, 0) : GUI_COL_TEXT, GUI_ALIGN_CENTER);
}

int guiChoiceClick(const GuiRect &r, int labelW)
{
	const GuiInput &in = guiInput();
	if (!in.pointerOn)
		return 1;
	const GuiRect v = valueArea(r, labelW);
	return in.px < v.x + ARROW_W + 4 && in.px >= v.x - 4 ? -1 : 1;
}

void guiToggleDraw(const GuiRect &r, const char *label, bool on, bool focused)
{
	dsBox(r, focused ? GUI_COL_ROW_FOCUS : GUI_COL_PANEL);
	const GuiRect sw(r.x + r.w - 27, r.y + (r.h - 11) / 2, 20, 11);
	guiTextBox(GUI_FONT_BODY, GuiRect(r.x + 7, r.y, sw.x - r.x - 11, r.h), label, GUI_COL_TEXT);
	guiRoundRect(sw, 5, on ? GUI_COL_ACCENT : guiRgba(190, 190, 190));
	guiRoundRect(GuiRect(on ? sw.x + sw.w - 10 : sw.x + 1, sw.y + 1, 9, 9), 4, GUI_COL_PANEL);
}
