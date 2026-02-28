/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-ui-console - main.c                                       *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2007-2018 Richard42                                     *
 *   Copyright (C) 2008 Ebenblues Nmn Okaygo Tillin9                       *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* This is the main application entry point for the console-only front-end
 * for Mupen64Plus v2.0.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <SDL.h>
#include <SDL_main.h>
#include <SDL_thread.h>

#include "cheat.h"
#include "compare_core.h"
#include "core_interface.h"
#include "debugger.h"
#include "m64p_types.h"
#include "main.h"
#include "osal_preproc.h"
#include "osal_files.h"
#include "plugin.h"
#include "version.h"

#ifndef WIN32
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef VIDEXT_HEADER
#define xstr(s) str(s)
#define str(s) #s
#include xstr(VIDEXT_HEADER)
#endif

#define PIF_ROM_SIZE 2048

/* Version number for UI-Console config section parameters */
#define CONFIG_PARAM_VERSION     1.00

/** global variables **/
int    g_Verbose = 0;

/** static (local) variables **/
static m64p_handle l_ConfigCore = NULL;
static m64p_handle l_ConfigVideo = NULL;
static m64p_handle l_ConfigUI = NULL;
static m64p_handle l_ConfigTransferPak = NULL;
static m64p_handle l_Config64DD = NULL;

static const char *l_CoreLibPath = NULL;
static const char *l_ConfigDirPath = NULL;
static const char *l_ROMFilepath = NULL;       // filepath of ROM to load & run at startup
static const char *l_SaveStatePath = NULL;     // save state to load at startup

#if defined(SHAREDIR)
  static const char *l_DataDirPath = SHAREDIR;
#else
  static const char *l_DataDirPath = NULL;
#endif

static int  *l_TestShotList = NULL;      // list of screenshots to take for regression test support
static int   l_TestShotIdx = 0;          // index of next screenshot frame in list
static int   l_SaveOptions = 1;          // save command-line options in configuration file (enabled by default)
static int   l_CoreCompareMode = 0;      // 0 = disable, 1 = send, 2 = receive
static int   l_LaunchDebugger = 0;
static const char *l_AgentServerEndpoint = NULL;
static int   l_AgentMode = 0;
static int   l_AgentProfileMode = 0;
static SDL_atomic_t l_LastFrame;
static int   l_AgentServerStop = 0;
static SDL_Thread *l_AgentServerThread = NULL;
static SDL_mutex *l_AgentMutex = NULL;
static SDL_atomic_t l_StateLoadEventSeq;
static SDL_atomic_t l_StateLoadLastResult;
static SDL_atomic_t l_StateSaveEventSeq;
static SDL_atomic_t l_StateSaveLastResult;
static SDL_atomic_t l_ScreenshotEventSeq;
static SDL_atomic_t l_ScreenshotLastResult;
static uint32_t l_AgentInputShadow[4];
#ifndef WIN32
static int   l_AgentListenFd = -1;
static int   l_AgentClientFd = -1;
static char  l_AgentSocketPath[sizeof(((struct sockaddr_un*)0)->sun_path)];
#endif

static eCheatMode l_CheatMode = CHEAT_DISABLE;
static char      *l_CheatNumList = NULL;

static void CoreStateChanged(void *Context, m64p_core_param Param, int Value);

enum
{
    AGENT_BTN_DPAD_R = 0x0001,
    AGENT_BTN_DPAD_L = 0x0002,
    AGENT_BTN_DPAD_D = 0x0004,
    AGENT_BTN_DPAD_U = 0x0008,
    AGENT_BTN_START  = 0x0010,
    AGENT_BTN_Z      = 0x0020,
    AGENT_BTN_B      = 0x0040,
    AGENT_BTN_A      = 0x0080,
    AGENT_BTN_C_R    = 0x0100,
    AGENT_BTN_C_L    = 0x0200,
    AGENT_BTN_C_D    = 0x0400,
    AGENT_BTN_C_U    = 0x0800,
    AGENT_BTN_R      = 0x1000,
    AGENT_BTN_L      = 0x2000
};

enum
{
    AGENT_PROFILE_NONE = 0,
    AGENT_PROFILE_WATCH = 1,
    AGENT_PROFILE_TRAIN = 2
};

struct agent_framebuffer_preset
{
    const char* name;
    int x_milli;
    int y_milli;
    int w_milli;
    int h_milli;
    const char* description;
};

static const struct agent_framebuffer_preset l_AgentFramebufferPresets[] =
{
    { "full", 0, 0, 1000, 1000, "full frame" },
    { "hud", 0, 0, 1000, 220, "top HUD band" },
    { "dialog", 0, 620, 1000, 380, "bottom dialog area" },
    { "battle_ui", 0, 470, 1000, 530, "battle command UI area" },
    { "action_command", 260, 360, 480, 260, "timing / action-command focus region" }
};

/*********************************************************************************************************
 *  Callback functions from the core
 */

void DebugMessage(int level, const char *message, ...)
{
  char msgbuf[1024];
  va_list args;

  va_start(args, message);
  vsnprintf(msgbuf, 1024, message, args);

  DebugCallback("UI-Console", level, msgbuf);

  va_end(args);
}

void DebugCallback(void *Context, int level, const char *message)
{
#ifdef ANDROID
    if (level == M64MSG_ERROR)
        __android_log_print(ANDROID_LOG_ERROR, (const char *) Context, "%s", message);
    else if (level == M64MSG_WARNING)
        __android_log_print(ANDROID_LOG_WARN, (const char *) Context, "%s", message);
    else if (level == M64MSG_INFO)
        __android_log_print(ANDROID_LOG_INFO, (const char *) Context, "%s", message);
    else if (level == M64MSG_STATUS)
        __android_log_print(ANDROID_LOG_DEBUG, (const char *) Context, "%s", message);
    else if (level == M64MSG_VERBOSE)
    {
        if (g_Verbose)
            __android_log_print(ANDROID_LOG_VERBOSE, (const char *) Context, "%s", message);
    }
    else
        __android_log_print(ANDROID_LOG_ERROR, (const char *) Context, "Unknown: %s", message);
#else
    if (level == M64MSG_ERROR)
        printf("%s Error: %s\n", (const char *) Context, message);
    else if (level == M64MSG_WARNING)
        printf("%s Warning: %s\n", (const char *) Context, message);
    else if (level == M64MSG_INFO)
        printf("%s: %s\n", (const char *) Context, message);
    else if (level == M64MSG_STATUS)
        printf("%s Status: %s\n", (const char *) Context, message);
    else if (level == M64MSG_VERBOSE)
    {
        if (g_Verbose)
            printf("%s: %s\n", (const char *) Context, message);
    }
    else
        printf("%s Unknown: %s\n", (const char *) Context, message);
#endif
}

static void FrameCallback(unsigned int FrameIndex)
{
    SDL_AtomicSet(&l_LastFrame, (int) FrameIndex);

    // take a screenshot if we need to
    if (l_TestShotList != NULL)
    {
        int nextshot = l_TestShotList[l_TestShotIdx];
        if (nextshot == FrameIndex)
        {
            (*CoreDoCommand)(M64CMD_TAKE_NEXT_SCREENSHOT, 0, NULL);  /* tell the core take a screenshot */
            // advance list index to next screenshot frame number.  If it's 0, then quit
            l_TestShotIdx++;
        }
        else if (nextshot == 0)
        {
            (*CoreDoCommand)(M64CMD_STOP, 0, NULL);  /* tell the core to shut down ASAP */
            free(l_TestShotList);
            l_TestShotList = NULL;
        }
    }
}

static char *formatstr(const char *fmt, ...) ATTR_FMT(1, 2);

char *formatstr(const char *fmt, ...)
{
	int size = 128, ret;
	char *str = (char *)malloc(size), *newstr;
	va_list args;

	/* There are two implementations of vsnprintf we have to deal with:
	 * C99 version: Returns the number of characters which would have been written
	 *              if the buffer had been large enough, and -1 on failure.
	 * Windows version: Returns the number of characters actually written,
	 *                  and -1 on failure or truncation.
	 * NOTE: An implementation equivalent to the Windows one appears in glibc <2.1.
	 */
	while (str != NULL)
	{
		va_start(args, fmt);
		ret = vsnprintf(str, size, fmt, args);
		va_end(args);

		// Successful result?
		if (ret >= 0 && ret < size)
			return str;

		// Increment the capacity of the buffer
		if (ret >= size)
			size = ret + 1; // C99 version: We got the needed buffer size
		else
			size *= 2; // Windows version: Keep guessing

		newstr = (char *)realloc(str, size);
		if (newstr == NULL)
			free(str);
		str = newstr;
	}

	return NULL;
}

static int is_path_separator(char c)
{
    return strchr(OSAL_DIR_SEPARATORS, c) != NULL;
}

char* combinepath(const char* first, const char *second)
{
    size_t len_first, off_second = 0;

    if (first == NULL || second == NULL)
        return NULL;

    len_first = strlen(first);

    while (is_path_separator(first[len_first-1]))
        len_first--;

    while (is_path_separator(second[off_second]))
        off_second++;

    return formatstr("%.*s%c%s", (int) len_first, first, OSAL_DIR_SEPARATORS[0], second + off_second);
}


/*********************************************************************************************************
 *  Configuration handling
 */

static m64p_error OpenConfigurationHandles(void)
{
    float fConfigParamsVersion;
    int bSaveConfig = 0;
    m64p_error rval;
    unsigned int i;

    /* Open Configuration sections for core library and console User Interface */
    rval = (*ConfigOpenSection)("Core", &l_ConfigCore);
    if (rval != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_ERROR, "failed to open 'Core' configuration section");
        return rval;
    }

    rval = (*ConfigOpenSection)("Video-General", &l_ConfigVideo);
    if (rval != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_ERROR, "failed to open 'Video-General' configuration section");
        return rval;
    }

    rval = (*ConfigOpenSection)("Transferpak", &l_ConfigTransferPak);
    if (rval != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_ERROR, "failed to open 'Transferpak' configuration section");
        return rval;
    }

    rval = (*ConfigOpenSection)("64DD", &l_Config64DD);
    if (rval != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_ERROR, "failed to open '64DD' configuration section");
        return rval;
    }

    rval = (*ConfigOpenSection)("UI-Console", &l_ConfigUI);
    if (rval != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_ERROR, "failed to open 'UI-Console' configuration section");
        return rval;
    }

    if ((*ConfigGetParameter)(l_ConfigUI, "Version", M64TYPE_FLOAT, &fConfigParamsVersion, sizeof(float)) != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_WARNING, "No version number in 'UI-Console' config section. Setting defaults.");
        (*ConfigDeleteSection)("UI-Console");
        (*ConfigOpenSection)("UI-Console", &l_ConfigUI);
        bSaveConfig = 1;
    }
    else if (((int) fConfigParamsVersion) != ((int) CONFIG_PARAM_VERSION))
    {
        DebugMessage(M64MSG_WARNING, "Incompatible version %.2f in 'UI-Console' config section: current is %.2f. Setting defaults.", fConfigParamsVersion, (float) CONFIG_PARAM_VERSION);
        (*ConfigDeleteSection)("UI-Console");
        (*ConfigOpenSection)("UI-Console", &l_ConfigUI);
        bSaveConfig = 1;
    }
    else if ((CONFIG_PARAM_VERSION - fConfigParamsVersion) >= 0.0001f)
    {
        /* handle upgrades */
        float fVersion = CONFIG_PARAM_VERSION;
        ConfigSetParameter(l_ConfigUI, "Version", M64TYPE_FLOAT, &fVersion);
        DebugMessage(M64MSG_INFO, "Updating parameter set version in 'UI-Console' config section to %.2f", fVersion);
        bSaveConfig = 1;
    }

    /* Set default values for my Config parameters */
    (*ConfigSetDefaultFloat)(l_ConfigUI, "Version", CONFIG_PARAM_VERSION,  "Mupen64Plus UI-Console config parameter set version number.  Please don't change this version number.");
    (*ConfigSetDefaultString)(l_ConfigUI, "PluginDir", OSAL_CURRENT_DIR, "Directory in which to search for plugins");
    (*ConfigSetDefaultString)(l_ConfigUI, "VideoPlugin", "mupen64plus-video-rice" OSAL_DLL_EXTENSION, "Filename of video plugin");
    (*ConfigSetDefaultString)(l_ConfigUI, "AudioPlugin", "mupen64plus-audio-sdl" OSAL_DLL_EXTENSION, "Filename of audio plugin");
    (*ConfigSetDefaultString)(l_ConfigUI, "InputPlugin", "mupen64plus-input-sdl" OSAL_DLL_EXTENSION, "Filename of input plugin");
    (*ConfigSetDefaultString)(l_ConfigUI, "RspPlugin", "mupen64plus-rsp-hle" OSAL_DLL_EXTENSION, "Filename of RSP plugin");

    for(i = 1; i < 5; ++i) {
        char key[64];
        char desc[2048];
#define SET_DEFAULT_STRING(key_fmt, default_value, desc_fmt) \
        do { \
            snprintf(key, sizeof(key), key_fmt, i); \
            snprintf(desc, sizeof(desc), desc_fmt, i); \
            (*ConfigSetDefaultString)(l_ConfigTransferPak, key, default_value, desc); \
        } while(0)

        SET_DEFAULT_STRING("GB-rom-%u", "", "Filename of the GB ROM to load into transferpak %u");
        SET_DEFAULT_STRING("GB-ram-%u", "", "Filename of the GB RAM to load into transferpak %u");
#undef SET_DEFAULT_STRING
    }

    (*ConfigSetDefaultString)(l_Config64DD, "IPL-ROM", "", "Filename of the 64DD IPL ROM");
    (*ConfigSetDefaultString)(l_Config64DD, "Disk", "", "Filename of the disk to load into Disk Drive");

    if (bSaveConfig && l_SaveOptions && ConfigSaveSection != NULL) { /* ConfigSaveSection was added in Config API v2.1.0 */
        (*ConfigSaveSection)("UI-Console");
        (*ConfigSaveSection)("Transferpak");
    }

    return M64ERR_SUCCESS;
}

