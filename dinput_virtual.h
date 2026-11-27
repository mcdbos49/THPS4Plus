/*
 * dinput_virtual.h - THPS4+ controller input.
 * The game reads pads through DirectInput 8 (DINPUT8.dll). We redirect its DirectInput8Create import to
 * ourselves, give the IDirectInput8A object a private vtable copy (like the d3d8 objects), and add ONE
 * extra device to its enumeration: a virtual DirectInput gamepad. That device is fed from XInput, SDL2
 * (SDL2.dll next to the game) or the winmm joystick API, whichever finds a pad. The keyboard and mouse
 * devices stay the game's own real DirectInput devices; the game's own controller settings screens work
 * on the virtual pad like on any DirectInput gamepad.
 *
 * Needs: #include <windows.h>, <mmsystem.h> before this file, plus logf_() and streq_ci() from the main file.
 * Link with -ldxguid.   [Input] keys: Enable, Backend (auto|sdl|xinput|winmm|off), AlwaysPresent, Deadzone.
 *
 * Virtual pad layout (DIJOYSTATE2): X,Y = left stick, Rx,Ry = right stick, POV0 = D-pad.
 *   buttons: 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 Back, 7 Start, 8 L3, 9 R3, 10 LT, 11 RT, 12-15 D-pad up/down/left/right
 *   (winmm pads: raw button bits 0-31 as the driver reports them).
 */
#ifndef DINPUT_VIRTUAL_H
#define DINPUT_VIRTUAL_H
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>
#include <math.h>

static void logf_(const char *fmt, ...);
static int streq_ci(const char *a, const char *b);

/* ------------------------------------------------------------ settings */
static int g_in_enable = 1, g_in_always = 1, g_in_backend;      /* backend: 0 auto, 1 sdl, 2 winmm, 3 xinput, -1 off */
static float g_in_dead = 0.20f;
static int g_in_type = 0x15, g_in_hid = 1;                       /* DI8DEVTYPE_GAMEPAD (0x15) or JOYSTICK (0x14); DIDEVTYPE_HID flag */
static int g_in_trig_btn[2] = { 11, 12 };                          /* [Input] TriggerButtons=a,b : also report LT / RT as these button numbers (0-127) */
static int g_in_trig_axis = 0;                                   /* [Input] TriggerAxis: LT/RT on the shared Z axis */
static int g_in_dpad_btn = 1, g_in_dpad_pov = 1;                  /* D-pad as buttons 13-16 (the XInput Plus layout your launcher used) and/or as the hat */
static int g_in_vib = 1, g_in_vib_scale = 100;                    /* [Input] Vibration, VibrationScale (percent) */
static int g_in_hide = 1;                                        /* 1 = hide the real pads while the virtual one is offered */
static DWORD g_in_raw;                                           /* [Input] DevTypeRaw: exact dwDevType to report (hex), 0 = build it from the two keys above */
static GUID g_vguid_inst = { 0x7e1c5f10, 0x4a3b, 0x4c2d, { 0x9a, 0x11, 0x50, 0x34, 0x2b, 0x00, 0x00, 0x01 } };   /* the game's config.cfg remembers its pad by this GUID */
static char g_vguid_src[MAX_PATH + 32] = "built-in default";
static int g_vin_quiet;                                         /* 1 while called from DllMain (the log is not ready yet) */
#define VLOG if (!g_vin_quiet) logf_

static void vin_clean(char *s)                                   /* GetPrivateProfileString keeps '; comment' text: cut it off and trim */
{
    char *c = strchr(s, ';');
    size_t n;
    if (c) *c = 0;
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static int vin_parse_guid(const char *s, GUID *g)
{
    unsigned a, b, c, d[8];
    int i;
    while (*s == ' ' || *s == '{') s++;
    if (sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11) return 0;
    g->Data1 = a; g->Data2 = (unsigned short)b; g->Data3 = (unsigned short)c;
    for (i = 0; i < 8; i++) g->Data4[i] = (unsigned char)d[i];
    return 1;
}

/* The game saves the pad it uses as "gp=<GUID>" in config.cfg and only accepts a device with that GUID.
   So the virtual pad takes that GUID: from [Input] InstanceGuid, else from the first gp= line of config.cfg. */
static void vin_find_guid(const char *ini)
{
    char b[MAX_PATH], dir[MAX_PATH], path[MAX_PATH + 16], line[256], *sl;
    const char *rel[3] = { "config.cfg", "Data\\config.cfg", "..\\config.cfg" };
    int i;
    GetPrivateProfileStringA("Input", "InstanceGuid", "", b, sizeof b, ini); vin_clean(b);
    if (b[0] && vin_parse_guid(b, &g_vguid_inst)) { strcpy(g_vguid_src, "ini InstanceGuid"); return; }
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    sl = strrchr(dir, '\\'); if (sl) sl[1] = 0; else dir[0] = 0;
    GetPrivateProfileStringA("Input", "ConfigFile", "", b, sizeof b, ini); vin_clean(b);
    for (i = -1; i < 3; i++) {
        FILE *f;
        if (i < 0) { if (!b[0]) continue; snprintf(path, sizeof path, "%s", b); }
        else snprintf(path, sizeof path, "%s%s", dir, rel[i]);
        f = fopen(path, "r");
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "gp=", 3) == 0 && vin_parse_guid(line + 3, &g_vguid_inst)) {
                snprintf(g_vguid_src, sizeof g_vguid_src, "gp= line of %s", path);
                fclose(f);
                return;
            }
        }
        fclose(f);
    }
}

static void vin_config(const char *ini)
{
    char b[64];
    g_in_enable = (int)GetPrivateProfileIntA("Input", "Enable", 1, ini);
    g_in_always = (int)GetPrivateProfileIntA("Input", "AlwaysPresent", 1, ini);
    g_in_dead = (float)GetPrivateProfileIntA("Input", "Deadzone", 20, ini) / 100.0f;
    if (g_in_dead < 0.0f) g_in_dead = 0.0f;
    if (g_in_dead > 0.9f) g_in_dead = 0.9f;
    GetPrivateProfileStringA("Input", "DeviceType", "gamepad", b, sizeof b, ini); vin_clean(b);
    g_in_type = streq_ci(b, "joystick") ? 0x14 : 0x15;
    g_in_hid = (int)GetPrivateProfileIntA("Input", "HidFlag", 1, ini);
    g_in_hide = (int)GetPrivateProfileIntA("Input", "HideRealPads", 1, ini);
    vin_find_guid(ini);
    g_in_trig_axis = (int)GetPrivateProfileIntA("Input", "TriggerAxis", 0, ini);
    g_in_vib = (int)GetPrivateProfileIntA("Input", "Vibration", 1, ini);
    g_in_vib_scale = (int)GetPrivateProfileIntA("Input", "VibrationScale", 100, ini);
    g_in_dpad_btn = (int)GetPrivateProfileIntA("Input", "DpadButtons", 1, ini);
    g_in_dpad_pov = (int)GetPrivateProfileIntA("Input", "DpadHat", 1, ini);
    GetPrivateProfileStringA("Input", "TriggerButtons", "11,12", b, sizeof b, ini); vin_clean(b);
    g_in_trig_btn[0] = g_in_trig_btn[1] = -1;
    sscanf(b, " %d , %d", &g_in_trig_btn[0], &g_in_trig_btn[1]);
    if (g_in_trig_btn[0] < -1 || g_in_trig_btn[0] > 127) g_in_trig_btn[0] = -1;
    if (g_in_trig_btn[1] < -1 || g_in_trig_btn[1] > 127) g_in_trig_btn[1] = -1;
    GetPrivateProfileStringA("Input", "DevTypeRaw", "0", b, sizeof b, ini); vin_clean(b);
    g_in_raw = (DWORD)strtoul(b, NULL, 16);
    GetPrivateProfileStringA("Input", "Backend", "auto", b, sizeof b, ini); vin_clean(b);
    g_in_backend = streq_ci(b, "sdl") ? 1 : streq_ci(b, "winmm") ? 2 : streq_ci(b, "xinput") ? 3 :
                   (streq_ci(b, "off") || streq_ci(b, "dinput") || streq_ci(b, "native")) ? -1 : 0;
}

