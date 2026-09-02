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
//--DCN: Copied from earlier version of desmumeWii
#include <stdio.h>
#include <stdlib.h>
#include <gccore.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <wiiuse/wpad.h>
//#include "FrontEnd.h"


typedef struct configparm{
	char name[32];
	long *var;
}configP;

configP configparms[30];
int totalconfig=0;


void InitConfigParms(){
	
}

int selposconfig=0;
void DisplayConfigParms(){
	int c;

	for (c=0;c<totalconfig;c++){
	 	
	    if(selposconfig == c)
		{
			
		}else {
			
		}
		
		
	}

}

int frameposconfig=0;
int langposconfig=0;

void DoConfig()
{

	

}

//5SM2SF


typedef struct fname{
	char name[256];
}f_name;

typedef struct flist{
	f_name fname[256];
	int cnt;
}f_list;

f_list filelist;

void ClearFileList(){
	filelist.cnt =0;
}


int HasExtension(char *filename){
	if(filename[strlen(filename)-4] == '.'){
		return 1;
	}
	return 0;
}


void GetExtension(const char *srcfile,char *outext){
	if(HasExtension((char *)srcfile)){
		strcpy(outext,srcfile + strlen(srcfile) - 3);
	}else{
		strcpy(outext,"");
	}
}

enum {
	EXT_NDS = 1,
	EXT_GZ = 2,
	EXT_ZIP = 4,
	EXT_UNKNOWN = 8,
};

const struct {
	const char *szExt;
	int nExtId;
} stExtentions[] = {
	{"nds",EXT_NDS},
//	{"gz",EXT_GZ},
//	{"zip",EXT_ZIP},
	{NULL, EXT_UNKNOWN}
};

int getExtId(const char *szFilePath) {
	char *pszExt;

	if ((pszExt = strrchr(szFilePath, '.'))) {
		pszExt++;
		int i;
		for (i = 0; stExtentions[i].nExtId != EXT_UNKNOWN; i++) {
			if (!strcasecmp(stExtentions[i].szExt,pszExt)) {
				return stExtentions[i].nExtId;
			}
		}
	}

	return EXT_UNKNOWN;
}

void GetFileList(const char *root)
{
	
}


void DisplayFileList()
{
	
}

char app_path[128];
char romname[256];


void DSEmuGui(char *path,char *out)
{
	
}


//--DCN: End copy