static m64p_error SaveConfigurationOptions(void)
{
    /* if shared data directory was given on the command line, write it into the config file */
    if (l_DataDirPath != NULL)
        (*ConfigSetParameter)(l_ConfigCore, "SharedDataPath", M64TYPE_STRING, l_DataDirPath);

    /* if any plugin filepaths were given on the command line, write them into the config file */
    if (g_PluginDir != NULL)
        (*ConfigSetParameter)(l_ConfigUI, "PluginDir", M64TYPE_STRING, g_PluginDir);
    if (g_GfxPlugin != NULL)
        (*ConfigSetParameter)(l_ConfigUI, "VideoPlugin", M64TYPE_STRING, g_GfxPlugin);
    if (g_AudioPlugin != NULL)
        (*ConfigSetParameter)(l_ConfigUI, "AudioPlugin", M64TYPE_STRING, g_AudioPlugin);
    if (g_InputPlugin != NULL)
        (*ConfigSetParameter)(l_ConfigUI, "InputPlugin", M64TYPE_STRING, g_InputPlugin);
    if (g_RspPlugin != NULL)
        (*ConfigSetParameter)(l_ConfigUI, "RspPlugin", M64TYPE_STRING, g_RspPlugin);

    if ((*ConfigHasUnsavedChanges)(NULL))
        return (*ConfigSaveFile)();
    else
        return M64ERR_SUCCESS;
}

/*********************************************************************************************************
 *  Command-line parsing
 */

static void printUsage(const char *progname)
{
    printf("Usage: %s [parameters] [romfile]\n"
           "\n"
           "Parameters:\n"
           "    --noosd                : disable onscreen display\n"
           "    --osd                  : enable onscreen display\n"
           "    --fullscreen           : use fullscreen display mode\n"
           "    --windowed             : use windowed display mode\n"
           "    --resolution (res)     : display resolution (640x480, 800x600, 1024x768, etc)\n"
           "    --nospeedlimit         : disable core speed limiter (should be used with dummy audio plugin)\n"
           "    --cheats (cheat-spec)  : enable or list cheat codes for the given rom file\n"
           "    --corelib (filepath)   : use core library (filepath) (can be only filename or full path)\n"
           "    --configdir (dir)      : force configation directory to (dir); should contain mupen64plus.cfg\n"
           "    --datadir (dir)        : search for shared data files (.ini files, languages, etc) in (dir)\n"
           "    --debug                : launch console-based debugger (requires core lib built for debugging)\n"
           "    --plugindir (dir)      : search for plugins in (dir)\n"
           "    --sshotdir (dir)       : set screenshot directory to (dir)\n"
           "    --gfx (plugin-spec)    : use gfx plugin given by (plugin-spec)\n"
           "    --audio (plugin-spec)  : use audio plugin given by (plugin-spec)\n"
           "    --input (plugin-spec)  : use input plugin given by (plugin-spec)\n"
           "    --rsp (plugin-spec)    : use rsp plugin given by (plugin-spec)\n"
           "    --emumode (mode)       : set emu mode to: 0=Pure Interpreter 1=Interpreter 2=DynaRec\n"
           "    --savestate (filepath) : savestate loaded at startup\n"
           "    --testshots (list)     : take screenshots at frames given in comma-separated (list), then quit\n"
           "    --set (param-spec)     : set a configuration variable, format: ParamSection[ParamName]=Value\n"
           "    --gb-rom-{1,2,3,4}     : define GB cart rom to load inside transferpak {1,2,3,4}\n"
           "    --gb-ram-{1,2,3,4}     : define GB cart ram to load inside transferpak {1,2,3,4}\n"
           "    --dd-ipl-rom           : define 64DD IPL rom\n"
           "    --dd-disk              : define disk to load into the disk drive\n"
           "    --core-compare-send    : use the Core Comparison debugging feature, in data sending mode\n"
           "    --core-compare-recv    : use the Core Comparison debugging feature, in data receiving mode\n"
           "    --agent-server (path)  : enable JSON CLI server on unix socket path (forces windowed max 4:3)\n"
           "    --agent-profile (mode) : apply preset for agents: watch|train\n"
           "    --nosaveoptions        : do not save the given command-line options in configuration file\n"
           "    --pif (filepath)       : use a binary PIF ROM (filepath) instead of HLE PIF\n"
           "    --verbose              : print lots of information\n"
           "    --help                 : see this help message\n\n"
           "(plugin-spec):\n"
           "    (pluginname)           : filename (without path) of plugin to find in plugin directory\n"
           "    (pluginpath)           : full path and filename of plugin\n"
           "    'dummy'                : use dummy plugin\n\n"
           "(cheat-spec):\n"
           "    'list'                 : show all of the available cheat codes\n"
           "    'all'                  : enable all of the available cheat codes\n"
           "    (codelist)             : a comma-separated list of cheat code numbers to enable,\n"
           "                             with dashes to use code variables (ex 1-2 to use cheat 1 option 2)\n"
           "\n", progname);

    return;
}

static int SetConfigParameter(const char *ParamSpec)
{
    char *ParsedString, *VarName, *VarValue=NULL;
    m64p_handle ConfigSection;
    m64p_type VarType;
    m64p_error rval;

    if (ParamSpec == NULL)
    {
        DebugMessage(M64MSG_ERROR, "ParamSpec is NULL in SetConfigParameter()");
        return 1;
    }

    /* make a copy of the input string */
    ParsedString = (char *) malloc(strlen(ParamSpec) + 1);
    if (ParsedString == NULL)
    {
        DebugMessage(M64MSG_ERROR, "SetConfigParameter() couldn't allocate memory for temporary string.");
        return 2;
    }
    strcpy(ParsedString, ParamSpec);

    /* parse it for the simple section[name]=value format */
    VarName = strchr(ParsedString, '[');
    if (VarName != NULL)
    {
        *VarName++ = 0;
        VarValue = strchr(VarName, ']');
        if (VarValue != NULL)
        {
            *VarValue++ = 0;
        }
    }
    if (VarName == NULL || VarValue == NULL || *VarValue != '=')
    {
        DebugMessage(M64MSG_ERROR, "invalid (param-spec) '%s'", ParamSpec);
        free(ParsedString);
        return 3;
    }
    VarValue++;

    /* then set the value */
    rval = (*ConfigOpenSection)(ParsedString, &ConfigSection);
    if (rval != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_ERROR, "SetConfigParameter failed to open config section '%s'", ParsedString);
        free(ParsedString);
        return 4;
    }
    if ((*ConfigGetParameterType)(ConfigSection, VarName, &VarType) == M64ERR_SUCCESS)
    {
        switch(VarType)
        {
            int ValueInt;
            float ValueFloat;
            case M64TYPE_INT:
                ValueInt = atoi(VarValue);
                ConfigSetParameter(ConfigSection, VarName, M64TYPE_INT, &ValueInt);
                break;
            case M64TYPE_FLOAT:
                ValueFloat = (float) atof(VarValue);
                ConfigSetParameter(ConfigSection, VarName, M64TYPE_FLOAT, &ValueFloat);
                break;
            case M64TYPE_BOOL:
                ValueInt = (int) (osal_insensitive_strcmp(VarValue, "true") == 0);
                ConfigSetParameter(ConfigSection, VarName, M64TYPE_BOOL, &ValueInt);
                break;
            case M64TYPE_STRING:
                ConfigSetParameter(ConfigSection, VarName, M64TYPE_STRING, VarValue);
                break;
            default:
                DebugMessage(M64MSG_ERROR, "invalid VarType in SetConfigParameter()");
                return 5;
        }
    }
    else
    {
        ConfigSetParameter(ConfigSection, VarName, M64TYPE_STRING, VarValue);
    }

    free(ParsedString);
    return 0;
}

static int *ParseNumberList(const char *InputString, int *ValuesFound)
{
    const char *str;
    int *OutputList;

    /* count the number of integers in the list */
    int values = 1;
    str = InputString;
    while ((str = strchr(str, ',')) != NULL)
    {
        str++;
        values++;
    }

    /* create a list and populate it with the frame counter values at which to take screenshots */
    if ((OutputList = (int *) malloc(sizeof(int) * (values + 1))) != NULL)
    {
        int idx = 0;
        str = InputString;
        while (str != NULL)
        {
            OutputList[idx++] = atoi(str);
            str = strchr(str, ',');
            if (str != NULL) str++;
        }
        OutputList[idx] = 0;
    }

    if (ValuesFound != NULL)
        *ValuesFound = values;
    return OutputList;
}

static void SetMax43WindowForAgentMode(void)
{
    int fullscreen = 0;
    int screen_w = 1024;
    int screen_h = 768;
    int desktop_w = 0;
    int desktop_h = 0;
    SDL_DisplayMode display_mode;
    int video_was_init = SDL_WasInit(SDL_INIT_VIDEO);

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0)
    {
        if (SDL_GetDesktopDisplayMode(0, &display_mode) == 0)
        {
            desktop_w = display_mode.w;
            desktop_h = display_mode.h;
        }
        else if (SDL_GetCurrentDisplayMode(0, &display_mode) == 0)
        {
            desktop_w = display_mode.w;
            desktop_h = display_mode.h;
        }
    }

    if (desktop_w > 0 && desktop_h > 0)
    {
        if (desktop_w * 3 > desktop_h * 4)
            desktop_w = (desktop_h * 4) / 3;
        else
            desktop_h = (desktop_w * 3) / 4;

        screen_w = desktop_w;
        screen_h = desktop_h;
    }

    (*ConfigSetParameter)(l_ConfigVideo, "Fullscreen", M64TYPE_BOOL, &fullscreen);
    (*ConfigSetParameter)(l_ConfigVideo, "ScreenWidth", M64TYPE_INT, &screen_w);
    (*ConfigSetParameter)(l_ConfigVideo, "ScreenHeight", M64TYPE_INT, &screen_h);

    DebugMessage(M64MSG_INFO,
        "Agent mode window size set to %dx%d (max 4:3 within display)",
        screen_w, screen_h);

    if (!video_was_init)
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static void ApplyAgentProfile(void)
{
    int value = 0;

    if (l_AgentProfileMode == AGENT_PROFILE_NONE)
        return;

    if (l_AgentProfileMode == AGENT_PROFILE_WATCH)
    {
        value = 1;
        (*ConfigSetParameter)(l_ConfigCore, "OnScreenDisplay", M64TYPE_BOOL, &value);
        if ((*CoreDoCommand)(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &value) != M64ERR_SUCCESS)
            DebugMessage(M64MSG_WARNING, "failed to apply watch profile speed limiter setting");
        DebugMessage(M64MSG_INFO, "applied agent profile: watch (OSD on, speed limiter on)");
    }
    else if (l_AgentProfileMode == AGENT_PROFILE_TRAIN)
    {
        value = 0;
        (*ConfigSetParameter)(l_ConfigCore, "OnScreenDisplay", M64TYPE_BOOL, &value);
        if ((*CoreDoCommand)(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &value) != M64ERR_SUCCESS)
            DebugMessage(M64MSG_WARNING, "failed to apply train profile speed limiter setting");
        DebugMessage(M64MSG_INFO, "applied agent profile: train (OSD off, speed limiter off)");
    }
}

