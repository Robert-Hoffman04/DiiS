#ifndef GEKKO
# include <stdlib.h>
# include <string.h>
#endif
#include <stdio.h>
#include <stdarg.h>

#include "log.h"

// #define _FLOG

#ifdef _FLOG
static FILE *flog;
#endif

void Log_Init() {
#ifdef _FLOG
# ifdef GEKKO
	flog = fopen("sd:/dslog.log", "wb");
# else
	char *filename = getenv("HOME");
	strcat(filename, "/dslog.log");
	flog = fopen(filename, "wb");
# endif
#endif
}

void Log_DeInit() {
#ifdef _FLOG
	if(flog)
		fclose(flog);
#endif
}

void Log_fprintf(const char *msg, ...) {
#ifdef _FLOG
	va_list args;
	va_start(args, msg);
	vfprintf(flog, msg, args);
	va_end(args);
#endif
}