/* ------------------------------------------------------------ pad state + backends */
typedef struct { int ok; float lx, ly, rx, ry, lt, rt; unsigned btn; int pov; } VPad;     /* sticks: +x right, +y UP; pov: -1 or centidegrees */

static int vin_pov(int up, int down, int left, int right)
{
    if (up && right) return 4500;
    if (right && down) return 13500;
    if (down && left) return 22500;
    if (left && up) return 31500;
    if (up) return 0;
    if (right) return 9000;
    if (down) return 18000;
    if (left) return 27000;
    return -1;
}

static void vin_stick(float *x, float *y)
{
    float m = (float)sqrt((double)(*x * *x + *y * *y));
    if (m < g_in_dead) { *x = 0; *y = 0; return; }
    {
        float s = (m > 1.0f ? 1.0f : m);
        s = (s - g_in_dead) / (1.0f - g_in_dead) / m;
        *x *= s; *y *= s;
        if (*x > 1.0f) *x = 1.0f;
        if (*x < -1.0f) *x = -1.0f;
        if (*y > 1.0f) *y = 1.0f;
        if (*y < -1.0f) *y = -1.0f;
    }
}

/* ---- XInput ---- */
typedef struct { DWORD pkt; WORD btn; BYTE lt, rt; SHORT lx, ly, rx, ry; } VXI_STATE;
typedef DWORD (WINAPI *VXI_Get)(DWORD, VXI_STATE *);
static VXI_Get v_xi;
typedef DWORD (WINAPI *VXI_Set)(DWORD, void *);
static VXI_Set v_xi_set;
static int v_active;                                             /* backend that delivered the last pad state: 1 sdl, 2 winmm, 3 xinput */
static int v_xi_tried, v_xi_idx = -1;
static DWORD v_xi_next;

static int vin_poll_xinput(VPad *p)
{
    VXI_STATE s;
    int i;
    DWORD now = GetTickCount();
    if (!v_xi_tried) {
        static const char *dlls[3] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        int k;
        v_xi_tried = 1;
        for (k = 0; k < 3 && !v_xi; k++) {
            HMODULE m = LoadLibraryA(dlls[k]);
            if (m) { v_xi = (VXI_Get)GetProcAddress(m, "XInputGetState"); v_xi_set = (VXI_Set)GetProcAddress(m, "XInputSetState"); }
        }
        logf_("input: XInput %s\n", v_xi ? "loaded" : "not available");
    }
    if (!v_xi) return 0;
    if (v_xi_idx >= 0 && v_xi((DWORD)v_xi_idx, &s) != 0) v_xi_idx = -1;         /* (a pad that vanished just stops vibrating) */
    if (v_xi_idx < 0) {
        if ((int)(now - v_xi_next) < 0) return 0;               /* polling empty slots is slow: look once a second */
        v_xi_next = now + 1000;
        for (i = 0; i < 4; i++) if (v_xi((DWORD)i, &s) == 0) { v_xi_idx = i; logf_("input: XInput pad found in slot %d\n", i); break; }
        if (v_xi_idx < 0) return 0;
    }
    memset(p, 0, sizeof *p);
    p->ok = 1;
    p->lt = s.lt / 255.0f; p->rt = s.rt / 255.0f;
    p->lx = s.lx / 32768.0f; p->ly = s.ly / 32768.0f; p->rx = s.rx / 32768.0f; p->ry = s.ry / 32768.0f;
    if (s.btn & 0x1000) p->btn |= 1u << 0;
    if (s.btn & 0x2000) p->btn |= 1u << 1;
    if (s.btn & 0x4000) p->btn |= 1u << 2;
    if (s.btn & 0x8000) p->btn |= 1u << 3;
    if (s.btn & 0x0100) p->btn |= 1u << 4;
    if (s.btn & 0x0200) p->btn |= 1u << 5;
    if (s.btn & 0x0020) p->btn |= 1u << 6;
    if (s.btn & 0x0010) p->btn |= 1u << 7;
    if (s.btn & 0x0040) p->btn |= 1u << 8;
    if (s.btn & 0x0080) p->btn |= 1u << 9;
    if (s.lt > 40) p->btn |= 1u << 10;
    if (s.rt > 40) p->btn |= 1u << 11;
    if (s.btn & 0x1) p->btn |= 1u << 12;
    if (s.btn & 0x2) p->btn |= 1u << 13;
    if (s.btn & 0x4) p->btn |= 1u << 14;
    if (s.btn & 0x8) p->btn |= 1u << 15;
    p->pov = vin_pov(s.btn & 1, s.btn & 2, s.btn & 4, s.btn & 8);
    return 1;
}

/* ---- SDL2 (game controller API) ---- */
typedef int (*VSDL_Init)(DWORD);
typedef int (*VSDL_Int)(void);
typedef int (*VSDL_IntInt)(int);
typedef void *(*VSDL_Open)(int);
typedef void (*VSDL_Void)(void);
typedef int (*VSDL_PtrInt)(void *);
typedef short (*VSDL_Axis)(void *, int);
typedef unsigned char (*VSDL_Btn)(void *, int);
typedef void (*VSDL_Close)(void *);
static int v_sdl_tried;
static VSDL_Int s_NumJoy;
static VSDL_IntInt s_IsGC;
static VSDL_Open s_Open;
static VSDL_Close s_Close;
static VSDL_PtrInt s_Attached;
static VSDL_Axis s_Axis;
static VSDL_Btn s_Btn;
static VSDL_Void s_Update;
typedef int (*VSDL_Rumble)(void *, unsigned short, unsigned short, unsigned);
static VSDL_Rumble s_Rumble;
static void *v_sdl_pad;
static DWORD v_sdl_next;

static int vin_poll_sdl(VPad *p)
{
    DWORD now = GetTickCount();
    float a;
    static const unsigned char map[15] = { 0, 1, 2, 3, 6, 0xFF, 7, 8, 9, 4, 5, 12, 13, 14, 15 };   /* SDL button -> our bit */
    int b;
    if (!v_sdl_tried) {
        HMODULE m = LoadLibraryA("SDL2.dll");
        VSDL_Init init;
        v_sdl_tried = 1;
        if (m) {
            init = (VSDL_Init)GetProcAddress(m, "SDL_Init");
            s_NumJoy = (VSDL_Int)GetProcAddress(m, "SDL_NumJoysticks");
            s_IsGC = (VSDL_IntInt)GetProcAddress(m, "SDL_IsGameController");
            s_Open = (VSDL_Open)GetProcAddress(m, "SDL_GameControllerOpen");
            s_Close = (VSDL_Close)GetProcAddress(m, "SDL_GameControllerClose");
            s_Attached = (VSDL_PtrInt)GetProcAddress(m, "SDL_GameControllerGetAttached");
            s_Axis = (VSDL_Axis)GetProcAddress(m, "SDL_GameControllerGetAxis");
            s_Btn = (VSDL_Btn)GetProcAddress(m, "SDL_GameControllerGetButton");
            s_Update = (VSDL_Void)GetProcAddress(m, "SDL_GameControllerUpdate");
            s_Rumble = (VSDL_Rumble)GetProcAddress(m, "SDL_GameControllerRumble");
            if (!(init && s_NumJoy && s_IsGC && s_Open && s_Attached && s_Axis && s_Btn && s_Update) || init(0x200 | 0x2000) != 0) {
                s_Open = NULL;
                logf_("input: SDL2.dll found but unusable\n");
            } else logf_("input: SDL2 loaded\n");
        }
    }
    if (!s_Open) return 0;
    if (v_sdl_pad && !s_Attached(v_sdl_pad)) { if (s_Close) s_Close(v_sdl_pad); v_sdl_pad = NULL; }
    if (!v_sdl_pad) {
        int i, n;
        if ((int)(now - v_sdl_next) < 0) return 0;
        v_sdl_next = now + 1000;
        if (s_Update) s_Update();
        n = s_NumJoy();
        for (i = 0; i < n && !v_sdl_pad; i++) if (s_IsGC(i)) { v_sdl_pad = s_Open(i); if (v_sdl_pad) logf_("input: SDL2 pad %d opened\n", i); }
        if (!v_sdl_pad) return 0;
    }
    s_Update();
    memset(p, 0, sizeof *p);
    p->ok = 1;
    p->lt = s_Axis(v_sdl_pad, 4) / 32767.0f; p->rt = s_Axis(v_sdl_pad, 5) / 32767.0f;
    p->lx = s_Axis(v_sdl_pad, 0) / 32768.0f; p->ly = -s_Axis(v_sdl_pad, 1) / 32768.0f;       /* SDL: y down is positive */
    p->rx = s_Axis(v_sdl_pad, 2) / 32768.0f; p->ry = -s_Axis(v_sdl_pad, 3) / 32768.0f;
    for (b = 0; b < 15; b++) if (map[b] != 0xFF && s_Btn(v_sdl_pad, b)) p->btn |= 1u << map[b];
    a = s_Axis(v_sdl_pad, 4); if (a > 10000.0f) p->btn |= 1u << 10;
    a = s_Axis(v_sdl_pad, 5); if (a > 10000.0f) p->btn |= 1u << 11;
    p->pov = vin_pov(s_Btn(v_sdl_pad, 11), s_Btn(v_sdl_pad, 12), s_Btn(v_sdl_pad, 13), s_Btn(v_sdl_pad, 14));
    return 1;
}