static const char* AgentFindValue(const char *json, const char *key)
{
    char key_pattern[128];
    const char *p = NULL;
    size_t key_len;

    if (json == NULL || key == NULL)
        return NULL;

    key_len = strlen(key);
    if (key_len + 3 >= sizeof(key_pattern))
        return NULL;

    snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", key);
    p = strstr(json, key_pattern);
    if (p == NULL)
        return NULL;

    p += strlen(key_pattern);
    while (*p != '\0' && isspace((unsigned char) *p))
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p != '\0' && isspace((unsigned char) *p))
        p++;

    return p;
}

static int AgentGetInt(const char *json, const char *key, int *value)
{
    char *endptr = NULL;
    long v;
    const char *p = AgentFindValue(json, key);

    if (p == NULL || value == NULL)
        return 0;

    v = strtol(p, &endptr, 10);
    if (endptr == p)
        return 0;

    *value = (int) v;
    return 1;
}

static int AgentGetUInt32(const char *json, const char *key, uint32_t *value)
{
    char *endptr = NULL;
    unsigned long v;
    const char *p = AgentFindValue(json, key);

    if (p == NULL || value == NULL)
        return 0;

    v = strtoul(p, &endptr, 0);
    if (endptr == p)
        return 0;

    *value = (uint32_t) v;
    return 1;
}

static int AgentGetBool(const char *json, const char *key, int *value)
{
    const char *p = AgentFindValue(json, key);

    if (p == NULL || value == NULL)
        return 0;

    if (strncmp(p, "true", 4) == 0)
    {
        *value = 1;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0)
    {
        *value = 0;
        return 1;
    }

    return AgentGetInt(json, key, value);
}

static int AgentGetString(const char *json, const char *key, char *out, size_t out_size)
{
    size_t out_idx = 0;
    const char *p = AgentFindValue(json, key);

    if (p == NULL || out == NULL || out_size == 0 || *p != '"')
        return 0;
    p++;

    while (*p != '\0' && *p != '"' && out_idx + 1 < out_size)
    {
        if (*p == '\\' && p[1] != '\0')
            p++;
        out[out_idx++] = *p++;
    }

    if (*p != '"')
        return 0;

    out[out_idx] = '\0';
    return 1;
}

static int AgentParseControlPort(const char *json, const char *key, unsigned int *control_id)
{
    int port = 0;

    if (!AgentGetInt(json, key, &port) || control_id == NULL)
        return 0;
    if (port >= 1 && port <= 4)
    {
        *control_id = (unsigned int) (port - 1);
        return 1;
    }
    if (port >= 0 && port <= 3)
    {
        *control_id = (unsigned int) port;
        return 1;
    }

    return 0;
}

static uint32_t AgentButtonMaskFromName(const char *name)
{
    if (name == NULL)
        return 0;

    if (osal_insensitive_strcmp(name, "a") == 0) return AGENT_BTN_A;
    if (osal_insensitive_strcmp(name, "b") == 0) return AGENT_BTN_B;
    if (osal_insensitive_strcmp(name, "z") == 0) return AGENT_BTN_Z;
    if (osal_insensitive_strcmp(name, "start") == 0) return AGENT_BTN_START;
    if (osal_insensitive_strcmp(name, "l") == 0) return AGENT_BTN_L;
    if (osal_insensitive_strcmp(name, "r") == 0) return AGENT_BTN_R;
    if (osal_insensitive_strcmp(name, "dpad_up") == 0 || osal_insensitive_strcmp(name, "du") == 0) return AGENT_BTN_DPAD_U;
    if (osal_insensitive_strcmp(name, "dpad_down") == 0 || osal_insensitive_strcmp(name, "dd") == 0) return AGENT_BTN_DPAD_D;
    if (osal_insensitive_strcmp(name, "dpad_left") == 0 || osal_insensitive_strcmp(name, "dl") == 0) return AGENT_BTN_DPAD_L;
    if (osal_insensitive_strcmp(name, "dpad_right") == 0 || osal_insensitive_strcmp(name, "dr") == 0) return AGENT_BTN_DPAD_R;
    if (osal_insensitive_strcmp(name, "c_up") == 0 || osal_insensitive_strcmp(name, "cu") == 0) return AGENT_BTN_C_U;
    if (osal_insensitive_strcmp(name, "c_down") == 0 || osal_insensitive_strcmp(name, "cd") == 0) return AGENT_BTN_C_D;
    if (osal_insensitive_strcmp(name, "c_left") == 0 || osal_insensitive_strcmp(name, "cl") == 0) return AGENT_BTN_C_L;
    if (osal_insensitive_strcmp(name, "c_right") == 0 || osal_insensitive_strcmp(name, "cr") == 0) return AGENT_BTN_C_R;
    return 0;
}

static uint32_t AgentBuildStateWithStick(uint32_t state, int x, int y)
{
    uint8_t ux = (uint8_t) ((int8_t) x);
    uint8_t uy = (uint8_t) ((int8_t) y);
    state &= 0x0000ffffU;
    state |= ((uint32_t) ux) << 16;
    state |= ((uint32_t) uy) << 24;
    return state;
}

static const struct agent_framebuffer_preset* AgentFindFramebufferPreset(const char* name)
{
    size_t i;

    if (name == NULL)
        return NULL;

    for (i = 0; i < sizeof(l_AgentFramebufferPresets) / sizeof(l_AgentFramebufferPresets[0]); ++i)
    {
        if (osal_insensitive_strcmp(name, l_AgentFramebufferPresets[i].name) == 0)
            return &l_AgentFramebufferPresets[i];
    }

    return NULL;
}

static void AgentApplyFramebufferPresetCrop(
    const struct agent_framebuffer_preset* preset,
    int width,
    int height,
    int* crop_x,
    int* crop_y,
    int* crop_w,
    int* crop_h)
{
    int x;
    int y;
    int w;
    int h;

    if (preset == NULL || crop_x == NULL || crop_y == NULL || crop_w == NULL || crop_h == NULL
        || width <= 0 || height <= 0)
    {
        return;
    }

    x = (width * preset->x_milli) / 1000;
    y = (height * preset->y_milli) / 1000;
    w = (width * preset->w_milli) / 1000;
    h = (height * preset->h_milli) / 1000;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= width) x = width - 1;
    if (y >= height) y = height - 1;

    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (x + w > width) w = width - x;
    if (y + h > height) h = height - y;

    if (w < 1) w = 1;
    if (h < 1) h = 1;

    *crop_x = x;
    *crop_y = y;
    *crop_w = w;
    *crop_h = h;
}

static int AgentBuildFramebufferPresetListResult(char *out, size_t out_size)
{
    size_t used = 0;
    size_t i;
    int wrote = 0;

    if (out == NULL || out_size < 32)
        return 0;

    wrote = snprintf(out, out_size, "{\"presets\":[");
    if (wrote < 0 || (size_t) wrote >= out_size)
        return 0;
    used = (size_t) wrote;

    for (i = 0; i < sizeof(l_AgentFramebufferPresets) / sizeof(l_AgentFramebufferPresets[0]); ++i)
    {
        const struct agent_framebuffer_preset* preset = &l_AgentFramebufferPresets[i];
        wrote = snprintf(
            out + used,
            out_size - used,
            "%s{\"name\":\"%s\",\"x_milli\":%d,\"y_milli\":%d,\"w_milli\":%d,\"h_milli\":%d,\"description\":\"%s\"}",
            (i == 0) ? "" : ",",
            preset->name,
            preset->x_milli,
            preset->y_milli,
            preset->w_milli,
            preset->h_milli,
            preset->description);
        if (wrote < 0 || (size_t) wrote >= out_size - used)
            return 0;
        used += (size_t) wrote;
    }

    wrote = snprintf(out + used, out_size - used, "]}");
    if (wrote < 0 || (size_t) wrote >= out_size - used)
        return 0;

    return 1;
}

#ifndef WIN32
static void AgentSetFd(int *fd_slot, int value)
{
    if (l_AgentMutex != NULL)
        SDL_LockMutex(l_AgentMutex);
    *fd_slot = value;
    if (l_AgentMutex != NULL)
        SDL_UnlockMutex(l_AgentMutex);
}

static int AgentTakeFd(int *fd_slot)
{
    int fd;

    if (l_AgentMutex != NULL)
        SDL_LockMutex(l_AgentMutex);
    fd = *fd_slot;
    *fd_slot = -1;
    if (l_AgentMutex != NULL)
        SDL_UnlockMutex(l_AgentMutex);

    return fd;
}

static int AgentGetFd(int *fd_slot)
{
    int fd;

    if (l_AgentMutex != NULL)
        SDL_LockMutex(l_AgentMutex);
    fd = *fd_slot;
    if (l_AgentMutex != NULL)
        SDL_UnlockMutex(l_AgentMutex);

    return fd;
}

static int AgentSendAll(int fd, const char *buf, size_t len)
{
    while (len > 0)
    {
        ssize_t sent = send(fd, buf, len, 0);
        if (sent < 0)
        {
            if (errno == EINTR)
                continue;
            return 0;
        }
        buf += sent;
        len -= (size_t) sent;
    }

    return 1;
}

static void AgentSendResponse(int fd, int id, int ok, const char *result_json, const char *error_text)
{
    char response[2048];

    if (ok)
    {
        if (result_json != NULL)
            snprintf(response, sizeof(response), "{\"id\":%d,\"ok\":true,\"result\":%s}\n", id, result_json);
        else
            snprintf(response, sizeof(response), "{\"id\":%d,\"ok\":true}\n", id);
    }
    else
    {
        if (error_text == NULL)
            error_text = "command failed";
        snprintf(response, sizeof(response), "{\"id\":%d,\"ok\":false,\"error\":\"%s\"}\n", id, error_text);
    }

    AgentSendAll(fd, response, strlen(response));
}

