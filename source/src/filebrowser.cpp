/*
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

#include "filebrowser.h"
#include <sys/dir.h>
#include <sys/param.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <wiiuse/wpad.h>
#include "ctrlssdl.h"

#define TYPE_FILTER(x)  (strstr(x, ".nds") || strstr(x, ".NDS"))

typedef enum {
	BROWSER_FILE_NOT_FOUND  = -1,
	BROWSER_FILE_SELECTED   = 0,
	BROWSER_CANCELED        = 1,
	BROWSER_CHANGE_FOLDER   = 2
} ret_action;

typedef struct {
	char name[255];
	int  size;
	int  attr;
} dir_ent;

static int const per_page = 16;

typedef struct {
	char title[255];
	char path[MAXPATHLEN];
} file_browser_st;

static void clear_console(){
	printf("\x1B[2J");
	VIDEO_WaitVSync();
	printf("\x1B[2;2H");
}

static void browse_back(char *str){
	int length = strlen(str);
	int idx = length;
	do{
		char ch = str[idx];
		str[idx] = '\0';
		if( ch == '/' ) {
			if( str[idx-1] == ':' ) // root folder.
				str[idx] = '/';     // Check is here, because it happens only once per function call.
			break;
		}
		--idx;
	}while(idx > 0);
}


static ret_action textFileBrowser(file_browser_st *file_struct){

	// Set everything up to read

	DIR* dp = opendir(file_struct->path);

	if(!dp)
		return BROWSER_FILE_NOT_FOUND;

	struct stat fstat;
	
	s32 pathLen = strlen(file_struct->path);
	s32 num_entries = 1, i = 0;
	dir_ent* dir = (dir_ent*) malloc( num_entries * sizeof(dir_ent) );
	struct dirent *tdir;
	
	// Read each entry of the directory

	while ((tdir=readdir(dp))!=NULL) {

		// Skip if it's the '.' folder (that does us no good)
		if(strcmp(tdir->d_name, ".") == 0)
			continue;
		
		u32 tdirNameLen = strlen(tdir->d_name);

		if(MAXPATHLEN - pathLen - tdirNameLen <= 0){
			continue; // TOO LONG!
			// Print an error?
		}
		
		char filename[MAXPATHLEN];
		char div = '/';
		memset(filename, 0, MAXPATHLEN);
		
		// We have to pass the entire filepath to the stat function
		strncat(filename, file_struct->path, pathLen);
		// Add in the divider
		strncat(filename, &div, 1);
		// ...And the name
		strncat(filename, tdir->d_name, tdirNameLen);
		
		stat(filename,&fstat);

		// If it is a directory or a .nds file:
		if(S_ISDIR(fstat.st_mode) || TYPE_FILTER(filename)){
			// Make sure we have room for this one
			if(i == num_entries) {
				dir_ent *new_dir;
				++num_entries;
				new_dir = (dir_ent*) realloc(dir, num_entries * sizeof(dir_ent));
				if (new_dir==NULL)
					break; // Out of memory, return the files we've already found
				dir = new_dir;
			}
			
			strcpy(dir[i].name, tdir->d_name);
			dir[i].size = fstat.st_size;
			dir[i].attr = fstat.st_mode;
			++i;
		}
		
		
	}

	closedir(dp);
	
	int index = 0;

	u8 page = 0, start, limit;
	u8 draw = 1;

	clear_console();
	
	//
	//
	//--DCN: This is so I don't have to do this every time I test
	/*
	{
		index++;
		//
		sprintf(file_struct->path, "%s/%s", file_struct->path, dir[index].name);
		free(dir);
		return BROWSER_FILE_SELECTED;
	}
	//*/
	//
	//
	//
	
	
	while(1){
		PAD_ScanPads();
		WPAD_ScanPads();

		if((WPAD_ButtonsHeld(0) & WPAD_BUTTON_HOME) || (PAD_ButtonsHeld(0) & PAD_TRIGGER_Z))
			return BROWSER_CANCELED;

		if(GetHeld(UP, UP, UP)){
			if(index) index--;
			usleep(150000);
			draw = 1;
		}

		if(GetHeld(DOWN, DOWN, DOWN)){
			if(index < num_entries - 1) index++;
			usleep(150000);
			draw = 1;
		}

		if(GetInput(LEFT, LEFT, LEFT)){
			index = 0;
			draw = 1;
		}

		if(GetInput(RIGHT, RIGHT, RIGHT)){
			index = num_entries - 1;
			draw = 1;
		}

		if(GetInput(A, A, A)){
			if(index == 0 && strcmp(dir[index].name, "..") == 0) {
				browse_back(file_struct->path);
			}else{
				sprintf(file_struct->path, "%s/%s", file_struct->path, dir[index].name);
			}

			BOOL is_dir = (dir[index].attr & S_IFDIR);
			free(dir);
			if(is_dir) {
				return BROWSER_CHANGE_FOLDER;
			}
			else
				return BROWSER_FILE_SELECTED;
		}

		/*if(GetInput(B, B, B)) 
		{
			return BROWSER_CANCELED;
		}*/

		if(draw){
			u8 old_page = page;
			page = index / per_page;
			if(old_page != page){
				clear_console();
			}
			start = page * per_page;
			limit = ( num_entries > (start + per_page) ) ? ( start + per_page ) : num_entries;
			printf("\x1b[2J");
			printf("\x1B[2;2H");
			printf("\x1b[33m");
			printf("\t%s\n\n", file_struct->title);
			printf("\tbrowsing %s\n\n", file_struct->path);
			
			for(int temp = start; temp < limit; temp++){
				printf("\x1b[%um", (index == temp) ? 32 : 37);
				printf("\t%s\t%s\n", (dir[temp].attr & S_IFDIR) ? "DIR" : "   ", dir[temp].name);
			}

			printf("\x1b[37m");
			draw = 0;
		}
		VIDEO_WaitVSync();
	}
}

int FileBrowser( char *dir ) {

	file_browser_st game_filename;
	strcpy(game_filename.title, "Welcome to DeSmuME Wii!\n\nWARNING! If you paid for this software, you have been scammed!");

	sprintf(game_filename.path, "%s", dir);

	int ret = textFileBrowser(&game_filename);

	if(ret == BROWSER_FILE_NOT_FOUND){
		browse_back(game_filename.path);
		browse_back(game_filename.path);	// move to the root if no DS/ROMS folder found
		ret = textFileBrowser(&game_filename);
	}
	
	while(ret == BROWSER_CHANGE_FOLDER) {
		ret = textFileBrowser(&game_filename);
	}
	
	if (ret == BROWSER_FILE_SELECTED) {
		strcpy(dir, game_filename.path);
	}

	clear_console();
	return ret;
}
