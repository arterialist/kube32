// SPDX-License-Identifier: GPL-2.0-or-later
/* The sim-only build (make sim) leaves out the engine's screen-side files
 * (Makefile: SIM_EXCLUDE): menu, automap, intermission, finale, wipe, HUD,
 * status bar, savegames, the rasterizers, the screen, sound and joystick
 * drivers, the configuration file. What the rest of the engine still calls
 * into them is answered here: nothing to draw, no menu, no saves, and the
 * globals they owned with their default values. */
#include "include/k32libc.h"
#include "../doomgeneric/doomdef.h"
#include "../doomgeneric/doomtype.h"
#include "../doomgeneric/d_event.h"
#include "../doomgeneric/d_player.h"
#include "../doomgeneric/r_defs.h"
#include "../doomgeneric/r_draw.h"
#include "../doomgeneric/i_video.h"
#include "../doomgeneric/i_sound.h"
#include "../doomgeneric/sha1.h"
#include "../doomgeneric/v_patch.h"

/* ---- globals the excluded files defined ---- */
boolean automapactive, menuactive, inhelpscreens;
int screenblocks = 10, detailLevel = 0, showMessages = 1, mouseSensitivity = 5;
int snd_musicdevice = 0;
boolean screenvisible = true, screensaver_mode = false;
char* chat_macros[10] = {"", "", "", "", "", "", "", "", "", ""};
char* player_names[4] = {"Green: ", "Indigo: ", "Brown: ", "Red: "};
FILE* save_stream;
boolean savegame_error;
/* r_draw.c's: the renderer's column and span operands, and the view's size */
lighttable_t* dc_colormap;
int dc_x, dc_yl, dc_yh;
fixed_t dc_iscale, dc_texturemid;
byte* dc_source;
byte* dc_translation;
int ds_y, ds_x1, ds_x2;
lighttable_t* ds_colormap;
fixed_t ds_xfrac, ds_yfrac, ds_xstep, ds_ystep;
byte* ds_source;
int viewwidth, scaledviewwidth, viewheight, viewwindowx, viewwindowy;
static byte translation_store[3 * 256];
byte* translationtables = translation_store;

/* ---- the screen ---- */
void I_InitInput(void);
void I_GetEvent(void);
void I_InitGraphics(void) { I_InitInput(); }
void I_GraphicsCheckCommandLine(void) {}
void I_StartFrame(void) {}
void I_StartTic(void) { I_GetEvent(); }   /* the keys (i_input.c) */
void I_UpdateNoBlit(void) {}
void I_FinishUpdate(void) {}
void I_SetPalette(byte* palette) { (void)palette; }
void I_SetWindowTitle(char* title) { (void)title; }
void I_EnableLoadingDisk(void) {}
void I_DisplayFPSDots(boolean dots_on) { (void)dots_on; }
void I_CheckIsScreensaver(void) {}
void I_SetGrabMouseCallback(grabmouse_callback_t func) { (void)func; }
void I_BindVideoVariables(void) {}
void I_BeginRead(void) {}
void I_EndRead(void) {}
void I_Endoom(byte* data) { (void)data; }
void V_Init(void) {}
void V_DrawPatch(int x, int y, patch_t* patch) { (void)x; (void)y; (void)patch; }
void V_DrawPatchDirect(int x, int y, patch_t* patch) { (void)x; (void)y; (void)patch; }
void V_DrawMouseSpeedBox(int speed) { (void)speed; }
void V_RestoreBuffer(void) {}
void V_ScreenShot(char* format) { (void)format; }
void R_InitBuffer(int width, int height) {
    viewwindowx = (SCREENWIDTH - width) >> 1;
    viewwindowy = width == SCREENWIDTH ? 0 : (SCREENHEIGHT - 32 - height) >> 1;
}
void R_InitTranslationTables(void) {
    for (int i = 0; i < 256; i++) {
        translation_store[i] = translation_store[256 + i] = translation_store[512 + i] = (byte)i;
        if (i >= 0x70 && i <= 0x7f) {
            translation_store[i] = 0x60 + (i & 0xf);
            translation_store[256 + i] = 0x40 + (i & 0xf);
            translation_store[512 + i] = 0x20 + (i & 0xf);
        }
    }
}
void R_FillBackScreen(void) {}
void R_DrawViewBorder(void) {}
/* the rasterizers r_main.c installs before the draw list takes over */
void R_DrawColumn(void) {}
void R_DrawColumnLow(void) {}
void R_DrawFuzzColumn(void) {}
void R_DrawFuzzColumnLow(void) {}
void R_DrawTranslatedColumn(void) {}
void R_DrawTranslatedColumnLow(void) {}
void R_DrawSpan(void) {}
void R_DrawSpanLow(void) {}