static int AgentReadLine(int fd, char *line, size_t line_size)
{
    size_t used = 0;

    if (line_size == 0)
        return -1;

    for (;;)
    {
        char c = '\0';
        ssize_t received = recv(fd, &c, 1, 0);

        if (received == 0)
            return 0;
        if (received < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (c == '\r')
            continue;
        if (c == '\n')
        {
            line[used] = '\0';
            return 1;
        }

        if (used + 1 < line_size)
            line[used++] = c;
    }
}

static int AgentWaitForFrameAdvance(int initial_frame, int timeout_ms)
{
    Uint32 start = SDL_GetTicks();

    while ((int) (SDL_GetTicks() - start) < timeout_ms)
    {
        if (SDL_AtomicGet(&l_LastFrame) > initial_frame)
            return 1;
        if (l_AgentServerStop)
            return 0;
        SDL_Delay(1);
    }

    return 0;
}

static int AgentWaitForEventSeqWithPausedStepping(SDL_atomic_t* seq, int previous, int timeout_ms)
{
    Uint32 start = SDL_GetTicks();

    while ((int) (SDL_GetTicks() - start) < timeout_ms)
    {
        int emu_state = M64EMU_STOPPED;
        int frame_before = 0;
        int remaining_ms = timeout_ms - (int) (SDL_GetTicks() - start);

        if (SDL_AtomicGet(seq) != previous)
            return 1;
        if (l_AgentServerStop)
            return 0;

        if ((*CoreDoCommand)(M64CMD_CORE_STATE_QUERY, M64CORE_EMU_STATE, &emu_state) != M64ERR_SUCCESS)
            return 0;
        if (emu_state != M64EMU_PAUSED)
        {
            SDL_Delay(1);
            continue;
        }

        frame_before = SDL_AtomicGet(&l_LastFrame);
        if ((*CoreDoCommand)(M64CMD_ADVANCE_FRAME, 0, NULL) != M64ERR_SUCCESS)
            return 0;

        if (remaining_ms <= 0)
            remaining_ms = 1;
        if (!AgentWaitForFrameAdvance(frame_before, remaining_ms))
            return 0;
    }

    return 0;
}

static void AgentBuildStatusResult(char *out, size_t out_size)
{
    int emu_state = -1;
    int speed_factor = 0;
    int speed_limiter = 0;
    int state_slot = 0;
    int video_size = 0;
    int frame = SDL_AtomicGet(&l_LastFrame);
    int width = 0;
    int height = 0;

    (*CoreDoCommand)(M64CMD_CORE_STATE_QUERY, M64CORE_EMU_STATE, &emu_state);
    (*CoreDoCommand)(M64CMD_CORE_STATE_QUERY, M64CORE_SPEED_FACTOR, &speed_factor);
    (*CoreDoCommand)(M64CMD_CORE_STATE_QUERY, M64CORE_SPEED_LIMITER, &speed_limiter);
    (*CoreDoCommand)(M64CMD_CORE_STATE_QUERY, M64CORE_SAVESTATE_SLOT, &state_slot);
    (*CoreDoCommand)(M64CMD_CORE_STATE_QUERY, M64CORE_VIDEO_SIZE, &video_size);

    width = (video_size >> 16) & 0xffff;
    height = video_size & 0xffff;

    snprintf(out, out_size,
        "{\"emu_state\":%d,\"frame\":%d,\"speed_factor\":%d,"
        "\"speed_limiter\":%d,\"state_slot\":%d,\"video_width\":%d,\"video_height\":%d,"
        "\"input_shadow\":[%u,%u,%u,%u],"
        "\"state_load_last\":%d,\"state_save_last\":%d,\"screenshot_last\":%d}",
        emu_state, frame, speed_factor, speed_limiter, state_slot, width, height,
        l_AgentInputShadow[0], l_AgentInputShadow[1], l_AgentInputShadow[2], l_AgentInputShadow[3],
        SDL_AtomicGet(&l_StateLoadLastResult),
        SDL_AtomicGet(&l_StateSaveLastResult),
        SDL_AtomicGet(&l_ScreenshotLastResult));
}

static int AgentHandleCommand(int fd, const char *line)
{
    char cmd[64];
    char path[1024];
    char result[4096];
    int id = 0;
    int cmd_result = M64ERR_SUCCESS;

    (void) AgentGetInt(line, "id", &id);

    if (!AgentGetString(line, "cmd", cmd, sizeof(cmd)))
    {
        AgentSendResponse(fd, id, 0, NULL, "missing cmd");
        return 0;
    }

    if (strcmp(cmd, "status") == 0)
    {
        AgentBuildStatusResult(result, sizeof(result));
        AgentSendResponse(fd, id, 1, result, NULL);
        return 0;
    }
    if (strcmp(cmd, "framebuffer_presets") == 0)
    {
        if (!AgentBuildFramebufferPresetListResult(result, sizeof(result)))
        {
            AgentSendResponse(fd, id, 0, NULL, "failed to build presets result");
            return 0;
        }
        AgentSendResponse(fd, id, 1, result, NULL);
        return 0;
    }
    if (strcmp(cmd, "pause") == 0)
    {
        cmd_result = (*CoreDoCommand)(M64CMD_PAUSE, 0, NULL);
    }
    else if (strcmp(cmd, "resume") == 0)
    {
        cmd_result = (*CoreDoCommand)(M64CMD_RESUME, 0, NULL);
    }
    else if (strcmp(cmd, "stop") == 0)
    {
        cmd_result = (*CoreDoCommand)(M64CMD_STOP, 0, NULL);
    }
    else if (strcmp(cmd, "step_frames") == 0)
    {
        int count = 1;
        int i;
        int frame_before = 0;
        int emu_state = M64EMU_STOPPED;

        (void) AgentGetInt(line, "count", &count);
        if (count < 1)
            count = 1;
        if (count > 10000)
            count = 10000;
        if ((*CoreDoCommand)(M64CMD_CORE_STATE_QUERY, M64CORE_EMU_STATE, &emu_state) != M64ERR_SUCCESS)
        {
            AgentSendResponse(fd, id, 0, NULL, "failed to query emulation state");
            return 0;
        }
        if (emu_state != M64EMU_PAUSED)
        {
            AgentSendResponse(fd, id, 0, NULL, "step_frames requires paused state; call pause first");
            return 0;
        }

        for (i = 0; i < count; ++i)
        {
            frame_before = SDL_AtomicGet(&l_LastFrame);
                cmd_result = (*CoreDoCommand)(M64CMD_ADVANCE_FRAME, 0, NULL);
                if (cmd_result != M64ERR_SUCCESS || !AgentWaitForFrameAdvance(frame_before, 5000))
                {
                    if (cmd_result == M64ERR_SUCCESS)
                        cmd_result = M64ERR_SYSTEM_FAIL;
                    break;
                }
        }

        if (cmd_result == M64ERR_SUCCESS)
        {
            snprintf(result, sizeof(result),
                "{\"advanced\":%d,\"frame\":%d}", count, SDL_AtomicGet(&l_LastFrame));
            AgentSendResponse(fd, id, 1, result, NULL);
            return 0;
        }
    }
    else if (strcmp(cmd, "set_speed_limiter") == 0)
    {
        int enabled = 1;
        (void) AgentGetBool(line, "enabled", &enabled);
        enabled = (enabled != 0);
        cmd_result = (*CoreDoCommand)(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &enabled);
    }
    else if (strcmp(cmd, "input_set") == 0)
    {
        m64p_controller_input_state state;
        if (!AgentParseControlPort(line, "port", &state.controller))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing or invalid port (use 1-4)");
            return 0;
        }
        if (!AgentGetUInt32(line, "input", &state.input_state))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing input value");
            return 0;
        }
        l_AgentInputShadow[state.controller] = state.input_state;
        cmd_result = (*CoreDoCommand)(M64CMD_INPUT_SET_STATE, 0, &state);
    }
    else if (strcmp(cmd, "input_queue") == 0)
    {
        m64p_controller_input_queued_state state;
        if (!AgentParseControlPort(line, "port", &state.controller))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing or invalid port (use 1-4)");
            return 0;
        }
        if (!AgentGetUInt32(line, "input", &state.input_state))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing input value");
            return 0;
        }
        if (!AgentGetUInt32(line, "start_frame", &state.frame_start))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing start_frame");
            return 0;
        }
        if (!AgentGetUInt32(line, "end_frame", &state.frame_end))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing end_frame");
            return 0;
        }
        cmd_result = (*CoreDoCommand)(M64CMD_INPUT_QUEUE_STATE, 0, &state);
    }
    else if (strcmp(cmd, "input_press") == 0 || strcmp(cmd, "input_release") == 0)
    {
        m64p_controller_input_state state;
        char button_name[64];
        uint32_t mask = 0;
        int is_press = (strcmp(cmd, "input_press") == 0);

        if (!AgentParseControlPort(line, "port", &state.controller))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing or invalid port (use 1-4)");
            return 0;
        }
        if (!AgentGetString(line, "button", button_name, sizeof(button_name)))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing button");
            return 0;
        }
        mask = AgentButtonMaskFromName(button_name);
        if (mask == 0)
        {
            AgentSendResponse(fd, id, 0, NULL, "unknown button");
            return 0;
        }

        if (is_press)
            l_AgentInputShadow[state.controller] |= mask;
        else
            l_AgentInputShadow[state.controller] &= ~mask;

        state.input_state = l_AgentInputShadow[state.controller];
        cmd_result = (*CoreDoCommand)(M64CMD_INPUT_SET_STATE, 0, &state);
    }
    else if (strcmp(cmd, "input_stick") == 0)
    {
        m64p_controller_input_state state;
        int x = 0;
        int y = 0;

        if (!AgentParseControlPort(line, "port", &state.controller))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing or invalid port (use 1-4)");
            return 0;
        }
        if (!AgentGetInt(line, "x", &x) || !AgentGetInt(line, "y", &y))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing x/y");
            return 0;
        }
        if (x < -128) x = -128;
        if (x > 127) x = 127;
        if (y < -128) y = -128;
        if (y > 127) y = 127;

        l_AgentInputShadow[state.controller] = AgentBuildStateWithStick(l_AgentInputShadow[state.controller], x, y);
        state.input_state = l_AgentInputShadow[state.controller];
        cmd_result = (*CoreDoCommand)(M64CMD_INPUT_SET_STATE, 0, &state);
    }
    else if (strcmp(cmd, "input_tap") == 0 || strcmp(cmd, "input_hold") == 0)
    {
        m64p_controller_input_queued_state hold_state;
        m64p_controller_input_queued_state release_state;
        char button_name[64];
        uint32_t mask = 0;
        uint32_t frame_now = (uint32_t) SDL_AtomicGet(&l_LastFrame);
        int frames = 1;

        if (!AgentParseControlPort(line, "port", &hold_state.controller))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing or invalid port (use 1-4)");
            return 0;
        }
        if (!AgentGetString(line, "button", button_name, sizeof(button_name)))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing button");
            return 0;
        }
        mask = AgentButtonMaskFromName(button_name);
        if (mask == 0)
        {
            AgentSendResponse(fd, id, 0, NULL, "unknown button");
            return 0;
        }
        (void) AgentGetInt(line, "frames", &frames);
        if (frames < 1)
            frames = 1;

        hold_state.input_state = l_AgentInputShadow[hold_state.controller] | mask;
        hold_state.frame_start = frame_now + 1;
        hold_state.frame_end = frame_now + (uint32_t) frames;

        release_state.controller = hold_state.controller;
        release_state.input_state = l_AgentInputShadow[hold_state.controller] & ~mask;
        release_state.frame_start = hold_state.frame_end + 1;
        release_state.frame_end = hold_state.frame_end + 1;

        cmd_result = (*CoreDoCommand)(M64CMD_INPUT_QUEUE_STATE, 0, &hold_state);
        if (cmd_result == M64ERR_SUCCESS)
            cmd_result = (*CoreDoCommand)(M64CMD_INPUT_QUEUE_STATE, 0, &release_state);
    }
    else if (strcmp(cmd, "input_get") == 0)
    {
        unsigned int control_id = 0;
        if (!AgentParseControlPort(line, "port", &control_id))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing or invalid port (use 1-4)");
            return 0;
        }
        snprintf(result, sizeof(result), "{\"port\":%u,\"input\":%u}", control_id + 1, l_AgentInputShadow[control_id]);
        AgentSendResponse(fd, id, 1, result, NULL);
        return 0;
    }
    else if (strcmp(cmd, "input_clear") == 0)
    {
        int control_id = -1;
        int port = 0;
        if (AgentGetInt(line, "port", &port))
        {
            if (port >= 1 && port <= 4)
                control_id = port - 1;
            else if (port >= 0 && port <= 3)
                control_id = port;
            else
            {
                AgentSendResponse(fd, id, 0, NULL, "invalid port (use 1-4)");
                return 0;
            }
        }
        if (control_id < 0)
            memset(l_AgentInputShadow, 0, sizeof(l_AgentInputShadow));
        else
            l_AgentInputShadow[control_id] = 0;
        cmd_result = (*CoreDoCommand)(M64CMD_INPUT_CLEAR, control_id, NULL);
    }
    else if (strcmp(cmd, "set_speed_factor") == 0)
    {
        int value = 100;
        if (!AgentGetInt(line, "value", &value))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing speed factor value");
            return 0;
        }
        cmd_result = (*CoreDoCommand)(M64CMD_CORE_STATE_SET, M64CORE_SPEED_FACTOR, &value);
    }
    else if (strcmp(cmd, "set_state_slot") == 0)
    {
        int slot = 0;
        if (!AgentGetInt(line, "slot", &slot))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing slot");
            return 0;
        }
        cmd_result = (*CoreDoCommand)(M64CMD_STATE_SET_SLOT, slot, NULL);
    }
    else if (strcmp(cmd, "save_state") == 0)
    {
        int save_seq = SDL_AtomicGet(&l_StateSaveEventSeq);
        if (AgentGetString(line, "path", path, sizeof(path)))
        {
            int format = 2;
            (void) AgentGetInt(line, "format", &format);
            cmd_result = (*CoreDoCommand)(M64CMD_STATE_SAVE, format, path);
        }
        else
        {
            cmd_result = (*CoreDoCommand)(M64CMD_STATE_SAVE, 0, NULL);
        }

        if (cmd_result == M64ERR_SUCCESS)
        {
            if (!AgentWaitForEventSeqWithPausedStepping(&l_StateSaveEventSeq, save_seq, 5000))
            {
                AgentSendResponse(fd, id, 0, NULL, "save_state timed out");
                return 0;
            }
            if (SDL_AtomicGet(&l_StateSaveLastResult) == 0)
            {
                AgentSendResponse(fd, id, 0, NULL, "save_state failed");
                return 0;
            }
        }
    }
    else if (strcmp(cmd, "load_state") == 0)
    {
        int load_seq = SDL_AtomicGet(&l_StateLoadEventSeq);
        if (AgentGetString(line, "path", path, sizeof(path)))
            cmd_result = (*CoreDoCommand)(M64CMD_STATE_LOAD, 0, path);
        else
            cmd_result = (*CoreDoCommand)(M64CMD_STATE_LOAD, 0, NULL);

        if (cmd_result == M64ERR_SUCCESS)
        {
            if (!AgentWaitForEventSeqWithPausedStepping(&l_StateLoadEventSeq, load_seq, 5000))
            {
                AgentSendResponse(fd, id, 0, NULL, "load_state timed out");
                return 0;
            }
            if (SDL_AtomicGet(&l_StateLoadLastResult) == 0)
            {
                AgentSendResponse(fd, id, 0, NULL, "load_state failed");
                return 0;
            }
        }
    }
    else if (strcmp(cmd, "screenshot") == 0)
    {
        int shot_seq = SDL_AtomicGet(&l_ScreenshotEventSeq);
        cmd_result = (*CoreDoCommand)(M64CMD_TAKE_NEXT_SCREENSHOT, 0, NULL);
        if (cmd_result == M64ERR_SUCCESS)
        {
            if (!AgentWaitForEventSeqWithPausedStepping(&l_ScreenshotEventSeq, shot_seq, 5000))
            {
                AgentSendResponse(fd, id, 0, NULL, "screenshot timed out");
                return 0;
            }
            if (SDL_AtomicGet(&l_ScreenshotLastResult) == 0)
            {
                AgentSendResponse(fd, id, 0, NULL, "screenshot failed");
                return 0;
            }
        }
    }
    else if (strcmp(cmd, "framebuffer_dump") == 0 || strcmp(cmd, "framebuffer_dump_preset") == 0)
    {
        char output_path[1024];
        char preset_name[64];
        const struct agent_framebuffer_preset* preset = NULL;
        int use_preset = 0;
        int front = 0;
        int video_size = 0;
        int width = 0;
        int height = 0;
        int crop_x = 0;
        int crop_y = 0;
        int crop_w = 0;
        int crop_h = 0;
        int scale_div = 1;
        int out_w = 0;
        int out_h = 0;
        unsigned char* rgb = NULL;
        unsigned char* out_rgb = NULL;
        FILE* f = NULL;
        int y = 0;

        if (!AgentGetString(line, "path", output_path, sizeof(output_path)))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing path");
            return 0;
        }
        (void) AgentGetBool(line, "front", &front);
        if (strcmp(cmd, "framebuffer_dump_preset") == 0)
        {
            if (!AgentGetString(line, "preset", preset_name, sizeof(preset_name)))
            {
                AgentSendResponse(fd, id, 0, NULL, "missing preset");
                return 0;
            }
            preset = AgentFindFramebufferPreset(preset_name);
            if (preset == NULL)
            {
                AgentSendResponse(fd, id, 0, NULL, "unknown preset");
                return 0;
            }
            use_preset = 1;
        }
        if ((*CoreDoCommand)(M64CMD_CORE_STATE_QUERY, M64CORE_VIDEO_SIZE, &video_size) != M64ERR_SUCCESS)
        {
            AgentSendResponse(fd, id, 0, NULL, "failed to query video size");
            return 0;
        }

        width = (video_size >> 16) & 0xffff;
        height = video_size & 0xffff;
        if (width <= 0 || height <= 0)
        {
            AgentSendResponse(fd, id, 0, NULL, "invalid video size");
            return 0;
        }

        crop_w = width;
        crop_h = height;
        if (use_preset)
        {
            AgentApplyFramebufferPresetCrop(preset, width, height, &crop_x, &crop_y, &crop_w, &crop_h);
        }
        else
        {
            (void) AgentGetInt(line, "crop_x", &crop_x);
            (void) AgentGetInt(line, "crop_y", &crop_y);
            (void) AgentGetInt(line, "crop_w", &crop_w);
            (void) AgentGetInt(line, "crop_h", &crop_h);
        }
        (void) AgentGetInt(line, "scale_div", &scale_div);

        if (crop_x < 0) crop_x = 0;
        if (crop_y < 0) crop_y = 0;
        if (crop_w <= 0) crop_w = width - crop_x;
        if (crop_h <= 0) crop_h = height - crop_y;
        if (crop_x + crop_w > width) crop_w = width - crop_x;
        if (crop_y + crop_h > height) crop_h = height - crop_y;
        if (scale_div < 1) scale_div = 1;

        out_w = crop_w / scale_div;
        out_h = crop_h / scale_div;
        if (out_w < 1) out_w = 1;
        if (out_h < 1) out_h = 1;

        rgb = (unsigned char*) malloc((size_t) width * (size_t) height * 3U);
        out_rgb = (unsigned char*) malloc((size_t) out_w * (size_t) out_h * 3U);
        if (rgb == NULL || out_rgb == NULL)
        {
            free(rgb);
            free(out_rgb);
            AgentSendResponse(fd, id, 0, NULL, "out of memory");
            return 0;
        }

        cmd_result = (*CoreDoCommand)(M64CMD_READ_SCREEN, front ? 1 : 0, rgb);
        if (cmd_result != M64ERR_SUCCESS)
        {
            free(rgb);
            free(out_rgb);
            AgentSendResponse(fd, id, 0, NULL, "read_screen failed");
            return 0;
        }

        for (y = 0; y < out_h; ++y)
        {
            int x;
            int src_y = crop_y + y * scale_div;
            if (src_y >= height) src_y = height - 1;
            for (x = 0; x < out_w; ++x)
            {
                int src_x = crop_x + x * scale_div;
                size_t src_idx;
                size_t dst_idx;
                if (src_x >= width) src_x = width - 1;
                src_idx = ((size_t) src_y * (size_t) width + (size_t) src_x) * 3U;
                dst_idx = ((size_t) y * (size_t) out_w + (size_t) x) * 3U;
                out_rgb[dst_idx + 0] = rgb[src_idx + 0];
                out_rgb[dst_idx + 1] = rgb[src_idx + 1];
                out_rgb[dst_idx + 2] = rgb[src_idx + 2];
            }
        }

        f = fopen(output_path, "wb");
        if (f == NULL)
        {
            free(rgb);
            free(out_rgb);
            AgentSendResponse(fd, id, 0, NULL, "failed to open output path");
            return 0;
        }
        fprintf(f, "P6\n%d %d\n255\n", out_w, out_h);
        if (fwrite(out_rgb, 1, (size_t) out_w * (size_t) out_h * 3U, f) != (size_t) out_w * (size_t) out_h * 3U)
        {
            fclose(f);
            free(rgb);
            free(out_rgb);
            AgentSendResponse(fd, id, 0, NULL, "failed to write framebuffer");
            return 0;
        }
        fclose(f);
        free(rgb);
        free(out_rgb);

        if (use_preset)
        {
            snprintf(result, sizeof(result),
                "{\"path\":\"%s\",\"preset\":\"%s\",\"source_width\":%d,\"source_height\":%d,"
                "\"crop_x\":%d,\"crop_y\":%d,\"crop_w\":%d,\"crop_h\":%d,"
                "\"width\":%d,\"height\":%d,\"scale_div\":%d}",
                output_path, preset->name, width, height,
                crop_x, crop_y, crop_w, crop_h,
                out_w, out_h, scale_div);
        }
        else
        {
            snprintf(result, sizeof(result),
                "{\"path\":\"%s\",\"source_width\":%d,\"source_height\":%d,"
                "\"crop_x\":%d,\"crop_y\":%d,\"crop_w\":%d,\"crop_h\":%d,"
                "\"width\":%d,\"height\":%d,\"scale_div\":%d}",
                output_path, width, height,
                crop_x, crop_y, crop_w, crop_h,
                out_w, out_h, scale_div);
        }
        AgentSendResponse(fd, id, 1, result, NULL);
        return 0;
    }
    else if (strcmp(cmd, "depth_dump") == 0)
    {
        char output_path[1024];
        int front = 0;
        int rotate180 = 0;
        int video_size = 0;
        int width = 0;
        int height = 0;
        int crop_x = 0;
        int crop_y = 0;
        int crop_w = 0;
        int crop_h = 0;
        int scale_div = 1;
        int out_w = 0;
        int out_h = 0;
        uint16_t* depth = NULL;
        unsigned char* out_depth = NULL;
        FILE* f = NULL;
        int y = 0;

        if (!AgentGetString(line, "path", output_path, sizeof(output_path)))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing path");
            return 0;
        }
        (void) AgentGetBool(line, "front", &front);
        (void) AgentGetBool(line, "rotate180", &rotate180);
        if ((*CoreDoCommand)(M64CMD_CORE_STATE_QUERY, M64CORE_VIDEO_SIZE, &video_size) != M64ERR_SUCCESS)
        {
            AgentSendResponse(fd, id, 0, NULL, "failed to query video size");
            return 0;
        }

        width = (video_size >> 16) & 0xffff;
        height = video_size & 0xffff;
        if (width <= 0 || height <= 0)
        {
            AgentSendResponse(fd, id, 0, NULL, "invalid video size");
            return 0;
        }

        crop_w = width;
        crop_h = height;
        (void) AgentGetInt(line, "crop_x", &crop_x);
        (void) AgentGetInt(line, "crop_y", &crop_y);
        (void) AgentGetInt(line, "crop_w", &crop_w);
        (void) AgentGetInt(line, "crop_h", &crop_h);
        (void) AgentGetInt(line, "scale_div", &scale_div);

        if (crop_x < 0) crop_x = 0;
        if (crop_y < 0) crop_y = 0;
        if (crop_w <= 0) crop_w = width - crop_x;
        if (crop_h <= 0) crop_h = height - crop_y;
        if (crop_x + crop_w > width) crop_w = width - crop_x;
        if (crop_y + crop_h > height) crop_h = height - crop_y;
        if (scale_div < 1) scale_div = 1;

        out_w = crop_w / scale_div;
        out_h = crop_h / scale_div;
        if (out_w < 1) out_w = 1;
        if (out_h < 1) out_h = 1;

        depth = (uint16_t*) malloc((size_t) width * (size_t) height * sizeof(uint16_t));
        out_depth = (unsigned char*) malloc((size_t) out_w * (size_t) out_h * 2U);
        if (depth == NULL || out_depth == NULL)
        {
            free(depth);
            free(out_depth);
            AgentSendResponse(fd, id, 0, NULL, "out of memory");
            return 0;
        }

        cmd_result = (*CoreDoCommand)(M64CMD_READ_SCREEN_DEPTH, front ? 1 : 0, depth);
        if (cmd_result != M64ERR_SUCCESS)
        {
            free(depth);
            free(out_depth);
            if (cmd_result == M64ERR_UNSUPPORTED)
                AgentSendResponse(fd, id, 0, NULL, "depth read is not supported by this video plugin");
            else
                AgentSendResponse(fd, id, 0, NULL, "read_screen_depth failed");
            return 0;
        }

        for (y = 0; y < out_h; ++y)
        {
            int x;
            int src_y = crop_y + y * scale_div;
            if (src_y >= height) src_y = height - 1;
            for (x = 0; x < out_w; ++x)
            {
                int src_x = crop_x + x * scale_div;
                size_t src_idx;
                size_t dst_idx;
                uint16_t z;
                int dst_x = x;
                int dst_y = y;
                if (src_x >= width) src_x = width - 1;
                if (rotate180)
                {
                    dst_x = out_w - 1 - x;
                    dst_y = out_h - 1 - y;
                }
                src_idx = (size_t) src_y * (size_t) width + (size_t) src_x;
                dst_idx = ((size_t) dst_y * (size_t) out_w + (size_t) dst_x) * 2U;
                z = depth[src_idx];
                out_depth[dst_idx + 0] = (unsigned char) (z & 0xff);
                out_depth[dst_idx + 1] = (unsigned char) ((z >> 8) & 0xff);
            }
        }

        f = fopen(output_path, "wb");
        if (f == NULL)
        {
            free(depth);
            free(out_depth);
            AgentSendResponse(fd, id, 0, NULL, "failed to open output path");
            return 0;
        }
        if (fwrite(out_depth, 1, (size_t) out_w * (size_t) out_h * 2U, f) != (size_t) out_w * (size_t) out_h * 2U)
        {
            fclose(f);
            free(depth);
            free(out_depth);
            AgentSendResponse(fd, id, 0, NULL, "failed to write depth buffer");
            return 0;
        }
        fclose(f);
        free(depth);
        free(out_depth);

        snprintf(result, sizeof(result),
            "{\"path\":\"%s\",\"format\":\"u16le\",\"source_width\":%d,\"source_height\":%d,"
            "\"crop_x\":%d,\"crop_y\":%d,\"crop_w\":%d,\"crop_h\":%d,"
            "\"width\":%d,\"height\":%d,\"scale_div\":%d,\"rotate180\":%d}",
            output_path, width, height,
            crop_x, crop_y, crop_w, crop_h,
            out_w, out_h, scale_div, rotate180 ? 1 : 0);
        AgentSendResponse(fd, id, 1, result, NULL);
        return 0;
    }
    else if (strcmp(cmd, "mem_read") == 0)
    {
        uint32_t addr = 0;
        int bits = 32;
        int emu_state = M64EMU_STOPPED;
        unsigned long long value = 0;

        if (!(g_CoreCapabilities & M64CAPS_DEBUGGER))
        {
            AgentSendResponse(fd, id, 0, NULL, "debugger capability is required for mem_read");
            return 0;
        }
        if (!AgentGetUInt32(line, "addr", &addr))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing addr");
            return 0;
        }
        (void) AgentGetInt(line, "bits", &bits);
        if ((*CoreDoCommand)(M64CMD_CORE_STATE_QUERY, M64CORE_EMU_STATE, &emu_state) != M64ERR_SUCCESS || emu_state != M64EMU_PAUSED)
        {
            AgentSendResponse(fd, id, 0, NULL, "mem_read requires paused state");
            return 0;
        }

        if (bits == 8 && DebugMemRead8 != NULL) value = (unsigned long long) (uint8_t) DebugMemRead8(addr);
        else if (bits == 16 && DebugMemRead16 != NULL) value = (unsigned long long) (uint16_t) DebugMemRead16(addr);
        else if (bits == 32 && DebugMemRead32 != NULL) value = (unsigned long long) (uint32_t) DebugMemRead32(addr);
        else if (bits == 64 && DebugMemRead64 != NULL) value = (unsigned long long) (uint64_t) DebugMemRead64(addr);
        else
        {
            AgentSendResponse(fd, id, 0, NULL, "unsupported bits value (use 8,16,32,64)");
            return 0;
        }

        snprintf(result, sizeof(result), "{\"addr\":%u,\"bits\":%d,\"value\":%llu}", addr, bits, value);
        AgentSendResponse(fd, id, 1, result, NULL);
        return 0;
    }
    else if (strcmp(cmd, "mem_write") == 0)
    {
        uint32_t addr = 0;
        uint32_t value32 = 0;
        unsigned long long value64 = 0;
        int bits = 32;
        int emu_state = M64EMU_STOPPED;

        if (!(g_CoreCapabilities & M64CAPS_DEBUGGER))
        {
            AgentSendResponse(fd, id, 0, NULL, "debugger capability is required for mem_write");
            return 0;
        }
        if (!AgentGetUInt32(line, "addr", &addr))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing addr");
            return 0;
        }
        if (!AgentGetUInt32(line, "value", &value32))
        {
            AgentSendResponse(fd, id, 0, NULL, "missing value");
            return 0;
        }
        value64 = (unsigned long long) value32;
        (void) AgentGetInt(line, "bits", &bits);
        if ((*CoreDoCommand)(M64CMD_CORE_STATE_QUERY, M64CORE_EMU_STATE, &emu_state) != M64ERR_SUCCESS || emu_state != M64EMU_PAUSED)
        {
            AgentSendResponse(fd, id, 0, NULL, "mem_write requires paused state");
            return 0;
        }

        if (bits == 8 && DebugMemWrite8 != NULL) DebugMemWrite8(addr, (unsigned char) value32);
        else if (bits == 16 && DebugMemWrite16 != NULL) DebugMemWrite16(addr, (unsigned short) value32);
        else if (bits == 32 && DebugMemWrite32 != NULL) DebugMemWrite32(addr, value32);
        else if (bits == 64 && DebugMemWrite64 != NULL) DebugMemWrite64(addr, value64);
        else
        {
            AgentSendResponse(fd, id, 0, NULL, "unsupported bits value (use 8,16,32,64)");
            return 0;
        }

        AgentSendResponse(fd, id, 1, NULL, NULL);
        return 0;
    }
    else if (strcmp(cmd, "shutdown") == 0)
    {
        cmd_result = (*CoreDoCommand)(M64CMD_STOP, 0, NULL);
        l_AgentServerStop = 1;
        if (cmd_result == M64ERR_SUCCESS)
        {
            AgentSendResponse(fd, id, 1, NULL, NULL);
            return 1;
        }
    }
    else
    {
        AgentSendResponse(fd, id, 0, NULL, "unknown command");
        return 0;
    }

    if (cmd_result == M64ERR_SUCCESS)
    {
        AgentSendResponse(fd, id, 1, NULL, NULL);
    }
    else
    {
        snprintf(result, sizeof(result), "core command failed (%d)", cmd_result);
        AgentSendResponse(fd, id, 0, NULL, result);
    }

    return 0;
}