/* ---- winmm joystick (any generic DirectInput-era pad) ---- */
static int v_wm_idx = -1;
static DWORD v_wm_next;

static int vin_poll_winmm(VPad *p)
{
    JOYINFOEX ji;
    DWORD now = GetTickCount();
    UINT i;
    memset(&ji, 0, sizeof ji);
    ji.dwSize = sizeof ji; ji.dwFlags = JOY_RETURNALL;
    if (v_wm_idx >= 0) { if (joyGetPosEx((UINT)v_wm_idx, &ji) != JOYERR_NOERROR) v_wm_idx = -1; }
    if (v_wm_idx < 0) {
        if ((int)(now - v_wm_next) < 0) return 0;
        v_wm_next = now + 1000;
        for (i = 0; i < 4; i++) {
            ji.dwSize = sizeof ji; ji.dwFlags = JOY_RETURNALL;
            if (joyGetPosEx(i, &ji) == JOYERR_NOERROR) { v_wm_idx = (int)i; logf_("input: winmm joystick %u found\n", i); break; }
        }
        if (v_wm_idx < 0) return 0;
    }
    memset(p, 0, sizeof *p);
    p->ok = 1;
    p->lx = ((float)ji.dwXpos - 32767.5f) / 32768.0f; p->ly = -((float)ji.dwYpos - 32767.5f) / 32768.0f;
    p->rx = ((float)ji.dwZpos - 32767.5f) / 32768.0f; p->ry = -((float)ji.dwRpos - 32767.5f) / 32768.0f;
    p->btn = ji.dwButtons;
    p->pov = (ji.dwPOV == 0xFFFF || ji.dwPOV == 0xFFFFFFFF) ? -1 : (int)ji.dwPOV;
    return 1;
}

static int vin_poll(VPad *p)
{
    memset(p, 0, sizeof *p); p->pov = -1;
    if (g_in_backend == 1) { int ok = vin_poll_sdl(p); v_active = ok ? 1 : 0; return ok; }
    if (g_in_backend == 2) { int ok = vin_poll_winmm(p); v_active = ok ? 2 : 0; return ok; }
    if (g_in_backend == 3) { int ok = vin_poll_xinput(p); v_active = ok ? 3 : 0; return ok; }
    if (vin_poll_sdl(p)) { v_active = 1; return 1; }
    if (vin_poll_xinput(p)) { v_active = 3; return 1; }
    if (vin_poll_winmm(p)) { v_active = 2; return 1; }
    v_active = 0;
    return 0;
}

/* motors: lo = heavy (left) motor, hi = light (right) motor, both 0..1 */
typedef struct { WORD l, r; } VXI_VIB;
static void vin_rumble(float lo, float hi)
{
    static WORD last_l, last_r;
    WORD l, r;
    if (!g_in_vib) return;
    lo *= g_in_vib_scale / 100.0f; hi *= g_in_vib_scale / 100.0f;
    l = (WORD)(lo <= 0 ? 0 : lo >= 1 ? 65535 : lo * 65535.0f);
    r = (WORD)(hi <= 0 ? 0 : hi >= 1 ? 65535 : hi * 65535.0f);
    if ((v_active == 3 || v_active == 0) && v_xi_set && v_xi_idx >= 0) {              /* an XInput pad is in use (also covers a forced Backend that never set v_active) */
        if (l != last_l || r != last_r) { VXI_VIB v; v.l = l; v.r = r; v_xi_set((DWORD)v_xi_idx, &v); }
    } else if (v_active == 1 && s_Rumble && v_sdl_pad) {
        if (l || r || last_l || last_r) s_Rumble(v_sdl_pad, l, r, 150);
    }
    { static int n; if ((l || r) && (l != last_l || r != last_r) && n++ < 12) logf_("input: rumble %u / %u (backend %d, xinput slot %d)\n", (unsigned)l, (unsigned)r, v_active, v_xi_idx); }
    last_l = l; last_r = r;
}

/* ------------------------------------------------------------ the virtual DirectInput device */
static void vff_update(void);                                    /* defined further down with the force-feedback code */
static const GUID g_vguid_prod = { 0x028E045E, 0x0000, 0x0000, { 0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44 } };
#define VNBUTTONS 17      /* the XInput Plus layout: 0-9 A B X Y LB RB Back Start L3 R3, 10 Guide, 11 LT, 12 RT, 13-16 D-pad up/right/down/left */
#define VNAXES 5
static DWORD vin_devtype(void) { if (g_in_raw) return g_in_raw; return (DWORD)g_in_type | (2u << 8) | (g_in_hid ? 0x10000u : 0u); }

typedef struct { const IDirectInputDevice8AVtbl *lpVtbl; LONG ref; LONG rmin[6], rmax[6]; } VDev;

static int vd_axis_of(const DIPROPHEADER *h)
{
    if (h->dwHow == DIPH_BYOFFSET) return (h->dwObj < 4 * VNAXES && (h->dwObj & 3) == 0) ? (int)(h->dwObj / 4) : -1;
    if (h->dwHow == DIPH_BYID) {
        DWORD inst = (h->dwObj >> 8) & 0xFFFF;
        return ((h->dwObj & DIDFT_AXIS) && inst < VNAXES) ? (int)inst : -1;
    }
    return -1;
}