/* ---- the menu, HUD, status bar, automap, intermission, finale ---- */
void M_Init(void) {}
void M_Ticker(void) {}
void M_Drawer(void) {}
boolean M_Responder(event_t* ev) { (void)ev; return false; }
void M_StartControlPanel(void) {}
void HU_Init(void) {}
void HU_Start(void) {}
void HU_Ticker(void) {}
void HU_Drawer(void) {}
void HU_Erase(void) {}
boolean HU_Responder(event_t* ev) { (void)ev; return false; }
char HU_dequeueChatChar(void) { return 0; }
void ST_Init(void) {}
void ST_Start(void) {}
void ST_Ticker(void) {}
void ST_Drawer(boolean fullscreen, boolean refresh) { (void)fullscreen; (void)refresh; }
boolean ST_Responder(event_t* ev) { (void)ev; return false; }
void AM_Stop(void) {}
void AM_Ticker(void) {}
void AM_Drawer(void) {}
boolean AM_Responder(event_t* ev) { (void)ev; return false; }
void WI_Start(wbstartstruct_t* wbstartstruct) { (void)wbstartstruct; }
void WI_Ticker(void) {}
void WI_Drawer(void) {}
void WI_End(void) {}
void F_StartFinale(void) {}
void F_Ticker(void) {}
void F_Drawer(void) {}
boolean F_Responder(event_t* ev) { (void)ev; return false; }
void StatCopy(wbstartstruct_t* stats) { (void)stats; }
void StatDump(void) {}

/* ---- the configuration file and savegames ---- */
void M_LoadDefaults(void) {}
void M_SaveDefaults(void) {}
void M_SetConfigDir(char* dir) { (void)dir; }
void M_SetConfigFilenames(char* main_config, char* extra_config) { (void)main_config; (void)extra_config; }
void M_BindVariable(char* name, void* variable) { (void)name; (void)variable; }
char* M_GetSaveGameDir(char* iwadname) { (void)iwadname; return ""; }
char* P_SaveGameFile(int slot) { (void)slot; return ""; }
char* P_TempSaveGameFile(void) { return ""; }
void P_ArchivePlayers(void) {}
void P_ArchiveWorld(void) {}
void P_ArchiveThinkers(void) {}
void P_ArchiveSpecials(void) {}
void P_UnArchivePlayers(void) {}
void P_UnArchiveWorld(void) {}
void P_UnArchiveThinkers(void) {}
void P_UnArchiveSpecials(void) {}
boolean P_ReadSaveGameHeader(void) { return false; }
boolean P_ReadSaveGameEOF(void) { return false; }
void P_WriteSaveGameHeader(char* description) { (void)description; }
void P_WriteSaveGameEOF(void) {}
void W_Checksum(sha1_digest_t digest) { memset(digest, 0, sizeof(sha1_digest_t)); }

/* ---- sound and joystick: no device ---- */
void I_InitSound(boolean use_sfx_prefix) { (void)use_sfx_prefix; }
void I_ShutdownSound(void) {}
void I_BindSoundVariables(void) {}
void I_PrecacheSounds(sfxinfo_t* sounds, int num_sounds) { (void)sounds; (void)num_sounds; }
int I_GetSfxLumpNum(sfxinfo_t* sfxinfo) { (void)sfxinfo; return -1; }
int I_StartSound(sfxinfo_t* sfxinfo, int channel, int vol, int sep) { (void)sfxinfo; (void)channel; (void)vol; (void)sep; return -1; }
void I_StopSound(int channel) { (void)channel; }
boolean I_SoundIsPlaying(int channel) { (void)channel; return false; }
void I_UpdateSound(void) {}
void I_UpdateSoundParams(int channel, int vol, int sep) { (void)channel; (void)vol; (void)sep; }
void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int volume) { (void)volume; }
void I_PauseSong(void) {}
void I_ResumeSong(void) {}
void* I_RegisterSong(void* data, int len) { (void)data; (void)len; return 0; }
void I_UnRegisterSong(void* handle) { (void)handle; }
void I_PlaySong(void* handle, boolean looping) { (void)handle; (void)looping; }
void I_StopSong(void) {}
void I_InitJoystick(void) {}
void I_BindJoystickVariables(void) {}