static int AgentInitUnixSocketPath(const char *endpoint)
{
    const char *path = endpoint;
    size_t path_len;

    l_AgentSocketPath[0] = '\0';

    if (path == NULL || *path == '\0')
        return 0;

    if (strncmp(path, "unix:", 5) == 0)
        path += 5;

    if (strncmp(path, "tcp:", 4) == 0)
    {
        DebugMessage(M64MSG_ERROR, "tcp: endpoints are not implemented yet in --agent-server");
        return 0;
    }

    path_len = strlen(path);
    if (path_len == 0 || path_len >= sizeof(l_AgentSocketPath))
    {
        DebugMessage(M64MSG_ERROR, "invalid unix socket path for --agent-server");
        return 0;
    }

    strcpy(l_AgentSocketPath, path);
    return 1;
}

static int AgentServerLoop(void *arg)
{
    struct sockaddr_un addr;
    int listen_fd;

    (void) arg;

    if (!AgentInitUnixSocketPath(l_AgentServerEndpoint))
        return 1;

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        DebugMessage(M64MSG_ERROR, "failed to create agent socket: %s", strerror(errno));
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, l_AgentSocketPath, sizeof(addr.sun_path) - 1);

    unlink(l_AgentSocketPath);

    if (bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)) != 0)
    {
        DebugMessage(M64MSG_ERROR, "failed to bind agent socket '%s': %s",
            l_AgentSocketPath, strerror(errno));
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 1) != 0)
    {
        DebugMessage(M64MSG_ERROR, "failed to listen on agent socket '%s': %s",
            l_AgentSocketPath, strerror(errno));
        close(listen_fd);
        unlink(l_AgentSocketPath);
        return 1;
    }

    AgentSetFd(&l_AgentListenFd, listen_fd);
    DebugMessage(M64MSG_INFO, "agent server listening on %s", l_AgentSocketPath);

    while (!l_AgentServerStop)
    {
        char line[4096];
        int client_fd = accept(AgentGetFd(&l_AgentListenFd), NULL, NULL);
        if (client_fd < 0)
        {
            if (errno == EINTR)
                continue;
            if (!l_AgentServerStop)
                SDL_Delay(10);
            continue;
        }

        AgentSetFd(&l_AgentClientFd, client_fd);
        DebugMessage(M64MSG_INFO, "agent client connected");

        while (!l_AgentServerStop)
        {
            int read_result = AgentReadLine(client_fd, line, sizeof(line));
            if (read_result <= 0)
                break;
            if (line[0] == '\0')
                continue;
            if (AgentHandleCommand(client_fd, line))
                break;
        }

        client_fd = AgentTakeFd(&l_AgentClientFd);
        if (client_fd >= 0)
            close(client_fd);
        DebugMessage(M64MSG_INFO, "agent client disconnected");
    }

    listen_fd = AgentTakeFd(&l_AgentListenFd);
    if (listen_fd >= 0)
        close(listen_fd);
    unlink(l_AgentSocketPath);
    l_AgentSocketPath[0] = '\0';
    return 0;
}

