/****************************************************************************
 * DiiS - gui_menu.h
 *
 * Shared between the menu's screens: gui_library.cpp (Library, game panel) and
 * gui_settings.cpp (Settings, folder picker, About).
 ***************************************************************************/
#ifndef DIIS_GUI_MENU_H
#define DIIS_GUI_MENU_H

#include "gui.h"
#include "../library/library.h"

static inline void menuBackground()
{
	guiDsBackground();
}

// The top bar: "DiiS" and the screen's title, light on the bar.
static inline void menuHeader(const char *title)
{
	const GuiRect bar(8, GUI_SAFE_Y, GUI_W - 16, GUI_TOPBAR_H - GUI_SAFE_Y - 1);
	guiTopBar();
	const int w = guiTextWidth(GUI_FONT_BODY, "DiiS");
	guiTextBox(GUI_FONT_BODY, bar, "DiiS", GUI_COL_TEXT_LIGHT);
	if (title)
		guiTextBox(GUI_FONT_BODY, GuiRect(bar.x + w + 8, bar.y, bar.w - w - 8, bar.h), title, guiRgba(210, 222, 230));
}

// gui_settings.cpp.
GuiScreen *newSettingsScreen();
// The CPU choices (JIT if built in, Interpreter), for a GuiChoice.
void menuAddCpuValues(GuiChoice &c);

#endif // DIIS_GUI_MENU_H
