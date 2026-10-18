/*
 * hostcall.h - host functions called by the game (see the hcalls[] table
 * in hostcall.c).
 */
#ifndef HOSTCALL_H
#define HOSTCALL_H

#include "m68000.h"

/* hostcall.c */
void Call_DumpRegs(void);
void Call_DumpDebug(void);

/* host_screen.c */
void Call_Memset(void);
void Call_MemsetBlue(void);
void Call_Memcpy(void);
void Call_BlitCursor(void);
void Call_RestoreUnderCursor(void);
void Call_PutPix(void);
void Call_FillLine(void);
void Call_OldHLine(void);
void Call_HLine(void);
void Call_BackHLine(void);
void Call_BlitBmp(void);
void Call_DrawStr(void);
void Call_DrawStrShadowed(void);
void Call_SetMainPalette(void);
void Call_SetCtrlPalette(void);
void Call_InformScreens(void);
void Call_SetScreenBase(void);

typedef struct
{
	unsigned short main[256], ctrl[16], ext[240];
	int len_main, ext_len, bgcol, mouse_shown;
	unsigned long base;
	unsigned char *raster;
} HostScreenState;
void host_screen_save(HostScreenState *s);
void host_screen_restore(const HostScreenState *s);
void Call_MakeExtPalette(void);

/* host_files.c */
void Call_Fread(void);
void Call_GameStart(void);
void Call_Fwrite(void);
void Call_Fdelete(void);
void Call_Fopendir(void);
void Call_Freaddir(void);
void Call_Fclosedir(void);

#endif /* HOSTCALL_H */