static int StartAgentServer(void)
{
    if (l_AgentMutex == NULL)
    {
        l_AgentMutex = SDL_CreateMutex();
        if (l_AgentMutex == NULL)
        {
            DebugMessage(M64MSG_ERROR, "failed to create agent mutex: %s", SDL_GetError());
            return 0;
        }
    }

    l_AgentServerStop = 0;
    l_AgentServerThread = SDL_CreateThread(AgentServerLoop, "AgentServer", NULL);
    if (l_AgentServerThread == NULL)
    {
        DebugMessage(M64MSG_ERROR, "failed to create agent server thread: %s", SDL_GetError());
        return 0;
    }
    return 1;
}

static void StopAgentServer(void)
{
    int fd;

    if (l_AgentServerThread == NULL)
        return;

    l_AgentServerStop = 1;

    fd = AgentTakeFd(&l_AgentClientFd);
    if (fd >= 0)
        close(fd);

    fd = AgentTakeFd(&l_AgentListenFd);
    if (fd >= 0)
    {
        close(fd);
        if (l_AgentSocketPath[0] != '\0')
            unlink(l_AgentSocketPath);
    }

    SDL_WaitThread(l_AgentServerThread, NULL);
    l_AgentServerThread = NULL;

    if (l_AgentMutex != NULL)
    {
        SDL_DestroyMutex(l_AgentMutex);
        l_AgentMutex = NULL;
    }
}
#else
static int StartAgentServer(void)
{
    DebugMessage(M64MSG_ERROR, "--agent-server is not available on this platform");
    return 0;
}

static void StopAgentServer(void)
{
}
#endif

static int ParseCommandLineInitial(int argc, const char **argv)
{
    int i;

    /* First phase of command line parsing: read parameters that affect the
       core and the ui-console behavior. */
    for (i = 1; i < argc; i++)
    {
        int ArgsLeft = argc - i - 1;

        if (strcmp(argv[i], "--corelib") == 0 && ArgsLeft >= 1)
        {
            l_CoreLibPath = argv[i+1];
            i++;
        }
        else if (strcmp(argv[i], "--configdir") == 0 && ArgsLeft >= 1)
        {
            l_ConfigDirPath = argv[i+1];
            i++;
        }
        else if (strcmp(argv[i], "--datadir") == 0 && ArgsLeft >= 1)
        {
            l_DataDirPath = argv[i+1];
            i++;
        }
        else if (strcmp(argv[i], "--agent-server") == 0 && ArgsLeft >= 1)
        {
            l_AgentServerEndpoint = argv[i+1];
            l_AgentMode = 1;
            i++;
        }
        else if (strcmp(argv[i], "--agent-profile") == 0 && ArgsLeft >= 1)
        {
            if (osal_insensitive_strcmp(argv[i+1], "watch") == 0)
                l_AgentProfileMode = AGENT_PROFILE_WATCH;
            else if (osal_insensitive_strcmp(argv[i+1], "train") == 0)
                l_AgentProfileMode = AGENT_PROFILE_TRAIN;
            else
                DebugMessage(M64MSG_WARNING, "unknown --agent-profile value '%s'", argv[i+1]);
            i++;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            printUsage(argv[0]);
            return 1;
        }
        else if (strcmp(argv[i], "--nosaveoptions") == 0)
        {
            l_SaveOptions = 0;
        }
    }

    return 0;
}

