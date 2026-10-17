/****************************************************************************
 * DiiS - gui_widgets.cpp   (see gui.h)
 *
 * Standard controls the menu screens are built from: buttons, the choice
 * (cycle) row and the on/off switch.
 ***************************************************************************/
#include "gui.h"

#define ARROW_W 28   // hit/draw width of each "<" / ">" end of a choice

void guiButton(const GuiRect &r, const char *label, bool primary)
{
	guiRoundRect(r, 10, primary ? GUI_COL_ACCENT : GUI_COL_PANEL);
	guiTextBox(GUI_FONT_BODY, r, label, GUI_COL_TEXT, GUI_ALIGN_CENTER);
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
	const int x = labelW < 0 ? r.x + 8 : labelW > 0 ? r.x + labelW : r.x + r.w / 2;
	return GuiRect(x, r.y, r.x + r.w - 8 - x, r.h);
}

void guiChoiceDraw(const GuiRect &r, const char *label, const GuiChoice &c, bool focused, bool marked, int labelW)
{
	guiRoundRect(r, 10, focused ? guiRgba(44, 56, 82) : GUI_COL_PANEL);
	const GuiRect v = valueArea(r, labelW);
	if (labelW >= 0)
		guiTextBox(GUI_FONT_BODY, GuiRect(r.x + 14, r.y, v.x - r.x - 20, r.h), label, GUI_COL_TEXT);
	const bool cycles = c.values.size() > 1;
	const GuiColor arrow = cycles ? (focused ? GUI_COL_TEXT : GUI_COL_DIM) : guiRgba(70, 80, 100);
	const int cy = v.y + v.h / 2;
	guiTriangle(v.x + 16, cy - 7, v.x + 16, cy + 7, v.x + 8, cy, arrow);
	guiTriangle(v.x + v.w - 16, cy - 7, v.x + v.w - 8, cy, v.x + v.w - 16, cy + 7, arrow);
	guiTextBox(GUI_FONT_BODY, GuiRect(v.x + ARROW_W, v.y, v.w - 2 * ARROW_W, v.h), c.value(),
	           marked ? guiRgba(120, 180, 255) : GUI_COL_TEXT, GUI_ALIGN_CENTER);
}

int guiChoiceClick(const GuiRect &r, int labelW)
{
	const GuiInput &in = guiInput();
	if (!in.pointerOn)
		return 1;
	const GuiRect v = valueArea(r, labelW);
	return in.px < v.x + ARROW_W + 8 && in.px >= v.x - 8 ? -1 : 1;
}

void guiToggleDraw(const GuiRect &r, const char *label, bool on, bool focused)
{
	guiRoundRect(r, 10, focused ? guiRgba(44, 56, 82) : GUI_COL_PANEL);
	const GuiRect sw(r.x + r.w - 54, r.y + (r.h - 22) / 2, 40, 22);
	guiTextBox(GUI_FONT_BODY, GuiRect(r.x + 14, r.y, sw.x - r.x - 22, r.h), label, GUI_COL_TEXT);
	guiRoundRect(sw, 11, on ? GUI_COL_ACCENT : guiRgba(70, 80, 100));
	guiRoundRect(GuiRect(on ? sw.x + sw.w - 20 : sw.x + 2, sw.y + 2, 18, 18), 9, GUI_COL_TEXT);
}