static HRESULT WINAPI vd_QueryInterface(IDirectInputDevice8A *self, REFIID riid, void **ppv)
{
    int ok = IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectInputDevice8A) || IsEqualGUID(riid, &IID_IDirectInputDevice7A) ||
             IsEqualGUID(riid, &IID_IDirectInputDevice2A) || IsEqualGUID(riid, &IID_IDirectInputDeviceA);
    { static int n; if (n++ < 8) logf_("input: game QueryInterface %08lX-%04X -> %s\n", (unsigned long)riid->Data1, riid->Data2, ok ? "yes" : "no"); }
    if (ok) { *ppv = self; ((VDev *)self)->ref++; return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI vd_AddRef(IDirectInputDevice8A *self) { return (ULONG)InterlockedIncrement(&((VDev *)self)->ref); }
static ULONG WINAPI vd_Release(IDirectInputDevice8A *self)
{
    LONG r = InterlockedDecrement(&((VDev *)self)->ref);
    if (r <= 0) { free(self); return 0; }
    return (ULONG)r;
}

static HRESULT WINAPI vd_GetCapabilities(IDirectInputDevice8A *self, LPDIDEVCAPS c)
{
    DWORD sz;
    (void)self;
    if (!c || c->dwSize < 24) return DIERR_INVALIDPARAM;
    sz = c->dwSize;
    memset(c, 0, sz); c->dwSize = sz;
    c->dwFlags = DIDC_ATTACHED | (g_in_vib ? (DIDC_FORCEFEEDBACK | DIDC_STARTDELAY) : 0);
    if (g_in_vib && sz >= 44) { c->dwFFSamplePeriod = 1000; c->dwFFMinTimeResolution = 1000; }
    c->dwDevType = vin_devtype();
    c->dwAxes = VNAXES; c->dwButtons = VNBUTTONS; c->dwPOVs = 1;
    { static int n; if (n++ < 3) logf_("input: game GetCapabilities (struct size %lu) -> flags 0x%lX, %lu axes, %lu buttons\n", (unsigned long)sz, (unsigned long)c->dwFlags, (unsigned long)c->dwAxes, (unsigned long)c->dwButtons); }
    return DI_OK;
}

static HRESULT WINAPI vd_EnumObjects(IDirectInputDevice8A *self, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags)
{
    static const GUID *ag[5] = { &GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis };
    static const char *an[5] = { "X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation" };
    DIDEVICEOBJECTINSTANCEA o;
    int i;
    (void)self;
    { static int n; if (n++ < 4) logf_("input: game EnumObjects (flags 0x%lX)\n", (unsigned long)flags); }
    if (!cb) return DIERR_INVALIDPARAM;
    if (flags == DIDFT_ALL || (flags & DIDFT_AXIS)) {
        for (i = 0; i < VNAXES; i++) {
            memset(&o, 0, sizeof o); o.dwSize = sizeof o;
            o.guidType = *ag[i]; o.dwOfs = (DWORD)(i * 4);
            o.dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE((DWORD)i); o.dwFlags = DIDOI_ASPECTPOSITION;
            if (g_in_vib && i < 2) { o.dwFlags |= DIDOI_FFACTUATOR; o.dwFFMaxForce = 10000; o.dwFFForceResolution = 100; }     /* X and Y are force-feedback actuators, like on a rumble pad */
            strcpy(o.tszName, an[i]);
            if (cb(&o, ref) == DIENUM_STOP) return DI_OK;
        }
    }
    if (flags == DIDFT_ALL || (flags & DIDFT_POV)) {
        memset(&o, 0, sizeof o); o.dwSize = sizeof o;
        o.guidType = GUID_POV; o.dwOfs = 32; o.dwType = DIDFT_POV | DIDFT_MAKEINSTANCE(0);
        strcpy(o.tszName, "Hat Switch");
        if (cb(&o, ref) == DIENUM_STOP) return DI_OK;
    }
    if (flags == DIDFT_ALL || (flags & DIDFT_BUTTON)) {
        for (i = 0; i < VNBUTTONS; i++) {
            memset(&o, 0, sizeof o); o.dwSize = sizeof o;
            o.guidType = GUID_Button; o.dwOfs = (DWORD)(48 + i);
            o.dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE((DWORD)i);
            sprintf(o.tszName, "Button %d", i);
            if (cb(&o, ref) == DIENUM_STOP) return DI_OK;
        }
    }
    return DI_OK;
}

static HRESULT WINAPI vd_GetProperty(IDirectInputDevice8A *self, REFGUID g, LPDIPROPHEADER h)
{
    VDev *d = (VDev *)self;
    { static int n; if (n++ < 6) logf_("input: game GetProperty %lu\n", (unsigned long)(DWORD_PTR)g); }
    if ((DWORD_PTR)g == 4 && h && h->dwSize >= sizeof(DIPROPRANGE)) {          /* DIPROP_RANGE */
        DIPROPRANGE *r = (DIPROPRANGE *)h;
        int a = (h->dwHow == DIPH_DEVICE) ? 0 : vd_axis_of(h);
        if (a < 0) return DIERR_OBJECTNOTFOUND;
        r->lMin = d->rmin[a]; r->lMax = d->rmax[a];
        return DI_OK;
    }
    return E_NOTIMPL;
}

static HRESULT WINAPI vd_SetProperty(IDirectInputDevice8A *self, REFGUID g, LPCDIPROPHEADER h)
{
    VDev *d = (VDev *)self;
    if ((DWORD_PTR)g == 4 && h && h->dwSize >= sizeof(DIPROPRANGE)) {
        const DIPROPRANGE *r = (const DIPROPRANGE *)h;
        int a, i;
        if (h->dwHow == DIPH_DEVICE) { for (i = 0; i < 6; i++) { d->rmin[i] = r->lMin; d->rmax[i] = r->lMax; } }
        else { a = vd_axis_of(h); if (a >= 0) { d->rmin[a] = r->lMin; d->rmax[a] = r->lMax; } }
    }
    { static int n; if (n++ < 12) logf_("input: game SetProperty %lu (how %lu, obj 0x%lX)\n", (unsigned long)(DWORD_PTR)g, h ? (unsigned long)h->dwHow : 0UL, h ? (unsigned long)h->dwObj : 0UL); }
    return DI_OK;                                                               /* deadzone, saturation, buffer size ...: accepted, ignored */
}

static LONG vd_axis(const VDev *d, int a, float v)                              /* v in -1..1 */
{
    float mid = ((float)d->rmin[a] + (float)d->rmax[a]) * 0.5f, half = ((float)d->rmax[a] - (float)d->rmin[a]) * 0.5f;
    return (LONG)(mid + v * half);
}

static HRESULT WINAPI vd_GetDeviceState(IDirectInputDevice8A *self, DWORD cb, LPVOID out)
{
    VDev *d = (VDev *)self;
    DIJOYSTATE2 j;
    VPad p;
    int i;
    if (!out) return DIERR_INVALIDPARAM;
    { static int once; if (!once) { once = 1; logf_("input: game reads the pad state (%lu bytes)\n", (unsigned long)cb); } }
    vff_update();
    memset(&j, 0, sizeof j);
    for (i = 0; i < 4; i++) j.rgdwPOV[i] = 0xFFFFFFFFu;
    j.lX = vd_axis(d, 0, 0); j.lY = vd_axis(d, 1, 0); j.lZ = vd_axis(d, 2, 0);
    j.lRx = vd_axis(d, 3, 0); j.lRy = vd_axis(d, 4, 0); j.lRz = vd_axis(d, 5, 0);
    j.rglSlider[0] = vd_axis(d, 2, 0); j.rglSlider[1] = vd_axis(d, 2, 0);
    if (vin_poll(&p) && p.ok) {
        float lx = p.lx, ly = p.ly, rx = p.rx, ry = p.ry;
        vin_stick(&lx, &ly); vin_stick(&rx, &ry);
        j.lX = vd_axis(d, 0, lx);  j.lY = vd_axis(d, 1, -ly);                   /* DirectInput: Y down is positive */
        j.lRx = vd_axis(d, 3, rx); j.lRy = vd_axis(d, 4, -ry);
        if (g_in_trig_axis) j.lZ = vd_axis(d, 2, p.lt - p.rt);                                      /* triggers share Z like on an Xbox 360 pad: LT +, RT - */
        j.rgdwPOV[0] = (p.pov < 0 || !g_in_dpad_pov) ? 0xFFFFFFFFu : (DWORD)p.pov;
        for (i = 0; i < 10; i++) if (p.btn & (1u << i)) j.rgbButtons[i] = 0x80;
        if (g_in_dpad_btn) {                                                    /* internal bits 12 up, 13 down, 14 left, 15 right */
            if (p.btn & (1u << 12)) j.rgbButtons[13] = 0x80;
            if (p.btn & (1u << 15)) j.rgbButtons[14] = 0x80;
            if (p.btn & (1u << 13)) j.rgbButtons[15] = 0x80;
            if (p.btn & (1u << 14)) j.rgbButtons[16] = 0x80;
        }
        if (g_in_trig_btn[0] >= 0 && p.lt > 0.16f) j.rgbButtons[g_in_trig_btn[0]] = 0x80;
        if (g_in_trig_btn[1] >= 0 && p.rt > 0.16f) j.rgbButtons[g_in_trig_btn[1]] = 0x80;
    }
    memcpy(out, &j, cb < sizeof j ? cb : sizeof j);
    return DI_OK;
}

static HRESULT WINAPI vd_SetDataFormat(IDirectInputDevice8A *self, LPCDIDATAFORMAT f)
{
    static int once;
    (void)self;
    if (f && !once) {
        DWORD i;
        once = 1;
        logf_("input: game SetDataFormat: data size %lu, %lu objects, flags 0x%lX\n", (unsigned long)f->dwDataSize, (unsigned long)f->dwNumObjs, (unsigned long)f->dwFlags);
        for (i = 0; i < f->dwNumObjs && i < 64; i++)
            logf_("   object ofs %lu type 0x%lX flags 0x%lX\n", (unsigned long)f->rgodf[i].dwOfs, (unsigned long)f->rgodf[i].dwType, (unsigned long)f->rgodf[i].dwFlags);
    }
    return DI_OK;
}

/* ---- force feedback: DirectInput effects played as XInput / SDL rumble ---- */
#define VFX_MAX 32
typedef struct { const IDirectInputEffectVtbl *lpVtbl; LONG ref; GUID type; int kind;       /* 0 constant, 1 ramp, 2 periodic */
                 DWORD duration, gain, delay, iters; LONG a, b; int playing; DWORD t0; } VFx;
static VFx *g_vfx[VFX_MAX];
static int g_vff_paused;
static IDirectInputEffectVtbl g_vfx_vtbl;
static int g_vfx_ready;

static void vff_update(void)
{
    DWORD now = GetTickCount();
    float lo = 0.0f, hi = 0.0f;
    int i;
    for (i = 0; i < VFX_MAX; i++) {
        VFx *f = g_vfx[i];
        unsigned long long el;
        float s, frac;
        if (!f || !f->playing || g_vff_paused) continue;
        el = (unsigned long long)(now - f->t0) * 1000ULL;
        if (el < f->delay) continue;
        el -= f->delay;
        if (f->duration != 0xFFFFFFFFu && f->iters != 0xFFFFFFFFu && el >= (unsigned long long)f->duration * f->iters) { f->playing = 0; continue; }
        if (f->kind == 1) {
            frac = (f->duration && f->duration != 0xFFFFFFFFu) ? (float)(el % f->duration) / (float)f->duration : 0.0f;
            s = (float)fabs((double)f->a + ((double)f->b - (double)f->a) * frac) / 10000.0f;
        } else s = (float)fabs((double)f->a) / 10000.0f;
        s *= (float)f->gain / 10000.0f;
        if (s > 1.0f) s = 1.0f;
        if (f->kind == 2) { if (s > hi) hi = s; } else if (s > lo) lo = s;
    }
    vin_rumble(lo, hi);
}

static void vfx_set(VFx *f, const DIEFFECT *e, DWORD fl, int all)
{
    if (!e) return;
    if (all || (fl & DIEP_DURATION)) f->duration = e->dwDuration;
    if (all || (fl & DIEP_GAIN)) f->gain = e->dwGain;
    if ((all || (fl & DIEP_STARTDELAY)) && e->dwSize >= sizeof(DIEFFECT)) f->delay = e->dwStartDelay;
    if ((all || (fl & DIEP_TYPESPECIFICPARAMS)) && e->lpvTypeSpecificParams) {
        if (f->kind == 0 && e->cbTypeSpecificParams >= sizeof(DICONSTANTFORCE)) f->a = ((const DICONSTANTFORCE *)e->lpvTypeSpecificParams)->lMagnitude;
        else if (f->kind == 1 && e->cbTypeSpecificParams >= sizeof(DIRAMPFORCE)) { f->a = ((const DIRAMPFORCE *)e->lpvTypeSpecificParams)->lStart; f->b = ((const DIRAMPFORCE *)e->lpvTypeSpecificParams)->lEnd; }
        else if (f->kind == 2 && e->cbTypeSpecificParams >= sizeof(DIPERIODIC)) f->a = (LONG)((const DIPERIODIC *)e->lpvTypeSpecificParams)->dwMagnitude;
    }
}

static HRESULT WINAPI vf_QueryInterface(IDirectInputEffect *self, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectInputEffect)) { *ppv = self; ((VFx *)self)->ref++; return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI vf_AddRef(IDirectInputEffect *self) { return (ULONG)InterlockedIncrement(&((VFx *)self)->ref); }
static ULONG WINAPI vf_Release(IDirectInputEffect *self)
{
    VFx *f = (VFx *)self;
    LONG r = InterlockedDecrement(&f->ref);
    if (r <= 0) {
        int i;
        for (i = 0; i < VFX_MAX; i++) if (g_vfx[i] == f) g_vfx[i] = NULL;
        free(f);
        vff_update();
        return 0;
    }
    return (ULONG)r;
}
static HRESULT WINAPI vf_Initialize(IDirectInputEffect *self, HINSTANCE hi, DWORD ver, REFGUID g) { (void)self; (void)hi; (void)ver; (void)g; return DI_OK; }
static HRESULT WINAPI vf_GetEffectGuid(IDirectInputEffect *self, LPGUID g) { if (!g) return DIERR_INVALIDPARAM; *g = ((VFx *)self)->type; return DI_OK; }
static HRESULT WINAPI vf_GetParameters(IDirectInputEffect *self, LPDIEFFECT e, DWORD fl)
{
    VFx *f = (VFx *)self; (void)fl;
    if (!e) return DIERR_INVALIDPARAM;
    e->dwDuration = f->duration; e->dwGain = f->gain;
    if (e->dwSize >= sizeof(DIEFFECT)) e->dwStartDelay = f->delay;
    return DI_OK;
}
static HRESULT WINAPI vf_SetParameters(IDirectInputEffect *self, LPCDIEFFECT e, DWORD fl)
{
    VFx *f = (VFx *)self;
    vfx_set(f, e, fl, 0);
    { static int n; if (n++ < 12) logf_("input: game SetParameters (flags 0x%lX) -> magnitude %ld, duration %lu us, gain %lu, start delay %lu\n", (unsigned long)fl, (long)f->a, (unsigned long)f->duration, (unsigned long)f->gain, (unsigned long)f->delay); }
    if (fl & DIEP_START) { f->t0 = GetTickCount(); f->playing = 1; f->iters = 1; }
    vff_update();
    return DI_OK;
}
static HRESULT WINAPI vf_Start(IDirectInputEffect *self, DWORD iters, DWORD flags)
{
    VFx *f = (VFx *)self; (void)flags;
    { static int n; if (n++ < 6) logf_("input: game starts a force-feedback effect (kind %d, magnitude %ld, duration %lu us, gain %lu)\n", f->kind, (long)f->a, (unsigned long)f->duration, (unsigned long)f->gain); }
    f->iters = iters ? iters : 1; f->t0 = GetTickCount(); f->playing = 1;
    vff_update();
    return DI_OK;
}
static HRESULT WINAPI vf_Stop(IDirectInputEffect *self) { ((VFx *)self)->playing = 0; vff_update(); return DI_OK; }
static HRESULT WINAPI vf_GetEffectStatus(IDirectInputEffect *self, LPDWORD st) { if (st) *st = ((VFx *)self)->playing ? DIEGES_PLAYING : 0; return DI_OK; }
static HRESULT WINAPI vf_Download(IDirectInputEffect *self) { static int n; (void)self; if (n++ < 3) logf_("input: game calls Download on an effect\n"); return DI_OK; }
static HRESULT WINAPI vf_Unload(IDirectInputEffect *self) { (void)self; return DI_OK; }
static HRESULT WINAPI vf_Escape(IDirectInputEffect *self, LPDIEFFESCAPE x) { (void)self; (void)x; return E_NOTIMPL; }

static void vf_build(void)
{
    if (g_vfx_ready) return;
    g_vfx_ready = 1;
    g_vfx_vtbl.QueryInterface = vf_QueryInterface; g_vfx_vtbl.AddRef = vf_AddRef; g_vfx_vtbl.Release = vf_Release;
    g_vfx_vtbl.Initialize = vf_Initialize; g_vfx_vtbl.GetEffectGuid = vf_GetEffectGuid;
    g_vfx_vtbl.GetParameters = vf_GetParameters; g_vfx_vtbl.SetParameters = vf_SetParameters;
    g_vfx_vtbl.Start = vf_Start; g_vfx_vtbl.Stop = vf_Stop; g_vfx_vtbl.GetEffectStatus = vf_GetEffectStatus;
    g_vfx_vtbl.Download = vf_Download; g_vfx_vtbl.Unload = vf_Unload; g_vfx_vtbl.Escape = vf_Escape;
}

static int vfx_kind(const GUID *g)
{
    if (IsEqualGUID(g, &GUID_ConstantForce)) return 0;
    if (IsEqualGUID(g, &GUID_RampForce)) return 1;
    if (IsEqualGUID(g, &GUID_Square) || IsEqualGUID(g, &GUID_Sine) || IsEqualGUID(g, &GUID_Triangle) ||
        IsEqualGUID(g, &GUID_SawtoothUp) || IsEqualGUID(g, &GUID_SawtoothDown)) return 2;
    return -1;
}

static HRESULT WINAPI vd_CreateEffect(IDirectInputDevice8A *self, REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT *pp, LPUNKNOWN outer)
{
    VFx *f;
    int k, i;
    (void)self; (void)outer;
    if (!g || !pp) return DIERR_INVALIDPARAM;
    k = vfx_kind(g);
    { static int n; if (n++ < 6) logf_("input: game creates a force-feedback effect (%08lX..., kind %d)\n", (unsigned long)g->Data1, k); }
    if (k < 0) return DIERR_DEVICENOTREG;
    f = (VFx *)calloc(1, sizeof *f);
    if (!f) return E_OUTOFMEMORY;
    vf_build();
    f->lpVtbl = &g_vfx_vtbl; f->ref = 1; f->type = *g; f->kind = k; f->gain = 10000; f->iters = 1; f->duration = 0xFFFFFFFFu;
    vfx_set(f, e, 0, 1);
    for (i = 0; i < VFX_MAX; i++) if (!g_vfx[i]) { g_vfx[i] = f; break; }
    *pp = (LPDIRECTINPUTEFFECT)f;
    return DI_OK;
}

static const GUID *vfx_guids[7];
static void vfx_guid_init(void)
{
    vfx_guids[0] = &GUID_ConstantForce; vfx_guids[1] = &GUID_RampForce; vfx_guids[2] = &GUID_Square; vfx_guids[3] = &GUID_Sine;
    vfx_guids[4] = &GUID_Triangle; vfx_guids[5] = &GUID_SawtoothUp; vfx_guids[6] = &GUID_SawtoothDown;
}
static void vfx_info(int i, DIEFFECTINFOA *o)
{
    static const char *nm[7] = { "Constant Force", "Ramp Force", "Square", "Sine", "Triangle", "Sawtooth Up", "Sawtooth Down" };
    memset(o, 0, sizeof *o); o->dwSize = sizeof *o;
    o->guid = *vfx_guids[i];
    o->dwEffType = (i == 0) ? DIEFT_CONSTANTFORCE : (i == 1) ? DIEFT_RAMPFORCE : DIEFT_PERIODIC;
    o->dwStaticParams = o->dwDynamicParams = DIEP_DURATION | DIEP_GAIN | DIEP_TYPESPECIFICPARAMS | DIEP_STARTDELAY;
    strcpy(o->tszName, nm[i]);
}
static HRESULT WINAPI vd_EnumEffects(IDirectInputDevice8A *self, LPDIENUMEFFECTSCALLBACKA cb, LPVOID ref, DWORD type)
{
    int i;
    (void)self;
    if (!cb) return DIERR_INVALIDPARAM;
    vfx_guid_init();
    { static int n; if (n++ < 3) logf_("input: game EnumEffects (type 0x%lX)\n", (unsigned long)type); }
    for (i = 0; i < 7; i++) {
        DIEFFECTINFOA o;
        vfx_info(i, &o);
        if ((type & 0xFF) != DIEFT_ALL && (type & 0xFF) != (o.dwEffType & 0xFF)) continue;
        if (cb(&o, ref) == DIENUM_STOP) break;
    }
    return DI_OK;
}
static HRESULT WINAPI vd_GetEffectInfo(IDirectInputDevice8A *self, LPDIEFFECTINFOA o, REFGUID g)
{
    int i;
    (void)self;
    vfx_guid_init();
    if (!o || !g) return DIERR_INVALIDPARAM;
    for (i = 0; i < 7; i++) if (IsEqualGUID(g, vfx_guids[i])) { vfx_info(i, o); return DI_OK; }
    return DIERR_DEVICENOTREG;
}
static HRESULT WINAPI vd_GetForceFeedbackState(IDirectInputDevice8A *self, LPDWORD st) { static int n; (void)self; if (n++ < 3) logf_("input: game calls GetForceFeedbackState\n"); if (st) *st = 0x50; return DI_OK; }     /* power on, actuators on */
static HRESULT WINAPI vd_SendForceFeedbackCommand(IDirectInputDevice8A *self, DWORD cmd)
{
    int i;
    (void)self;
    { static int n; if (n++ < 6) logf_("input: game calls SendForceFeedbackCommand(%lu)\n", (unsigned long)cmd); }
    if (cmd == 1 || cmd == 2) { for (i = 0; i < VFX_MAX; i++) if (g_vfx[i]) g_vfx[i]->playing = 0; g_vff_paused = 0; }      /* reset / stop all */
    else if (cmd == 4) g_vff_paused = 1;                                                                                 /* pause */
    else if (cmd == 8) g_vff_paused = 0;                                                                                 /* continue */
    vff_update();
    return DI_OK;
}
static HRESULT WINAPI vd_Poll(IDirectInputDevice8A *self) { (void)self; vff_update(); return DI_OK; }

#define VD_LG(fname, ret, ...) static HRESULT WINAPI vdl_##fname(__VA_ARGS__) { static int n; if (n++ < 3) logf_("input: game calls %s\n", #fname); return ret; }
VD_LG(Escape, E_NOTIMPL, void *a, DWORD_PTR b)
VD_LG(SendDeviceData, E_NOTIMPL, void *a, DWORD_PTR b, DWORD_PTR c, DWORD_PTR d, DWORD_PTR e)
VD_LG(Acquire, DI_OK, void *a)
VD_LG(Unacquire, DI_OK, void *a)
VD_LG(SetEventNotification, DI_OK, void *a, DWORD_PTR b)
VD_LG(GetObjectInfo, E_NOTIMPL, void *a, DWORD_PTR b, DWORD_PTR c, DWORD_PTR d)
static HRESULT WINAPI vdl_SetCooperativeLevel(void *a, DWORD_PTR hwnd, DWORD_PTR fl)
{ static int n; (void)a; (void)hwnd; if (n++ < 3) logf_("input: game calls SetCooperativeLevel (flags 0x%lX)\n", (unsigned long)fl); return DI_OK; }

static HRESULT WINAPI vd_GetDeviceData(IDirectInputDevice8A *self, DWORD cbo, LPDIDEVICEOBJECTDATA data, LPDWORD n, DWORD fl)
{ static int c; (void)self; (void)data; if (c++ < 3) logf_("input: game reads buffered data (object size %lu, flags 0x%lX)\n", (unsigned long)cbo, (unsigned long)fl); if (n) *n = 0; return DI_OK; }

static HRESULT WINAPI vd_GetDeviceInfo(IDirectInputDevice8A *self, LPDIDEVICEINSTANCEA i)
{
    (void)self;
    if (!i || i->dwSize < sizeof(DIDEVICEINSTANCEA)) return DIERR_INVALIDPARAM;
    memset(i, 0, sizeof *i); i->dwSize = sizeof *i;
    i->guidInstance = g_vguid_inst; i->guidProduct = g_vguid_prod;
    i->dwDevType = vin_devtype(); i->wUsagePage = 1; i->wUsage = 5;
    strcpy(i->tszInstanceName, "THPS4+ Virtual Gamepad"); strcpy(i->tszProductName, "THPS4+ Virtual Gamepad");
    return DI_OK;
}

/* Methods whose exact signature does not matter to us. The argument COUNT must match the real one (stdcall). */
#define VD_OK(n, ...)   static HRESULT WINAPI vd_ok##n(__VA_ARGS__) { return DI_OK; }
#define VD_NI(n, ...)   static HRESULT WINAPI vd_ni##n(__VA_ARGS__) { return E_NOTIMPL; }
VD_OK(1, void *a)
VD_OK(2, void *a, DWORD_PTR b)
VD_OK(3, void *a, DWORD_PTR b, DWORD_PTR c)
VD_OK(4, void *a, DWORD_PTR b, DWORD_PTR c, DWORD_PTR d)
VD_NI(2, void *a, DWORD_PTR b)
VD_NI(3, void *a, DWORD_PTR b, DWORD_PTR c)
VD_NI(4, void *a, DWORD_PTR b, DWORD_PTR c, DWORD_PTR d)
VD_NI(5, void *a, DWORD_PTR b, DWORD_PTR c, DWORD_PTR d, DWORD_PTR e)

#define VP(f) ((void *)(f))
static IDirectInputDevice8AVtbl g_vd_vtbl;
static int g_vd_vtbl_ready;

static void vd_build_vtbl(void)
{
    void **v = (void **)&g_vd_vtbl;
    int n = (int)(sizeof g_vd_vtbl / sizeof(void *));
    if (g_vd_vtbl_ready) return;
    g_vd_vtbl_ready = 1;
    /* Order of IDirectInputDevice8A (3 IUnknown + 29 own methods = 32). */
    if (n != 32) { logf_("input: unexpected IDirectInputDevice8A vtable size %d\n", n); }
    v[0] = VP(vd_QueryInterface); v[1] = VP(vd_AddRef); v[2] = VP(vd_Release);
    v[3] = VP(vd_GetCapabilities);
    v[4] = VP(vd_EnumObjects);
    v[5] = VP(vd_GetProperty);
    v[6] = VP(vd_SetProperty);
    v[7] = VP(vdl_Acquire);
    v[8] = VP(vdl_Unacquire);
    v[9] = VP(vd_GetDeviceState);
    v[10] = VP(vd_GetDeviceData);           /* (self, cbObj, data, pn, flags) = 5 args */
    v[11] = VP(vd_SetDataFormat);
    v[12] = VP(vdl_SetEventNotification);
    v[13] = VP(vdl_SetCooperativeLevel);
    v[14] = VP(vdl_GetObjectInfo);
    v[15] = VP(vd_GetDeviceInfo);
    v[16] = VP(vd_ok3);                     /* RunControlPanel */
    v[17] = VP(vd_ok4);                     /* Initialize */
    v[18] = VP(vd_CreateEffect);
    v[19] = VP(vd_EnumEffects);
    v[20] = VP(vd_GetEffectInfo);
    v[21] = VP(vd_GetForceFeedbackState);
    v[22] = VP(vd_SendForceFeedbackCommand);
    v[23] = VP(vd_ok4);                     /* EnumCreatedEffectObjects */
    v[24] = VP(vdl_Escape);
    v[25] = VP(vd_Poll);
    v[26] = VP(vdl_SendDeviceData);
    v[27] = VP(vd_ni5);                     /* EnumEffectsInFile */
    v[28] = VP(vd_ni5);                     /* WriteEffectToFile */
    v[29] = VP(vd_ni4);                     /* BuildActionMap */
    v[30] = VP(vd_ni4);                     /* SetActionMap */
    v[31] = VP(vd_ni2);                     /* GetImageInfo */
}

static IDirectInputDevice8A *vd_create(void)
{
    VDev *d = (VDev *)calloc(1, sizeof *d);
    int i;
    if (!d) return NULL;
    vd_build_vtbl();
    d->lpVtbl = &g_vd_vtbl; d->ref = 1;
    for (i = 0; i < 6; i++) { d->rmin[i] = 0; d->rmax[i] = 65535; }
    return (IDirectInputDevice8A *)d;
}

/* ------------------------------------------------------------ hooking IDirectInput8A */
typedef HRESULT (WINAPI *VPFN_Create)(IDirectInput8A *, REFGUID, LPDIRECTINPUTDEVICE8A *, LPUNKNOWN);
typedef HRESULT (WINAPI *VPFN_Enum)(IDirectInput8A *, DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD);
typedef HRESULT (WINAPI *VPFN_DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
static VPFN_Create vo_CreateDevice;
static VPFN_Enum vo_EnumDevices;
static VPFN_DI8Create vo_DI8Create;

static int vin_wanted(void)
{
    VPad p;
    if (!g_in_enable || g_in_backend < 0) return 0;
    if (g_in_always) return 1;
    return vin_poll(&p) && p.ok;
}

static HRESULT WINAPI vh_CreateDevice(IDirectInput8A *self, REFGUID g, LPDIRECTINPUTDEVICE8A *pp, LPUNKNOWN outer)
{
    if (pp && g && IsEqualGUID(g, &g_vguid_inst)) {
        *pp = vd_create();
        logf_("input: virtual gamepad created for the game\n");
        return *pp ? DI_OK : E_OUTOFMEMORY;
    }
    { static int n; if (g && n++ < 8) logf_("input: game creates device %08lX-%04X-%04X\n", (unsigned long)g->Data1, g->Data2, g->Data3); }
    return vo_CreateDevice(self, g, pp, outer);
}

typedef struct { LPDIENUMDEVICESCALLBACKA cb; LPVOID ref; int stop, n; } VEnumCtx;
static BOOL CALLBACK vin_enum_filter(LPCDIDEVICEINSTANCEA inst, LPVOID ref)      /* real devices: log them, hide the pads if asked to */
{
    VEnumCtx *c = (VEnumCtx *)ref;
    DWORD t = inst->dwDevType & 0xFF;
    BOOL r = DIENUM_CONTINUE;
    c->n++;
    int pad = (t != DI8DEVTYPE_KEYBOARD && t != DI8DEVTYPE_MOUSE);
    if (!pad || !g_in_hide) {
        r = c->cb(inst, c->ref);
        if (r == DIENUM_STOP) c->stop = 1;
    }
    { static int n; if (n++ < 8) logf_("input: real device '%s' type 0x%lX -> game callback %s\n", inst->tszInstanceName, (unsigned long)inst->dwDevType, (pad && g_in_hide) ? "not asked (hidden)" : (r ? "returned continue" : "returned STOP")); }
    return r;
}

static BOOL vin_offer_virtual(LPDIENUMDEVICESCALLBACKA cb, LPVOID ref)
{
    DIDEVICEINSTANCEA i;
    memset(&i, 0, sizeof i); i.dwSize = sizeof i;
    i.guidInstance = g_vguid_inst; i.guidProduct = g_vguid_prod;
    i.dwDevType = vin_devtype(); i.wUsagePage = 1; i.wUsage = 5;
    strcpy(i.tszInstanceName, "THPS4+ Virtual Gamepad"); strcpy(i.tszProductName, "THPS4+ Virtual Gamepad");
    { BOOL r = cb(&i, ref); logf_("input: game callback returned %d for the virtual pad (type 0x%lX)\n", (int)r, (unsigned long)i.dwDevType); return r; }
}

static HRESULT WINAPI vh_EnumDevices(IDirectInput8A *self, DWORD cls, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD fl)
{
    if (cb && (cls == DI8DEVCLASS_GAMECTRL || cls == DI8DEVCLASS_ALL) && vin_wanted()) {
        HRESULT hr = DI_OK;
        if (cls == DI8DEVCLASS_ALL) {
            VEnumCtx c; c.cb = cb; c.ref = ref; c.stop = 0; c.n = 0;
            hr = vo_EnumDevices(self, cls, vin_enum_filter, &c, fl);
            logf_("input: game EnumDevices class %lu flags 0x%lX: real enumeration hr=0x%08lX, %d device(s) listed\n", (unsigned long)cls, (unsigned long)fl, (unsigned long)hr, c.n);
            if (c.stop) { logf_("input: game stopped the enumeration before the virtual pad\n"); return hr; }
        }
        logf_("input: offering the virtual gamepad (class %lu, flags 0x%lX)\n", (unsigned long)cls, (unsigned long)fl);
        vin_offer_virtual(cb, ref);
        return hr;
    }
    return vo_EnumDevices(self, cls, cb, ref, fl);
}

static HRESULT WINAPI vh_DI8Create(HINSTANCE hi, DWORD ver, REFIID riid, LPVOID *ppv, LPUNKNOWN outer)
{
    HRESULT hr = vo_DI8Create(hi, ver, riid, ppv, outer);
    if (SUCCEEDED(hr) && ppv && *ppv && riid && IsEqualGUID(riid, &IID_IDirectInput8A)) {
        IDirectInput8A *o = (IDirectInput8A *)*ppv;
        IDirectInput8AVtbl *nv = (IDirectInput8AVtbl *)malloc(sizeof *nv);
        if (nv) {
            *nv = *o->lpVtbl;
            if (!vo_CreateDevice) vo_CreateDevice = nv->CreateDevice;
            if (!vo_EnumDevices) vo_EnumDevices = nv->EnumDevices;
            nv->CreateDevice = vh_CreateDevice;
            nv->EnumDevices = vh_EnumDevices;
            o->lpVtbl = nv;
            logf_("input: DirectInput8 object hooked\n");
        }
    } else if (SUCCEEDED(hr)) {
        logf_("input: DirectInput8Create asked for an interface we do not wrap (the Unicode one?)\n");
    }
    return hr;
}


/* ------------------------------------------------------------ diagnostics: does the game use the winmm joystick API? */
typedef UINT (WINAPI *VW_NumDevs)(void);
typedef MMRESULT (WINAPI *VW_Caps)(UINT_PTR, LPJOYCAPSA, UINT);
typedef MMRESULT (WINAPI *VW_Pos)(UINT, LPJOYINFO);
typedef MMRESULT (WINAPI *VW_PosEx)(UINT, LPJOYINFOEX);
static VW_NumDevs vw_NumDevs; static VW_Caps vw_Caps; static VW_Pos vw_Pos; static VW_PosEx vw_PosEx;
static int vw_cnt[4];

static UINT WINAPI vwh_NumDevs(void)
{ UINT r = vw_NumDevs(); if (vw_cnt[0]++ < 3) logf_("input: game calls joyGetNumDevs -> %u\n", r); return r; }
static MMRESULT WINAPI vwh_Caps(UINT_PTR id, LPJOYCAPSA c, UINT cb)
{ MMRESULT r = vw_Caps(id, c, cb); if (vw_cnt[1]++ < 3) logf_("input: game calls joyGetDevCapsA(%u) -> %u\n", (unsigned)id, (unsigned)r); return r; }
static MMRESULT WINAPI vwh_Pos(UINT id, LPJOYINFO p)
{ MMRESULT r = vw_Pos(id, p); if (vw_cnt[2]++ < 3) logf_("input: game calls joyGetPos(%u) -> %u\n", id, (unsigned)r); return r; }
static MMRESULT WINAPI vwh_PosEx(UINT id, LPJOYINFOEX p)
{ MMRESULT r = vw_PosEx(id, p); if (vw_cnt[3]++ < 3) logf_("input: game calls joyGetPosEx(%u) -> %u\n", id, (unsigned)r); return r; }

static int vin_patch_import(const char *dll, const char *func, void *newf, void **orig)
{
    HMODULE m = GetModuleHandleA(NULL), lib;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)m + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d;
    if (!dd->VirtualAddress) return 0;
    lib = GetModuleHandleA(dll); if (!lib) return 0;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)m + dd->VirtualAddress); d->Name; d++) {
        IMAGE_THUNK_DATA *oft, *ft;
        if (lstrcmpiA((const char *)((BYTE *)m + d->Name), dll) != 0) continue;
        oft = (IMAGE_THUNK_DATA *)((BYTE *)m + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        ft = (IMAGE_THUNK_DATA *)((BYTE *)m + d->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            if (strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)((BYTE *)m + oft->u1.AddressOfData))->Name, func) != 0) continue;
            if ((void *)ft->u1.Function == newf) return 1;
            *orig = (void *)GetProcAddress(lib, func);
            if (!*orig) return 0;
            if (VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                ft->u1.Function = (ULONG_PTR)newf;
                VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

static void vin_trace_winmm(void)
{
    static int done;
    int a, b, c, d;
    if (done || g_vin_quiet) return;
    done = 1;
    a = vin_patch_import("winmm.dll", "joyGetNumDevs", (void *)vwh_NumDevs, (void **)&vw_NumDevs);
    b = vin_patch_import("winmm.dll", "joyGetDevCapsA", (void *)vwh_Caps, (void **)&vw_Caps);
    c = vin_patch_import("winmm.dll", "joyGetPos", (void *)vwh_Pos, (void **)&vw_Pos);
    d = vin_patch_import("winmm.dll", "joyGetPosEx", (void *)vwh_PosEx, (void **)&vw_PosEx);
    logf_("input: winmm joystick imports traced: NumDevs %d, DevCaps %d, Pos %d, PosEx %d (0 = the exe does not import it)\n", a, b, c, d);
}

/* Redirect the exe's import of DirectInput8Create. Safe to call more than once. */
static void vin_install(void)
{
    HMODULE m, di;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dd;
    IMAGE_IMPORT_DESCRIPTOR *d;
    if (!g_in_enable || g_in_backend < 0) return;
    vin_trace_winmm();
    VLOG("input: virtual pad instance GUID %08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X (from %s)\n", (unsigned long)g_vguid_inst.Data1, g_vguid_inst.Data2, g_vguid_inst.Data3, g_vguid_inst.Data4[0], g_vguid_inst.Data4[1], g_vguid_inst.Data4[2], g_vguid_inst.Data4[3], g_vguid_inst.Data4[4], g_vguid_inst.Data4[5], g_vguid_inst.Data4[6], g_vguid_inst.Data4[7], g_vguid_src);
    if (!vo_DI8Create) {
        di = GetModuleHandleA("dinput8.dll"); if (!di) di = LoadLibraryA("dinput8.dll");
        vo_DI8Create = di ? (VPFN_DI8Create)GetProcAddress(di, "DirectInput8Create") : NULL;
        if (!vo_DI8Create) { VLOG("input: dinput8.dll not found\n"); return; }
    }
    m = GetModuleHandleA(NULL);
    dos = (IMAGE_DOS_HEADER *)m;
    nt = (IMAGE_NT_HEADERS *)((BYTE *)m + dos->e_lfanew);
    dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)m + dd->VirtualAddress); d->Name; d++) {
        IMAGE_THUNK_DATA *oft, *ft;
        if (lstrcmpiA((const char *)((BYTE *)m + d->Name), "dinput8.dll") != 0) continue;
        oft = (IMAGE_THUNK_DATA *)((BYTE *)m + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        ft = (IMAGE_THUNK_DATA *)((BYTE *)m + d->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            IMAGE_IMPORT_BY_NAME *bn;
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            bn = (IMAGE_IMPORT_BY_NAME *)((BYTE *)m + oft->u1.AddressOfData);
            if (strcmp((const char *)bn->Name, "DirectInput8Create") != 0) continue;
            if ((void *)ft->u1.Function == (void *)vh_DI8Create) { VLOG("input: DirectInput8Create import is redirected\n"); return; }
            if (VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                ft->u1.Function = (ULONG_PTR)vh_DI8Create;
                VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
                VLOG("input: DirectInput8Create import redirected\n");
            }
            return;
        }
    }
    VLOG("input: this exe does not import DirectInput8Create by name\n");
}
#endif