static m64p_error ParseCommandLineMain(int argc, const char **argv)
{
    int i;

    /* Second phase of command-line parsing: read all remaining parameters
       except for those that set plugin options. */
    for (i = 1; i < argc; i++)
    {
        int ArgsLeft = argc - i - 1;
        if (strcmp(argv[i], "--noosd") == 0)
        {
            int Osd = 0;
            (*ConfigSetParameter)(l_ConfigCore, "OnScreenDisplay", M64TYPE_BOOL, &Osd);
        }
        else if (strcmp(argv[i], "--osd") == 0)
        {
            int Osd = 1;
            (*ConfigSetParameter)(l_ConfigCore, "OnScreenDisplay", M64TYPE_BOOL, &Osd);
        }
        else if (strcmp(argv[i], "--fullscreen") == 0)
        {
            int Fullscreen = 1;
            (*ConfigSetParameter)(l_ConfigVideo, "Fullscreen", M64TYPE_BOOL, &Fullscreen);
        }
        else if (strcmp(argv[i], "--windowed") == 0)
        {
            int Fullscreen = 0;
            (*ConfigSetParameter)(l_ConfigVideo, "Fullscreen", M64TYPE_BOOL, &Fullscreen);
        }
        else if (strcmp(argv[i], "--nospeedlimit") == 0)
        {
            int EnableSpeedLimit = 0;
            if (g_CoreAPIVersion < 0x020001)
                DebugMessage(M64MSG_WARNING, "core library doesn't support --nospeedlimit");
            else
            {
                if ((*CoreDoCommand)(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &EnableSpeedLimit) != M64ERR_SUCCESS)
                    DebugMessage(M64MSG_ERROR, "core gave error while setting --nospeedlimit option");
            }
        }
        else if ((strcmp(argv[i], "--corelib") == 0 || strcmp(argv[i], "--configdir") == 0 ||
                  strcmp(argv[i], "--datadir") == 0 || strcmp(argv[i], "--agent-server") == 0 ||
                  strcmp(argv[i], "--agent-profile") == 0) && ArgsLeft >= 1)
        {   /* already handled in ParseCommandLineInitial (skip the value) */
            i++;
        }
        else if (strcmp(argv[i], "--nosaveoptions") == 0)
        {   /* already handled in ParseCommandLineInitial (no value to skip) */
            ;
        }
        else if (strcmp(argv[i], "--resolution") == 0 && ArgsLeft >= 1)
        {
            const char *res = argv[i+1];
            int xres, yres;
            i++;
            if (sscanf(res, "%ix%i", &xres, &yres) != 2)
                DebugMessage(M64MSG_WARNING, "couldn't parse resolution '%s'", res);
            else
            {
                (*ConfigSetParameter)(l_ConfigVideo, "ScreenWidth", M64TYPE_INT, &xres);
                (*ConfigSetParameter)(l_ConfigVideo, "ScreenHeight", M64TYPE_INT, &yres);
            }
        }
        else if (strcmp(argv[i], "--cheats") == 0 && ArgsLeft >= 1)
        {
            if (strcmp(argv[i+1], "all") == 0)
                l_CheatMode = CHEAT_ALL;
            else if (strcmp(argv[i+1], "list") == 0)
                l_CheatMode = CHEAT_SHOW_LIST;
            else
            {
                l_CheatMode = CHEAT_LIST;
                l_CheatNumList = (char*) argv[i+1];
            }
            i++;
        }
        else if (strcmp(argv[i], "--plugindir") == 0 && ArgsLeft >= 1)
        {
            g_PluginDir = argv[i+1];
            i++;
        }
        else if (strcmp(argv[i], "--sshotdir") == 0 && ArgsLeft >= 1)
        {
            (*ConfigSetParameter)(l_ConfigCore, "ScreenshotPath", M64TYPE_STRING, argv[i+1]);
            i++;
        }
        else if (strcmp(argv[i], "--gfx") == 0 && ArgsLeft >= 1)
        {
            g_GfxPlugin = argv[i+1];
            i++;
        }
        else if (strcmp(argv[i], "--audio") == 0 && ArgsLeft >= 1)
        {
            g_AudioPlugin = argv[i+1];
            i++;
        }
        else if (strcmp(argv[i], "--input") == 0 && ArgsLeft >= 1)
        {
            g_InputPlugin = argv[i+1];
            i++;
        }
        else if (strcmp(argv[i], "--rsp") == 0 && ArgsLeft >= 1)
        {
            g_RspPlugin = argv[i+1];
            i++;
        }
        else if (strcmp(argv[i], "--emumode") == 0 && ArgsLeft >= 1)
        {
            int emumode = atoi(argv[i+1]);
            i++;
            if (emumode < 0 || emumode > 2)
            {
                DebugMessage(M64MSG_WARNING, "invalid --emumode value '%i'", emumode);
                continue;
            }
            if (emumode == 2 && !(g_CoreCapabilities & M64CAPS_DYNAREC))
            {
                DebugMessage(M64MSG_WARNING, "Emulator core doesn't support Dynamic Recompiler.");
                emumode = 1;
            }
            (*ConfigSetParameter)(l_ConfigCore, "R4300Emulator", M64TYPE_INT, &emumode);
        }
        else if (strcmp(argv[i], "--savestate") == 0 && ArgsLeft >= 1)
        {
            l_SaveStatePath = argv[i+1];
            i++;
        }
        else if (strcmp(argv[i], "--testshots") == 0 && ArgsLeft >= 1)
        {
            l_TestShotList = ParseNumberList(argv[i+1], NULL);
            i++;
        }
        else if (strcmp(argv[i], "--set") == 0 && ArgsLeft >= 1)
        {
            /* skip this: it will be handled in ParseCommandLinePlugin */
            i++;
        }
        else if (strcmp(argv[i], "--debug") == 0)
        {
            l_LaunchDebugger = 1;
        }
        else if (strcmp(argv[i], "--core-compare-send") == 0)
        {
            l_CoreCompareMode = 1;
        }
        else if (strcmp(argv[i], "--core-compare-recv") == 0)
        {
            l_CoreCompareMode = 2;
        }
#define PARSE_GB_CART_PARAM(param, key) \
        else if (strcmp(argv[i], param) == 0) \
        { \
            ConfigSetParameter(l_ConfigTransferPak, key, M64TYPE_STRING, argv[i+1]); \
            i++; \
        }
        PARSE_GB_CART_PARAM("--gb-rom-1", "GB-rom-1")
        PARSE_GB_CART_PARAM("--gb-ram-1", "GB-ram-1")
        PARSE_GB_CART_PARAM("--gb-rom-2", "GB-rom-2")
        PARSE_GB_CART_PARAM("--gb-ram-2", "GB-ram-2")
        PARSE_GB_CART_PARAM("--gb-rom-3", "GB-rom-3")
        PARSE_GB_CART_PARAM("--gb-ram-3", "GB-ram-3")
        PARSE_GB_CART_PARAM("--gb-rom-4", "GB-rom-4")
        PARSE_GB_CART_PARAM("--gb-ram-4", "GB-ram-4")
#undef PARSE_GB_CART_PARAM
        else if (strcmp(argv[i], "--dd-ipl-rom") == 0)
        {
            ConfigSetParameter(l_Config64DD, "IPL-ROM", M64TYPE_STRING, argv[i+1]);
            i++;
        }
        else if (strcmp(argv[i], "--dd-disk") == 0)
        {
            ConfigSetParameter(l_Config64DD, "Disk", M64TYPE_STRING, argv[i+1]);
            i++;
        }
        else if (strcmp(argv[i], "--pif") == 0)
        {
            m64p_error pif_status = M64ERR_INVALID_STATE;
            /* load PIF image */
            FILE *pifPtr = fopen(argv[i+1], "rb");
            if (pifPtr != NULL)
            {
                unsigned char *PIF_buffer = (unsigned char *) malloc(PIF_ROM_SIZE);
                if (PIF_buffer != NULL)
                {
                    if (fread(PIF_buffer, 1, PIF_ROM_SIZE, pifPtr) == PIF_ROM_SIZE)
                    {
                        pif_status = (*CoreDoCommand)(M64CMD_PIF_OPEN, PIF_ROM_SIZE, PIF_buffer);
                    }
                    free(PIF_buffer);
                }
                fclose(pifPtr);
            }
            if (pif_status != M64ERR_SUCCESS)
            {
                DebugMessage(M64MSG_ERROR, "core failed to open PIF ROM file '%s'.", argv[i+1]);
            }
            i++;
        }
        else if (ArgsLeft == 0)
        {
            /* this is the last arg, it should be a ROM filename */
            l_ROMFilepath = argv[i];
            break;
        }
        else if (strcmp(argv[i], "--verbose") == 0)
        {
            g_Verbose = 1;
        }
        else
        {
            DebugMessage(M64MSG_WARNING, "unrecognized command-line parameter '%s'", argv[i]);
        }
        /* continue argv loop */
    }

    if (l_AgentMode)
        SetMax43WindowForAgentMode();
    ApplyAgentProfile();

    if (l_ROMFilepath != NULL)
        return M64ERR_SUCCESS;

    /* missing ROM filepath */
    DebugMessage(M64MSG_ERROR, "no ROM filepath given");
    return M64ERR_INPUT_INVALID;
}

static m64p_error ParseCommandLinePlugin(int argc, const char **argv)
{
    int i;

    /* Third phase of command-line parsing: read all plugin parameters. */
    for (i = 1; i < argc; i++)
    {
        int ArgsLeft = argc - i - 1;
        if (strcmp(argv[i], "--set") == 0 && ArgsLeft >= 1)
        {
            if (SetConfigParameter(argv[i+1]) != 0)
                return M64ERR_INPUT_INVALID;
            i++;
        }
    }
    return M64ERR_SUCCESS;
}

static char* media_loader_get_filename(void* cb_data, m64p_handle section_handle, const char* section, const char* key)
{
#define MUPEN64PLUS_CFG_NAME "mupen64plus.cfg"
    m64p_handle core_config;
    char value[4096];
    const char* configdir = NULL;
    char* cfgfilepath = NULL;

    /* reset filename */
    char* mem_filename = NULL;

    /* XXX: use external config API to force reload of file content */
    configdir = ConfigGetUserConfigPath();
    if (configdir == NULL) {
        DebugMessage(M64MSG_ERROR, "Can't get user config path !");
        return NULL;
    }

    cfgfilepath = combinepath(configdir, MUPEN64PLUS_CFG_NAME);
    if (cfgfilepath == NULL) {
        DebugMessage(M64MSG_ERROR, "Can't get config file path: %s + %s!", configdir, MUPEN64PLUS_CFG_NAME);
        return NULL;
    }

    if (ConfigExternalOpen(cfgfilepath, &core_config) != M64ERR_SUCCESS) {
        DebugMessage(M64MSG_ERROR, "Can't open config file %s!", cfgfilepath);
        goto release_cfgfilepath;
    }

    if (ConfigExternalGetParameter(core_config, section, key, value, sizeof(value)) != M64ERR_SUCCESS) {
        DebugMessage(M64MSG_ERROR, "Can't get parameter %s", key);
        goto close_config;
    }

    size_t len = strlen(value);
    if (len < 2 || value[0] != '"' || value[len-1] != '"') {
        DebugMessage(M64MSG_ERROR, "Invalid string format %s", value);
        goto close_config;
    }

    value[len-1] = '\0';
    mem_filename = strdup(value + 1);

    ConfigSetParameter(section_handle, key, M64TYPE_STRING, mem_filename);

close_config:
    ConfigExternalClose(core_config);
release_cfgfilepath:
    free(cfgfilepath);
    return mem_filename;
}


static char* media_loader_get_gb_cart_mem_file(void* cb_data, const char* mem, int control_id)
{
    char key[64];

    snprintf(key, sizeof(key), "GB-%s-%u", mem, control_id + 1);
    return media_loader_get_filename(cb_data, l_ConfigTransferPak, "Transferpak", key);
}

static char* media_loader_get_gb_cart_rom(void* cb_data, int control_id)
{
    return media_loader_get_gb_cart_mem_file(cb_data, "rom", control_id);
}

static char* media_loader_get_gb_cart_ram(void* cb_data, int control_id)
{
    return media_loader_get_gb_cart_mem_file(cb_data, "ram", control_id);
}

static char* media_loader_get_dd_rom(void* cb_data)
{
    return media_loader_get_filename(cb_data, l_Config64DD, "64DD", "IPL-ROM");
}

static char* media_loader_get_dd_disk(void* cb_data)
{
    return media_loader_get_filename(cb_data, l_Config64DD, "64DD", "Disk");
}

