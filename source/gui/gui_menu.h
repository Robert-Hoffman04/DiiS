/****************************************************************************
 * DiiS - gui_menu.h
 *
 * Shared between the menu's screens: gui_library.cpp (Library, Game) and
 * gui_settings.cpp (Settings, Game settings, folder picker, About).
 ***************************************************************************/
#ifndef DIIS_GUI_MENU_H
#define DIIS_GUI_MENU_H

#include "gui.h"
#include "../library/library.h"

static inline void menuBackground()
{
	guiRectGradient(GuiRect(0, 0, GUI_W, GUI_H), guiRgba(26, 32, 48), guiRgba(10, 12, 20));
}

static inline void menuHeader(const char *title)
{
	guiText(GUI_FONT_TITLE, 40, 20, "DiiS", GUI_COL_TEXT);
	if (title)
		guiText(GUI_FONT_BODY, 112, 28, title, GUI_COL_DIM);
}

// gui_settings.cpp. The game's LibGame must outlive the screen (the Game
// screen below it holds the library entry).
GuiScreen *newSettingsScreen();
GuiScreen *newGameSettingsScreen(const LibGame *game, const GuiImage &icon);

#endif // DIIS_GUI_MENU_H