static m64p_media_loader l_media_loader =
{
    NULL,
    media_loader_get_gb_cart_rom,
    media_loader_get_gb_cart_ram,
    NULL,
    media_loader_get_dd_rom,
    media_loader_get_dd_disk
};


/*********************************************************************************************************
* main function
*/

/* Allow state callback in external module to be specified via build flags (header and function name) */
#ifdef CALLBACK_HEADER
#define xstr(s) str(s)
#define str(s) #s
#include xstr(CALLBACK_HEADER)
#endif

static void CoreStateChanged(void *Context, m64p_core_param Param, int Value)
{
    switch (Param)
    {
        case M64CORE_STATE_LOADCOMPLETE:
            SDL_AtomicSet(&l_StateLoadLastResult, Value);
            SDL_AtomicAdd(&l_StateLoadEventSeq, 1);
            break;
        case M64CORE_STATE_SAVECOMPLETE:
            SDL_AtomicSet(&l_StateSaveLastResult, Value);
            SDL_AtomicAdd(&l_StateSaveEventSeq, 1);
            break;
        case M64CORE_SCREENSHOT_CAPTURED:
            SDL_AtomicSet(&l_ScreenshotLastResult, Value);
            SDL_AtomicAdd(&l_ScreenshotEventSeq, 1);
            break;
        default:
            break;
    }

#ifdef CALLBACK_FUNC
    CALLBACK_FUNC(Context, Param, Value);
#endif
}

#ifndef WIN32
/* Allow external modules to call the main function as a library method.  This is useful for user
 * interfaces that simply layer on top of (rather than re-implement) UI-Console (e.g. mupen64plus-ae).
 */
__attribute__ ((visibility("default")))
#endif
int main(int argc, char *argv[])
{
    int i;

    SDL_AtomicSet(&l_LastFrame, 0);
    SDL_AtomicSet(&l_StateLoadEventSeq, 0);
    SDL_AtomicSet(&l_StateLoadLastResult, 0);
    SDL_AtomicSet(&l_StateSaveEventSeq, 0);
    SDL_AtomicSet(&l_StateSaveLastResult, 0);
    SDL_AtomicSet(&l_ScreenshotEventSeq, 0);
    SDL_AtomicSet(&l_ScreenshotLastResult, 0);
    memset(l_AgentInputShadow, 0, sizeof(l_AgentInputShadow));

    printf(" __  __                         __   _  _   ____  _             \n");  
    printf("|  \\/  |_   _ _ __   ___ _ __  / /_ | || | |  _ \\| |_   _ ___ \n");
    printf("| |\\/| | | | | '_ \\ / _ \\ '_ \\| '_ \\| || |_| |_) | | | | / __|  \n");
    printf("| |  | | |_| | |_) |  __/ | | | (_) |__   _|  __/| | |_| \\__ \\  \n");
    printf("|_|  |_|\\__,_| .__/ \\___|_| |_|\\___/   |_| |_|   |_|\\__,_|___/  \n");
    printf("             |_|         https://mupen64plus.org/               \n");
    printf("%s Version %i.%i.%i\n\n", CONSOLE_UI_NAME, VERSION_PRINTF_SPLIT(CONSOLE_UI_VERSION));

    /* bootstrap some special parameters from the command line */
    if (ParseCommandLineInitial(argc, (const char **) argv) != 0)
        return 1;

    /* load the Mupen64Plus core library */
    if (AttachCoreLib(l_CoreLibPath) != M64ERR_SUCCESS)
        return 2;

    /* start the Mupen64Plus core library, load the configuration file */
    m64p_error rval = (*CoreStartup)(CORE_API_VERSION, l_ConfigDirPath, l_DataDirPath, "Core", DebugCallback, NULL, CoreStateChanged);
    if (rval != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_ERROR, "couldn't start Mupen64Plus core library.");
        DetachCoreLib();
        return 3;
    }

#ifdef VIDEXT_HEADER
    rval = CoreOverrideVidExt(&vidExtFunctions);
    if (rval != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_ERROR, "couldn't start VidExt library.");
        DetachCoreLib();
        return 14;
    }
#endif

    /* Open configuration sections */
    rval = OpenConfigurationHandles();
    if (rval != M64ERR_SUCCESS)
    {
        (*CoreShutdown)();
        DetachCoreLib();
        return 4;
    }

    /* parse non-plugin command-line options */
    rval = ParseCommandLineMain(argc, (const char **) argv);
    if (rval != M64ERR_SUCCESS)
    {
        (*CoreShutdown)();
        DetachCoreLib();
        return 5;
    }

    /* Ensure that the core supports comparison feature if necessary */
    if (l_CoreCompareMode != 0 && !(g_CoreCapabilities & M64CAPS_CORE_COMPARE))
    {
        DebugMessage(M64MSG_ERROR, "can't use --core-compare feature with this Mupen64Plus core library.");
        DetachCoreLib();
        return 6;
    }
    compare_core_init(l_CoreCompareMode);
    
    /* Ensure that the core supports the debugger if necessary */
    if (l_LaunchDebugger && !(g_CoreCapabilities & M64CAPS_DEBUGGER))
    {
        DebugMessage(M64MSG_ERROR, "can't use --debug feature with this Mupen64Plus core library.");
        DetachCoreLib();
        return 6;
    }

    /* save the given command-line options in configuration file if requested */
    if (l_SaveOptions)
        SaveConfigurationOptions();

    /* load ROM image */
    FILE *fPtr = fopen(l_ROMFilepath, "rb");
    if (fPtr == NULL)
    {
        DebugMessage(M64MSG_ERROR, "couldn't open ROM file '%s' for reading.", l_ROMFilepath);
        (*CoreShutdown)();
        DetachCoreLib();
        return 7;
    }

    /* get the length of the ROM, allocate memory buffer, load it from disk */
    long romlength = 0;
    fseek(fPtr, 0L, SEEK_END);
    romlength = ftell(fPtr);
    fseek(fPtr, 0L, SEEK_SET);
    unsigned char *ROM_buffer = (unsigned char *) malloc(romlength);
    if (ROM_buffer == NULL)
    {
        DebugMessage(M64MSG_ERROR, "couldn't allocate %li-byte buffer for ROM image file '%s'.", romlength, l_ROMFilepath);
        fclose(fPtr);
        (*CoreShutdown)();
        DetachCoreLib();
        return 8;
    }
    else if (fread(ROM_buffer, 1, romlength, fPtr) != romlength)
    {
        DebugMessage(M64MSG_ERROR, "couldn't read %li bytes from ROM image file '%s'.", romlength, l_ROMFilepath);
        free(ROM_buffer);
        fclose(fPtr);
        (*CoreShutdown)();
        DetachCoreLib();
        return 9;
    }
    fclose(fPtr);

    /* Try to load the ROM image into the core */
    if ((*CoreDoCommand)(M64CMD_ROM_OPEN, (int) romlength, ROM_buffer) != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_ERROR, "core failed to open ROM image file '%s'.", l_ROMFilepath);
        free(ROM_buffer);
        (*CoreShutdown)();
        DetachCoreLib();
        return 10;
    }
    free(ROM_buffer); /* the core copies the ROM image, so we can release this buffer immediately */

    /* handle the cheat codes */
    CheatStart(l_CheatMode, l_CheatNumList);
    if (l_CheatMode == CHEAT_SHOW_LIST)
    {
        (*CoreDoCommand)(M64CMD_ROM_CLOSE, 0, NULL);
        (*CoreShutdown)();
        DetachCoreLib();
        return 11;
    }

    /* search for and load plugins */
    rval = PluginSearchLoad(l_ConfigUI);
    if (rval != M64ERR_SUCCESS)
    {
        (*CoreDoCommand)(M64CMD_ROM_CLOSE, 0, NULL);
        (*CoreShutdown)();
        DetachCoreLib();
        return 12;
    }

    /* Parse and set plugin options. Doing this after loading the plugins
       allows the plugins to set up their own defaults first. */
    rval = ParseCommandLinePlugin(argc, (const char **) argv);
    if (rval != M64ERR_SUCCESS)
    {
        (*CoreDoCommand)(M64CMD_ROM_CLOSE, 0, NULL);
        (*CoreShutdown)();
        DetachCoreLib();
        return 5;
    }

    /* attach plugins to core */
    for (i = 0; i < 4; i++)
    {
        if ((*CoreAttachPlugin)(g_PluginMap[i].type, g_PluginMap[i].handle) != M64ERR_SUCCESS)
        {
            DebugMessage(M64MSG_ERROR, "core error while attaching %s plugin.", g_PluginMap[i].name);
            (*CoreDoCommand)(M64CMD_ROM_CLOSE, 0, NULL);
            (*CoreShutdown)();
            DetachCoreLib();
            return 13;
        }
    }

    /* set up Frame Callback if needed */
    if (l_TestShotList != NULL || l_AgentMode)
    {
        if ((*CoreDoCommand)(M64CMD_SET_FRAME_CALLBACK, 0, FrameCallback) != M64ERR_SUCCESS)
        {
            DebugMessage(M64MSG_WARNING, "couldn't set frame callback, testshots/agent frame tracking will not work.");
        }
    }

    /* set gb cart loader */
    if ((*CoreDoCommand)(M64CMD_SET_MEDIA_LOADER, sizeof(l_media_loader), &l_media_loader) != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_WARNING, "Couldn't set media loader, transferpak and GB carts will not work.");
    }

    /* load savestate at startup */
    if (l_SaveStatePath != NULL)
    {
        if ((*CoreDoCommand)(M64CMD_STATE_LOAD, 0, (void *) l_SaveStatePath) != M64ERR_SUCCESS)
        {
            DebugMessage(M64MSG_WARNING, "couldn't load state, rom will run normally.");
        }
    }

    /* Setup debugger */
    if (l_LaunchDebugger)
    {
        if (debugger_setup_callbacks())
        {
            DebugMessage(M64MSG_ERROR, "couldn't setup debugger callbacks.");
            (*CoreDoCommand)(M64CMD_ROM_CLOSE, 0, NULL);
            (*CoreShutdown)();
            DetachCoreLib();
            return 14;
        }
        /* Set Core config parameter to enable debugger */
        int bEnableDebugger = 1;
        (*ConfigSetParameter)(l_ConfigCore, "EnableDebugger", M64TYPE_BOOL, &bEnableDebugger);
        /* Fork the debugger input thread. */
        SDL_CreateThread(debugger_loop, "DebugLoop", NULL);
    }
    else
    {
        /* Set Core config parameter to disable debugger */
        int bEnableDebugger = 0;
        (*ConfigSetParameter)(l_ConfigCore, "EnableDebugger", M64TYPE_BOOL, &bEnableDebugger);
    }

    /* Save the configuration file again, if necessary, to capture updated
       parameters from plugins. This is the last opportunity to save changes
       before the relatively long-running game. */
    if (l_SaveOptions && (*ConfigHasUnsavedChanges)(NULL))
        (*ConfigSaveFile)();

    if (l_AgentMode)
    {
        if (!StartAgentServer())
        {
            DebugMessage(M64MSG_ERROR, "failed to start agent server");
            for (i = 0; i < 4; i++)
                (*CoreDetachPlugin)(g_PluginMap[i].type);
            PluginUnload();
            (*CoreDoCommand)(M64CMD_ROM_CLOSE, 0, NULL);
            (*CoreShutdown)();
            DetachCoreLib();
            return 15;
        }
    }

    /* run the game */
    (*CoreDoCommand)(M64CMD_EXECUTE, 0, NULL);

    if (l_AgentMode)
        StopAgentServer();

    /* detach plugins from core and unload them */
    for (i = 0; i < 4; i++)
        (*CoreDetachPlugin)(g_PluginMap[i].type);
    PluginUnload();

    /* close the ROM image */
    (*CoreDoCommand)(M64CMD_ROM_CLOSE, 0, NULL);

    /* save the configuration file again if --nosaveoptions was not specified, to keep any updated parameters from the core/plugins */
    if (l_SaveOptions && (*ConfigHasUnsavedChanges)(NULL))
        (*ConfigSaveFile)();

    /* Shut down and release the Core library */
    (*CoreShutdown)();
    DetachCoreLib();

    /* free allocated memory */
    if (l_TestShotList != NULL)
        free(l_TestShotList);

    return 0;
}
