/*
 * d3d8_thps4plus.c - THPS4+ proxy d3d8.dll (stage 1: diagnostics)
 *
 *   F6 (configurable)  dump every texture used in the next frame as PNG
 *   F9 (configurable)  dump every 2D (pre-transformed) draw of the next frame
 *   Widescreen (THPS4Plus.ini): real backbuffer size, Hor+ FOV, edge-anchored HUD
 *
 * Output goes to <game folder>\THPS4Plus\dump\ . Settings: THPS4Plus.ini
 * next to this dll. The game itself is not modified in any way.
 *
 * Method: call the real d3d8.dll, then give the returned objects a private
 * copy of their vtable with a few entries redirected to us.
 * Build in the MSYS2 MINGW32 shell with build.sh. This is C, not C++.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <d3d8.h>
#include <ctype.h>
#include <mmsystem.h>
#include "texdump_core.h"
#include "texload_core.h"

#define EXPORT __declspec(dllexport)
#define MAXTEX 4096

static HINSTANCE g_self;
static HMODULE g_real;
static char g_base[MAX_PATH];      /* folder of this dll, trailing backslash */
static char g_dumpdir[MAX_PATH];   /* ...\THPS4Plus\dump\ */
static char g_texdir[MAX_PATH];    /* ...\THPS4Plus\dump\textures\ */
static int g_vk_hud = VK_F9, g_vk_tex = VK_F6;
static int g_inited;
static int g_import_enable = 1;                          /* texture import on/off */
static unsigned long long g_glyph_hash = 0x09D4AA800A5B227EULL;   /* the button-glyph sheet */
static int g_ctrl_force = -1;                            /* -1 = automatic, else 0 keyboard / 1..4 controller type */
static int g_ctrl_detect = 1, g_ctrl_backend;            /* backend: 0 auto, 1 SDL2, 2 DirectInput-era (winmm), 3 XInput */
static float g_cull_k = 1.0f;            /* how much wider the engine's culling frustum is made */
static int g_cull_enable = 1, g_cull_done;
static DWORD g_cull_site = 0x0044216F;
static int g_fps_limit = 0;              /* frames per second cap, 0 = off (default) */
static int g_hud_frames = 1, g_hud_left; /* how many consecutive frames one F9 press dumps */

/* culling probe (diagnostic): finds the engine's aspect / FOV variables */
static int g_probe_enable, g_vk_probe = VK_F7;
static int g_viewdist_pct = 100, g_viewdist_done;      /* [Culling] ViewDistancePercent */
static unsigned long long g_load_hash[32];
static int g_nload;
static float g_load_aspect = 16.0f / 9.0f;               /* [Widescreen] LoadingTextures / LoadingAspect */
static int g_ov_windowed, g_ov_ms = -1, g_ov_bb = -1, g_ov_swap = -1;    /* [Display] Windowed, Multisample, BackBuffers, SwapEffect (experiments) */
static float g_seq[8];
static int g_nseq, g_seq_pct = 100;                 /* [Culling] PatchSeq / PatchSeqPercent (Probe=4) */
static float g_scanvals[4];
static int g_nscanvals;
static float g_raw_m11, g_raw_m22;       /* first perspective projection of the frame, exactly as the game sent it */
static int g_raw_set;
typedef struct { float *addr; float orig; char kind; char region; char confirmed; } ProbeCand;
static ProbeCand g_pc[200];
static int g_npc, g_pci, g_pstate, g_pphase, g_pwait, g_pconfirmed;
static float g_pb11, g_pb22;
static DWORD g_watchcfg[2];
static int g_nwatchcfg;

/* widescreen state */
static int g_ws_enable = 1, g_ws_active, g_hud_anchor = 1, g_fov_fix = 1, g_remap_hud = 1;
static UINT g_W = 1920, g_H = 1080;     /* what we give the game */
static UINT g_vw = 640, g_vh = 480;     /* the fixed virtual HUD layout space (640x480) */
static UINT g_rw = 1920, g_rh = 1080;   /* the screen size the game lays its 2D out for (= the real backbuffer) */
static UINT g_ask_w = 640, g_ask_h = 480; /* what the game last asked D3D for (used for render-target sizes) */
enum { MK_NONE = 0, MK_GENERIC = 1, MK_DIALOG = 2, MK_PAUSE = 3, MK_KEYBOARD = 4 };
enum { HP_ANCHORED = 0, HP_HIDDEN = 1, HP_TUCKED = 2 };
static int g_menu_cur, g_menu_next;     /* MK_* kind of menu seen: last frame / frame being drawn */
static int g_hud_policy[5] = { 0, HP_ANCHORED, HP_ANCHORED, HP_HIDDEN, HP_HIDDEN };
static unsigned long long g_kindhash[5][12];
static int g_nkind[5];
/* Explicit placement rules: which piece of UI sticks to which side. Anything not matched stays centred. */
typedef struct { unsigned long long h; int box; float x0, y0, x1, y1; int side; int hud; unsigned long long req; int maxseen; int nomenu; } AnchorRule;   /* side: 0 left, 1 centre, 2 right */
static AnchorRule g_rules[48];
static int g_nrules;
static float g_inset_x, g_inset_y;
static unsigned long long g_seen_cur[96], g_seen_prev[96];     /* textures drawn in 2D this frame / last frame */
static int g_nseen_cur, g_nseen_prev;
typedef struct { float x0, y0, x1, y1; int valid; } DRect;      /* bounding box of a dialog box (layout units) */
static DRect g_drect_cur, g_drect_prev;
typedef struct { float x0, x1, y0, y1; } CPiece;                /* a centred text piece drawn this frame */
static CPiece g_cp[128];
static int g_ncp;
static int g_split;                                             /* this frame has the two half-screen viewports */
static int g_rules_in_menu[5] = { 0, 1, 1, 1, 1 };               /* apply the placement rules while this menu kind is open */      /* optional extra margin for the score/cash HUD (640x480 units) */
typedef struct { void *p; int valid; unsigned long long h; } TexCache;
static TexCache g_tc[1024];
static int g_proj_logged, g_depth_logged, g_fog_logged, g_far_pct = 100;
static DWORD g_fog_last[160];                       /* last logged value per render state, for the fog diagnostics */
static int g_fog_seen[160];
static int g_rt_main = 1;               /* is the current render target the main screen? */
static int g_ws_logged;
static unsigned char *g_vbuf;
static size_t g_vcap;
static char g_realname[MAX_PATH];   /* optional: file name of the d3d8 dll to chain to (e.g. dgVoodoo) */

static FILE *g_log;
static FILE *g_dump;
static int g_dumping, g_draw_no, g_hud_no;
static DWORD g_fvf;
static IDirect3DBaseTexture8 *g_tex0;

static IDirect3DBaseTexture8 *g_texset[MAXTEX];
static int g_ntex, g_texcollect;
static unsigned char g_texhud[MAXTEX];                  /* 1 = this queued texture was drawn as 2D (HUD) */
static char g_ini[MAX_PATH];                            /* THPS4Plus.ini, for the live reload key */
static int g_vk_all = VK_F10, g_vk_reload = VK_F5, g_collect_all;
static char g_group[64] = "Level";                      /* [Debug] Group: folder name for the 3D textures of collect-all */
static char g_dir_hud[MAX_PATH], g_dir_group[MAX_PATH];
#define MAXSEEN 16384
static IDirect3DBaseTexture8 *g_seen_tex[MAXSEEN];      /* textures already queued during this collect-all session */
static int g_nseen_tex, g_all_new;

static IDirect3D8Vtbl g_d3dVt, g_d3dOrig;
static IDirect3DDevice8Vtbl g_devVt, g_devOrig;
static int g_clear_black = 0;   /* set when a listed loading picture is drawn narrower than the screen: clear the sides to black first */
static int g_d3dPatched, g_devPatched;

/* ------------------------------------------------------------------ setup */

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%sTHPS4Plus\\hook.log", g_base);
        g_log = fopen(p, "w");
    }
    if (!g_log) return;
    va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fflush(g_log);
}

static int parse_key(const char *s, int def)
{
    int n;
    if (!s || !*s) return def;
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '0' && s[1] <= '9') {
        n = atoi(s + 1);
        if (n >= 1 && n <= 12) return VK_F1 + n - 1;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (int)strtol(s, NULL, 16);
    if (!s[1] && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
    if (!s[1] && s[0] >= 'a' && s[0] <= 'z') return s[0] - 'a' + 'A';
    return def;
}

static void parse_hashes(const char *src, unsigned long long *arr, int *n, int max)
{
    char buf[512], *tok;
    strncpy(buf, src, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    *n = 0;
    for (tok = strtok(buf, ", "); tok && *n < max; tok = strtok(NULL, ", "))
        arr[(*n)++] = strtoull(tok, NULL, 16);
}

static int streq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return *a == *b;
}

static int parse_policy(const char *s, int def)
{
    if (streq_ci(s, "hidden")) return 1;     /* HP_HIDDEN   */
    if (streq_ci(s, "anchored")) return 0;   /* HP_ANCHORED */
    if (streq_ci(s, "tucked")) return 2;     /* HP_TUCKED   */
    return def;
}

static int parse_side(const char *s)
{
    if (streq_ci(s, "left")) return 0;
    if (streq_ci(s, "right")) return 2;
    return 1;
}

static void add_texture_rules(const char *list, int side)
{
    unsigned long long arr[16];
    int n = 0, i;
    parse_hashes(list, arr, &n, 16);
    for (i = 0; i < n && g_nrules < 48; i++) {
        AnchorRule *r = &g_rules[g_nrules++];
        r->h = arr[i]; r->box = 0; r->x0 = r->y0 = r->x1 = r->y1 = 0; r->side = side; r->hud = 1; r->req = 0;
    }
}

static void load_anchor_rules(const char *ini)
{
    char key[16], val[160], hs[40], sd[16];
    static const char *defrule[10] = {
        "",
        "A2CF8279019ABFA7,100,20,340,60,left,1,8394163FF34108B5",
        "A2CF8279019ABFA7,380,20,640,70,right,1,FCB17C018F4CC522",
        "A2CF8279019ABFA7,380,20,640,95,right,0",
        "A2CF8279019ABFA7,380,80,640,385,right,0",
        "09D4AA800A5B227E,280,80,640,385,right,0",
        "27A32B419712CC70,380,80,640,385,right,0",
        "8555A6E401CED096,380,80,640,385,right,0",
        "A2CF8279019ABFA7,20,340,200,400,left,0,8394163FF34108B5",
        "A2CF8279019ABFA7,20,60,230,335,left,0,8394163FF34108B5" };
    int i;
    g_nrules = 0;
    GetPrivateProfileStringA("Anchors", "LeftTextures",
        "8394163FF34108B5,37F7B338CC1A06CD,CA037E82868244D7,E1C6241B52A16B30,58996F233E19A2C1,CF103A4F94F52DBD",
        val, sizeof val, ini);
    add_texture_rules(val, 0);
    GetPrivateProfileStringA("Anchors", "RightTextures", "FCB17C018F4CC522", val, sizeof val, ini);
    add_texture_rules(val, 2);
    for (i = 1; i <= 16 && g_nrules < 48; i++) {
        float x0, y0, x1, y1;
        char rq[40];
        int hud = 1, n, maxs, nom;
        snprintf(key, sizeof key, "Rule%d", i);
        GetPrivateProfileStringA("Anchors", key, i < 10 ? defrule[i] : "", val, sizeof val, ini);
        if (!val[0]) continue;
        rq[0] = 0; maxs = 0; nom = 0;
        n = sscanf(val, " %16[0-9A-Fa-f] , %f , %f , %f , %f , %15[A-Za-z] , %d , %16[0-9A-Fa-f] , %d , %d", hs, &x0, &y0, &x1, &y1, sd, &hud, rq, &maxs, &nom);
        if (n >= 6) {
            AnchorRule *r = &g_rules[g_nrules++];
            r->h = strtoull(hs, NULL, 16); r->box = 1; r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1;
            r->side = parse_side(sd); r->hud = (n >= 7) ? hud : 1; r->req = (n >= 8) ? strtoull(rq, NULL, 16) : 0; r->maxseen = (n >= 9) ? maxs : 0; r->nomenu = (n >= 10) ? nom : 0;
        }
    }
    g_inset_x = (float)GetPrivateProfileIntA("Anchors", "HudInsetX", 0, ini);
    g_inset_y = (float)GetPrivateProfileIntA("Anchors", "HudInsetY", 0, ini);
    logf_("anchor rules loaded: %d\n", g_nrules);
}

static void import_scan(void);

#include "dinput_virtual.h"
#include "dsound_trace.h"

static void init_once(void)
{
    char ini[MAX_PATH], buf[64], *slash, tmp[MAX_PATH];
    if (g_inited) return;
    g_inited = 1;

    GetModuleFileNameA(g_self, g_base, MAX_PATH);
    (void)0;
    slash = strrchr(g_base, '\\');
    if (slash) slash[1] = 0;

    snprintf(tmp, sizeof tmp, "%sTHPS4Plus", g_base);            CreateDirectoryA(tmp, NULL);
    snprintf(tmp, sizeof tmp, "%sTHPS4Plus\\dump", g_base);      CreateDirectoryA(tmp, NULL);
    snprintf(g_dumpdir, sizeof g_dumpdir, "%sTHPS4Plus\\dump\\", g_base);
    snprintf(g_texdir, sizeof g_texdir, "%stextures", g_dumpdir); CreateDirectoryA(g_texdir, NULL);
    strcat(g_texdir, "\\");

    snprintf(ini, sizeof ini, "%sTHPS4Plus.ini", g_base);
    strcpy(g_ini, ini);
    GetPrivateProfileStringA("Hotkeys", "DumpTextures", "F6", buf, sizeof buf, ini);
    g_vk_tex = parse_key(buf, VK_F6);
    GetPrivateProfileStringA("Hotkeys", "DumpAllTextures", "F10", buf, sizeof buf, ini);
    g_vk_all = parse_key(buf, VK_F10);
    GetPrivateProfileStringA("Hotkeys", "ReloadIni", "F5", buf, sizeof buf, ini);
    g_vk_reload = parse_key(buf, VK_F5);
    GetPrivateProfileStringA("Hotkeys", "DumpHud", "F9", buf, sizeof buf, ini);
    g_vk_hud = parse_key(buf, VK_F9);
    g_ws_enable  = (int)GetPrivateProfileIntA("Widescreen", "Enable", 1, ini);
    g_hud_anchor = (int)GetPrivateProfileIntA("Widescreen", "HudAnchor", 1, ini);
    g_fov_fix    = (int)GetPrivateProfileIntA("Widescreen", "FovFix", 1, ini);
    { char lbuf[1024];
      GetPrivateProfileStringA("Widescreen", "LoadingTextures", "28476833231B7CEF", lbuf, sizeof lbuf, ini);
      g_nload = 0; parse_hashes(lbuf, g_load_hash, &g_nload, 30); }
    GetPrivateProfileStringA("Widescreen", "LoadingAspect", "16:9", buf, sizeof buf, ini);
    { float a = 0.0f, b = 0.0f; if (sscanf(buf, " %f : %f", &a, &b) == 2 && b > 0.0f) g_load_aspect = a / b; else if (sscanf(buf, " %f", &a) == 1 && a > 0.5f) g_load_aspect = a; else g_load_aspect = 16.0f / 9.0f; }
    g_ov_windowed = (int)GetPrivateProfileIntA("Display", "Windowed", 0, ini);
    g_ov_ms       = (int)GetPrivateProfileIntA("Display", "Multisample", -1, ini);
    g_ov_bb       = (int)GetPrivateProfileIntA("Display", "BackBuffers", -1, ini);
    g_ov_swap     = (int)GetPrivateProfileIntA("Display", "SwapEffect", -1, ini);
    g_far_pct    = (int)GetPrivateProfileIntA("Render", "FarScalePercent", 100, ini);   /* 100 = off; stretches the far clip plane (experiment) */
    if (g_far_pct < 100) g_far_pct = 100;
    g_remap_hud  = (int)GetPrivateProfileIntA("Widescreen", "RemapHud", 1, ini);
    g_vw = GetPrivateProfileIntA("Widescreen", "VirtualWidth", 640, ini);
    g_vh = GetPrivateProfileIntA("Widescreen", "VirtualHeight", 480, ini);
    if (!g_vw || !g_vh) { g_vw = 640; g_vh = 480; }
    g_W = GetPrivateProfileIntA("Display", "Width", 1920, ini);
    g_H = GetPrivateProfileIntA("Display", "Height", 1080, ini);
    if (!g_W || !g_H) {
        typedef BOOL (WINAPI *SDA)(void);
        HMODULE u32 = GetModuleHandleA("user32.dll");
        SDA sda = u32 ? (SDA)GetProcAddress(u32, "SetProcessDPIAware") : NULL;
        if (sda) sda();                      /* otherwise a 125% scaled desktop reports a smaller size */
        g_W = (UINT)GetSystemMetrics(SM_CXSCREEN); g_H = (UINT)GetSystemMetrics(SM_CYSCREEN);
    }
    {
        char list[512];
        static const char *kn[5] = { "", "GenericTextures", "DialogTextures", "PauseTextures", "KeyboardTextures" };
        static const char *kd[5] = { "", "46919C68E672BCA6",
            "8359A7261B70AB17,E115836C730D4A41,A48631B488B7E9F5,361D75E1D398621C,5708E8F8447D3B2B,FA0E6645AAEE1EB3",
            "EFE2536CD53A2079", "78F3151C1456619A" };
        static const char *pn[5] = { "", "HudInMenus", "HudInDialogs", "HudInPause", "HudInKeyboard" };
        static const char *pd[5] = { "", "anchored", "anchored", "hidden", "hidden" };
        int k;
        for (k = 1; k <= 4; k++) {
            GetPrivateProfileStringA("Menus", kn[k], kd[k], list, sizeof list, ini);
            parse_hashes(list, g_kindhash[k], &g_nkind[k], 12);
            GetPrivateProfileStringA("Menus", pn[k], pd[k], list, sizeof list, ini);
            g_hud_policy[k] = parse_policy(list, k >= 3 ? HP_HIDDEN : HP_ANCHORED);
        }
        load_anchor_rules(ini);
        {
            int rv = (int)GetPrivateProfileIntA("Menus", "RulesInMenus", 1, ini), kk;
            for (kk = 1; kk <= 4; kk++) g_rules_in_menu[kk] = rv;
        }
    }
    logf_("THPS4+ build %s %s\n", __DATE__, __TIME__);
    g_fps_limit = (int)GetPrivateProfileIntA("Display", "FrameLimit", 0, ini);
    g_hud_frames = (int)GetPrivateProfileIntA("Dump", "HudFrames", 1, ini);
    if (g_hud_frames < 1) g_hud_frames = 1;
    if (g_hud_frames > 12) g_hud_frames = 12;
    g_cull_enable = (int)GetPrivateProfileIntA("Culling", "CullFix", 1, ini);
    GetPrivateProfileStringA("Debug", "Group", "Level", g_group, sizeof g_group, ini);
    { char *q; for (q = g_group; *q; q++) if (!((*q >= '0' && *q <= '9') || (*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || *q == '_' || *q == '-')) *q = '_'; if (!g_group[0]) strcpy(g_group, "Level"); }
    g_viewdist_pct = (int)GetPrivateProfileIntA("Culling", "ViewDistancePercent", 100, ini);
    GetPrivateProfileStringA("Culling", "CullPatchAddress", "0044216F", buf, sizeof buf, ini);
    g_cull_site = (DWORD)strtoul(buf, NULL, 16);
    g_probe_enable = (int)GetPrivateProfileIntA("Culling", "Probe", 0, ini);
    g_seq_pct = (int)GetPrivateProfileIntA("Culling", "PatchSeqPercent", 100, ini);
    GetPrivateProfileStringA("Culling", "PatchSeq", "1200,700,900,900,1100", buf, sizeof buf, ini);
    { char *c = buf; g_nseq = 0; while (*c && g_nseq < 8) { g_seq[g_nseq++] = (float)atof(c); c = strchr(c, ','); if (!c) break; c++; } }
    GetPrivateProfileStringA("Culling", "ScanValues", "900,1764", buf, sizeof buf, ini);
    { char *c = buf; g_nscanvals = 0; while (*c && g_nscanvals < 4) { g_scanvals[g_nscanvals++] = (float)atof(c); c = strchr(c, ','); if (!c) break; c++; } }
    GetPrivateProfileStringA("Culling", "ProbeKey", "F7", buf, sizeof buf, ini);
    g_vk_probe = parse_key(buf, VK_F7);
    GetPrivateProfileStringA("Culling", "WatchAddresses", "00A72A8C", buf, sizeof buf, ini);
    {
        char b2[64], *tok;
        strncpy(b2, buf, sizeof b2 - 1); b2[sizeof b2 - 1] = 0;
        g_nwatchcfg = 0;
        for (tok = strtok(b2, ", "); tok && g_nwatchcfg < 2; tok = strtok(NULL, ", "))
            g_watchcfg[g_nwatchcfg++] = (DWORD)strtoul(tok, NULL, 16);
    }
    g_import_enable = (int)GetPrivateProfileIntA("Import", "Enable", 1, ini);
    GetPrivateProfileStringA("Import", "GlyphTexture", "09D4AA800A5B227E", buf, sizeof buf, ini);
    g_glyph_hash = strtoull(buf, NULL, 16);
    g_ctrl_detect = (int)GetPrivateProfileIntA("Controller", "Detect", 1, ini);
    GetPrivateProfileStringA("Controller", "Backend", "auto", buf, sizeof buf, ini);
    g_ctrl_backend = streq_ci(buf, "sdl") ? 1 : streq_ci(buf, "dinput") ? 2 : streq_ci(buf, "xinput") ? 3 : 0;
    GetPrivateProfileStringA("Controller", "ForceType", "auto", buf, sizeof buf, ini);
    g_ctrl_force = streq_ci(buf, "keyboard") || streq_ci(buf, "none") ? 0 : streq_ci(buf, "xbox") ? 1 :
                   streq_ci(buf, "playstation") ? 2 : streq_ci(buf, "nintendo") ? 3 : streq_ci(buf, "generic") ? 4 : -1;
    vin_config(ini);
    dsound_config(ini);
    g_vin_quiet = 0;
    vin_install();
    dsound_install();                                       /* no-op if DllMain already redirected it; logs the state */
    snprintf(tmp, sizeof tmp, "%sTHPS4Plus\\import", g_base);                      CreateDirectoryA(tmp, NULL);
    snprintf(tmp, sizeof tmp, "%sTHPS4Plus\\import\\textures", g_base);          CreateDirectoryA(tmp, NULL);
    snprintf(tmp, sizeof tmp, "%sTHPS4Plus\\import\\textures\\custom", g_base);     CreateDirectoryA(tmp, NULL);
    snprintf(tmp, sizeof tmp, "%sTHPS4Plus\\import\\textures\\controller", g_base); CreateDirectoryA(tmp, NULL);
    import_scan();
    GetPrivateProfileStringA("Compat", "RealD3D8", "", g_realname, sizeof g_realname, ini);
    logf_("THPS4+ loaded. DumpTextures key=0x%02X, DumpHud key=0x%02X, RealD3D8='%s'\n", g_vk_tex, g_vk_hud, g_realname);
}

/* ------------------------------------------------------------ texture dump */

static void tex_collect(IDirect3DBaseTexture8 *t)
{
    int i;
    for (i = 0; i < g_ntex; i++) if (g_texset[i] == t) return;
    if (g_ntex >= MAXTEX) return;
    t->lpVtbl->AddRef(t);
    g_texhud[g_ntex] = 0;
    g_texset[g_ntex++] = t;
}

static void dump_textures(int all)
{
    int i, saved = 0, existed = 0, unreadable = 0, unsupported = 0, other = 0;
    for (i = 0; i < g_ntex; i++) {
        IDirect3DBaseTexture8 *b = g_texset[i];
        IDirect3DTexture8 *t;
        D3DSURFACE_DESC d;
        D3DLOCKED_RECT lr;
        unsigned long long hsh;
        char name[MAX_PATH], fn[32], path[MAX_PATH];

        if (b->lpVtbl->GetType(b) != D3DRTYPE_TEXTURE) { other++; b->lpVtbl->Release(b); continue; }
        t = (IDirect3DTexture8 *)b;
        if (FAILED(t->lpVtbl->GetLevelDesc(t, 0, &d))) { unreadable++; b->lpVtbl->Release(b); continue; }
        if (FAILED(t->lpVtbl->LockRect(t, 0, &lr, NULL, D3DLOCK_READONLY))) {
            unreadable++;                                   /* render targets etc. */
            b->lpVtbl->Release(b);
            continue;
        }
        if (!td_hash((unsigned)d.Format, (const unsigned char *)lr.pBits, lr.Pitch, d.Width, d.Height, &hsh)) {
            unsupported++;
            logf_("unsupported format %d (%ux%u)\n", (int)d.Format, d.Width, d.Height);
        } else {
            snprintf(name, sizeof name, "%08lX%08lX_%ux%u_%s.png",
                     (unsigned long)(hsh >> 32), (unsigned long)(hsh & 0xFFFFFFFFUL),
                     d.Width, d.Height, td_fmt_name((unsigned)d.Format, fn, sizeof fn));
            snprintf(path, sizeof path, "%s%s", all ? (g_texhud[i] ? g_dir_hud : g_dir_group) : g_texdir, name);
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
                existed++;
            } else {
                unsigned char *rgba = td_decode((unsigned)d.Format, (const unsigned char *)lr.pBits,
                                                lr.Pitch, d.Width, d.Height);
                if (rgba && td_write_png(path, rgba, d.Width, d.Height)) saved++;
                else unsupported++;
                free(rgba);
            }
        }
        t->lpVtbl->UnlockRect(t, 0);
        b->lpVtbl->Release(b);
    }
    if (!all || saved)
        logf_("texture dump%s: %d textures, %d new PNGs, %d already dumped, %d unreadable, %d unsupported, %d non-2D\n",
              all ? " (collect-all)" : "", g_ntex, saved, existed, unreadable, unsupported, other);
    if (all) g_all_new += saved;
    g_ntex = 0;
    if (!all) MessageBeep(MB_ICONASTERISK);
}

/* ---- collect-all: dump every texture that gets used while it is switched on, HUD ones to dump\all\HUD, the rest to dump\all\<Group> */
static int seen_has(IDirect3DBaseTexture8 *t)
{
    int i;
    for (i = 0; i < g_nseen_tex; i++) if (g_seen_tex[i] == t) return 1;
    return 0;
}
static void seen_forget(IDirect3DBaseTexture8 *t)                /* a recycled pointer is a different texture */
{
    int i;
    for (i = 0; i < g_nseen_tex; i++) if (g_seen_tex[i] == t) { g_seen_tex[i] = g_seen_tex[--g_nseen_tex]; return; }
}
static void collect_all_toggle(void)
{
    char tmp[MAX_PATH];
    if (!g_collect_all) {
        snprintf(tmp, sizeof tmp, "%sall", g_dumpdir); CreateDirectoryA(tmp, NULL);
        snprintf(g_dir_hud, sizeof g_dir_hud, "%sall\\HUD", g_dumpdir); CreateDirectoryA(g_dir_hud, NULL); strcat(g_dir_hud, "\\");
        snprintf(g_dir_group, sizeof g_dir_group, "%sall\\%s", g_dumpdir, g_group); CreateDirectoryA(g_dir_group, NULL); strcat(g_dir_group, "\\");
        g_nseen_tex = 0; g_all_new = 0; g_collect_all = 1;
        logf_("collect-all ON: HUD textures -> %s, other textures -> %s\n", g_dir_hud, g_dir_group);
        MessageBeep(MB_ICONEXCLAMATION);
    } else {
        if (g_ntex) dump_textures(1);
        g_collect_all = 0;
        logf_("collect-all OFF: %d new PNG(s) written in this session\n", g_all_new);
        MessageBeep(MB_ICONASTERISK);
    }
}

/* --------------------------------------------------------------- HUD dump */

static void hud_begin(void)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%shud_dump_%03d.txt", g_dumpdir, ++g_hud_no);
    g_dump = fopen(p, "w");
    if (!g_dump) return;
    g_dumping = 1; g_draw_no = 0; g_hud_left = g_hud_frames;
    fprintf(g_dump, "# THPS4+ HUD dump (one full frame)\n");
    fprintf(g_dump, "# real backbuffer %ux%u, HUD layout space %ux%u, game last asked D3D for %ux%u, widescreen active=%d\n",
            g_W, g_H, g_vw, g_vh, g_rw, g_rh, g_ws_active);
}

static void hud_end(void)
{
    if (g_dump) { fprintf(g_dump, "# end of frame, %d 2D draws\n", g_draw_no); fclose(g_dump); }
    g_dump = NULL; g_dumping = 0;
    logf_("HUD dump written\n");
    MessageBeep(MB_ICONASTERISK);
}

static int vert_count(D3DPRIMITIVETYPE t, UINT n)
{
    switch (t) {
    case D3DPT_POINTLIST: return (int)n;
    case D3DPT_LINELIST: return (int)n * 2;
    case D3DPT_LINESTRIP: return (int)n + 1;
    case D3DPT_TRIANGLELIST: return (int)n * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return (int)n + 2;
    default: return 0;
    }
}

static void tex_filename(IDirect3DTexture8 *t, const D3DSURFACE_DESC *d, char *out, size_t n)
{
    D3DLOCKED_RECT lr;
    unsigned long long h;
    char fn[32];
    out[0] = '-'; out[1] = 0;
    if (FAILED(t->lpVtbl->LockRect(t, 0, &lr, NULL, D3DLOCK_READONLY))) return;
    if (td_hash((unsigned)d->Format, (const unsigned char *)lr.pBits, lr.Pitch, d->Width, d->Height, &h))
        snprintf(out, n, "%08lX%08lX_%ux%u_%s.png", (unsigned long)(h >> 32), (unsigned long)(h & 0xFFFFFFFFUL),
                 d->Width, d->Height, td_fmt_name((unsigned)d->Format, fn, sizeof fn));
    t->lpVtbl->UnlockRect(t, 0);
}

static void tex_info(char *tex, size_t tn, char *fname, size_t fn)
{
    snprintf(tex, tn, "-"); snprintf(fname, fn, "-");
    if (g_tex0) {
        IDirect3DBaseTexture8 *b = g_tex0;
        if (b->lpVtbl->GetType(b) == D3DRTYPE_TEXTURE) {
            IDirect3DTexture8 *t = (IDirect3DTexture8 *)b;
            D3DSURFACE_DESC d;
            if (SUCCEEDED(t->lpVtbl->GetLevelDesc(t, 0, &d))) {
                snprintf(tex, tn, "%ux%u fmt=%d", d.Width, d.Height, (int)d.Format);
                tex_filename(t, &d, fname, fn);
            }
        }
    }
}

static void hud_draw_ex(const char *path, D3DPRIMITIVETYPE type, UINT count, const void *data, UINT stride, int nv)
{
    char tex[96], fname[96];
    DWORD fvf = g_fvf;
    int i;
    const unsigned char *p = (const unsigned char *)data;

    tex_info(tex, sizeof tex, fname, sizeof fname);
    fprintf(g_dump, "DRAW %d type=%d prims=%u stride=%u fvf=0x%03lX tex=%s path=%s file=%s\n",
            g_draw_no++, (int)type, count, stride, (unsigned long)fvf, tex, path, fname);

    for (i = 0; i < nv; i++) {
        const unsigned char *v = p + (size_t)i * stride;
        const float *f = (const float *)v;
        size_t off = 16;
        unsigned long diff = 0;
        float u = 0, w = 0;
        int ntex = (int)((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
        if (fvf & D3DFVF_DIFFUSE) { memcpy(&diff, v + off, 4); off += 4; }
        if (fvf & D3DFVF_SPECULAR) off += 4;
        if (ntex) { memcpy(&u, v + off, 4); memcpy(&w, v + off + 4, 4); }
        fprintf(g_dump, "  v%d %.2f %.2f z=%.3f rhw=%.3f diff=%08lX uv=%.4f,%.4f\n",
                i, f[0], f[1], f[2], f[3], diff, u, w);
    }
}

static void hud_draw(D3DPRIMITIVETYPE type, UINT count, const void *data, UINT stride)
{
    hud_draw_ex("UP", type, count, data, stride, vert_count(type, count));
}


/* ------------------------------------------------------------- widescreen */

/* Experiments with how the device is presented (the loading video freezes under d3d8to9 outside Steam). */
static void apply_overrides(D3DPRESENT_PARAMETERS *pp)
{
    static int logged;
    if (!pp) return;
    if (g_ov_ms >= 0) pp->MultiSampleType = (D3DMULTISAMPLE_TYPE)g_ov_ms;
    if (g_ov_bb >= 0) pp->BackBufferCount = (UINT)g_ov_bb;
    if (g_ov_swap >= 0) pp->SwapEffect = (D3DSWAPEFFECT)g_ov_swap;
    if (g_ov_windowed) { pp->Windowed = TRUE; pp->FullScreen_RefreshRateInHz = 0; pp->FullScreen_PresentationInterval = 0; }
    if (!logged && (g_ov_ms >= 0 || g_ov_bb >= 0 || g_ov_swap >= 0 || g_ov_windowed)) {
        logged = 1;
        logf_("display overrides: windowed=%d multisample=%d backbuffers=%d swap effect=%d\n", g_ov_windowed, g_ov_ms, g_ov_bb, g_ov_swap);
    }
}

static int apply_resolution(D3DPRESENT_PARAMETERS *pp)
{
    if (!g_ws_enable || !pp || !pp->BackBufferWidth || !pp->BackBufferHeight) return 0;
    g_ask_w = pp->BackBufferWidth; g_ask_h = pp->BackBufferHeight;
    pp->BackBufferWidth = g_W; pp->BackBufferHeight = g_H;
    g_rw = g_W; g_rh = g_H;      /* the game reads the real size back and lays its 2D out for it */
    if (!pp->Windowed) pp->FullScreen_RefreshRateInHz = 0;
    logf_("widescreen: game asked for %ux%u, giving it %ux%u (HUD layout space %ux%u)\n", g_ask_w, g_ask_h, g_W, g_H, g_vw, g_vh);
    return 1;
}

static void scale_dim(UINT *w, UINT *h)
{
    if (g_ws_active && *w == g_ask_w && *h == g_ask_h) { *w = g_W; *h = g_H; }
}

/* Content hash of a texture (same hash as the dumped file names), cached per texture. */
static int tex_hash_cached(IDirect3DBaseTexture8 *b, unsigned long long *out)
{
    unsigned idx;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    IDirect3DTexture8 *t;
    unsigned long long h;

    if (!b) return 0;
    idx = (unsigned)(((size_t)b >> 4) & 1023);
    if (g_tc[idx].p == (void *)b) { *out = g_tc[idx].h; return g_tc[idx].valid; }
    g_tc[idx].p = (void *)b; g_tc[idx].valid = 0; g_tc[idx].h = 0;
    if (b->lpVtbl->GetType(b) != D3DRTYPE_TEXTURE) return 0;
    t = (IDirect3DTexture8 *)b;
    if (FAILED(t->lpVtbl->GetLevelDesc(t, 0, &d))) return 0;
    if (FAILED(t->lpVtbl->LockRect(t, 0, &lr, NULL, D3DLOCK_READONLY))) return 0;
    if (td_hash((unsigned)d.Format, (const unsigned char *)lr.pBits, lr.Pitch, d.Width, d.Height, &h)) {
        g_tc[idx].valid = 1; g_tc[idx].h = h;
    }
    t->lpVtbl->UnlockRect(t, 0);
    *out = g_tc[idx].h;
    return g_tc[idx].valid;
}

static int in_list(const unsigned long long *a, int n, unsigned long long h)
{
    int i;
    for (i = 0; i < n; i++) if (a[i] == h) return 1;
    return 0;
}

/* Which kind of menu does this texture belong to? (highest priority wins) */
static int menu_kind_of(unsigned long long h)
{
    int k;
    for (k = MK_KEYBOARD; k >= MK_GENERIC; k--)
        if (k != MK_DIALOG && in_list(g_kindhash[k], g_nkind[k], h)) return k;     /* dialogs have their own handling */
    return MK_NONE;
}

/* One uniform scale for the 640x480 layout: fit by height on 4:3 and wider, by width on narrower screens. */
static float layout_scale(void)
{
    float a = (float)g_H / (float)g_vh, b = (float)g_W / (float)g_vw;
    return a < b ? a : b;
}

static int match_rule(unsigned long long th, float cx, float cy)
{
    int i;
    for (i = 0; i < g_nrules; i++) {
        const AnchorRule *r = &g_rules[i];
        if (r->h != th) continue;
        if (r->req && !in_list(g_seen_prev, g_nseen_prev, r->req)) continue;
        if (r->nomenu && g_menu_cur == 1) continue;                    /* skip while a normal menu screen (bottom prompt bar) is open */
        if (r->maxseen > 0 && g_nseen_cur > r->maxseen) continue;     /* "only among the first N different textures of the frame" (popups come first) */
        if (r->box && !(cx >= r->x0 && cx <= r->x1 && cy >= r->y0 && cy <= r->y1)) continue;
        return i;
    }
    return -1;
}

static int in_rect(const DRect *r, float cx, float cy)
{
    return r->valid && cx >= r->x0 + 2.0f && cx <= r->x1 - 2.0f && cy >= r->y0 + 2.0f && cy <= r->y1 - 2.0f;
}

/* Is this piece the continuation of a centred line of text (starts just after one, same height)? */
static int glued_to_centre(float minx, float miny, float maxy)
{
    int i;
    float h = maxy - miny;
    for (i = 0; i < g_ncp; i++) {
        const CPiece *c = &g_cp[i];
        float top = miny > c->y0 ? miny : c->y0, bot = maxy < c->y1 ? maxy : c->y1;
        float hm = h < (c->y1 - c->y0) ? h : (c->y1 - c->y0);
        float gap = minx - c->x1;
        if (hm <= 0.0f || bot - top < 0.5f * hm) continue;
        if (gap >= -2.0f && gap <= 14.0f) return 1;
    }
    return 0;
}

/* Rewrite the x/y of one pre-transformed draw (a whole batch shares one placement).
 * The game lays its 2D out in a fixed 640x480 space and stretches it to the real screen
 * (x * W/640, y * H/480). We undo that stretch first, then place the result with one uniform scale.
 * Placement is by explicit rule (see [Anchors] in the ini): only the listed pieces of HUD stick to
 * the left or right edge. EVERYTHING ELSE stays in the centred 4:3 box exactly as the game laid it out.
 * While a menu or dialog is open, the score/cash HUD follows the [Menus] policy (anchored, hidden or
 * tucked) and everything else is centred.
 * Returns 1 if the draw should be skipped (hidden HUD). */
static int remap_2d(unsigned char *buf, int nv, UINT stride, IDirect3DBaseTexture8 *tex, int have_h, unsigned long long th)
{
    if (g_collect_all && tex) { int q; for (q = 0; q < g_ntex; q++) if (g_texset[q] == tex) g_texhud[q] = 1; }
    int i, mode, ri = -1, side = 1, hud = 0, pol = HP_ANCHORED;     /* mode: 0 left, 1 centre, 2 right, 3 fill */
    float minx = 1e9f, maxx = -1e9f, gminx = 1e9f, gmaxx = -1e9f, minyv = 1e9f, maxyv = -1e9f;
    float w, s, oy, ofs, ix = 0, iy = 0, vw = (float)g_vw, nminx = 1e9f, nmaxx = -1e9f, pic_w = 0.0f, pic_ofs = 0.0f;
    float kx = (float)g_vw / (float)g_rw, ky = (float)g_vh / (float)g_rh;
    static const char *names[7] = {"left", "centre", "right", "fill", "half1", "half2", "picture"};

    for (i = 0; i < nv; i++) {
        const float *f = (const float *)(buf + (size_t)i * stride);
        float xv = (f[0] + 0.5f) * kx, yv = (f[1] + 0.5f) * ky;     /* virtual coordinates */
        if (f[0] < gminx) gminx = f[0];
        if (f[0] > gmaxx) gmaxx = f[0];
        if (xv < minx) minx = xv;
        if (xv > maxx) maxx = xv;
        if (yv < minyv) minyv = yv;
        if (yv > maxyv) maxyv = yv;
    }
    s = layout_scale();
    oy = ((float)g_H - (float)g_vh * s) * 0.5f;      /* 0 unless the screen is narrower than 4:3 */
    w = maxx - minx;

    if (have_h) {
        int q, known = 0;
        for (q = 0; q < g_nseen_cur; q++) if (g_seen_cur[q] == th) { known = 1; break; }
        if (!known && g_nseen_cur < 96) g_seen_cur[g_nseen_cur++] = th;
        ri = match_rule(th, (minx + maxx) * 0.5f, (minyv + maxyv) * 0.5f);
        if (ri >= 0) { side = g_rules[ri].side; hud = g_rules[ri].hud; }
        {
            float cxv = (minx + maxx) * 0.5f, cyv = (minyv + maxyv) * 0.5f;
            if (in_list(g_kindhash[MK_DIALOG], g_nkind[MK_DIALOG], th)) {          /* a dialog box piece: remember where the box is */
                if (!g_drect_cur.valid) { g_drect_cur.x0 = minx; g_drect_cur.x1 = maxx; g_drect_cur.y0 = minyv; g_drect_cur.y1 = maxyv; g_drect_cur.valid = 1; }
                else {
                    if (minx < g_drect_cur.x0) g_drect_cur.x0 = minx;
                    if (maxx > g_drect_cur.x1) g_drect_cur.x1 = maxx;
                    if (minyv < g_drect_cur.y0) g_drect_cur.y0 = minyv;
                    if (maxyv > g_drect_cur.y1) g_drect_cur.y1 = maxyv;
                }
                ri = -1; side = 1; hud = 0;
            } else if (in_rect(&g_drect_cur, cxv, cyv) || in_rect(&g_drect_prev, cxv, cyv)) {
                ri = -1; side = 1; hud = 0;                                         /* text inside a dialog box stays with it */
            } else if (ri >= 0 && !hud && glued_to_centre(minx, minyv, maxyv)) {
                ri = -1; side = 1;                                                  /* "... Press [PAUSE] to ..." stays in line */
            }
        }
    }
    if (g_menu_cur && hud) {
        pol = g_hud_policy[g_menu_cur];
        if (pol == HP_HIDDEN) {
            if (g_dumping && g_dump) fprintf(g_dump, "  REMAP hidden (rule %d, menu kind %d is open)\n", ri, g_menu_cur);
            return 1;
        }
    }

    if (w >= vw * 0.95f) {
        mode = 3;                        /* full-width: fade/tint/bar -> fill ... */
        if (tex && tex->lpVtbl->GetType(tex) == D3DRTYPE_TEXTURE) {
            IDirect3DTexture8 *t = (IDirect3DTexture8 *)tex;
            D3DSURFACE_DESC d;
            if (SUCCEEDED(t->lpVtbl->GetLevelDesc(t, 0, &d)) && d.Width >= 256 && d.Height >= 256)
                mode = in_list(g_load_hash, g_nload, th) ? 6 : 1;   /* ... but a real picture keeps its proportions (listed loading screens: stretched to LoadingAspect) */
        }
    } else if (!g_hud_anchor) mode = 1;
    else if (g_menu_cur) {
        if (g_rules_in_menu[g_menu_cur]) mode = hud ? (pol == HP_ANCHORED ? side : 1) : side;   /* e.g. the pause menu over gameplay */
        else mode = (hud && pol == HP_ANCHORED) ? side : 1;
    }
    else mode = side;

    if (g_split && !g_menu_cur && mode != 3)                   /* two-player split screen: each half keeps its own left edge */
        mode = ((minx + maxx) * 0.5f < vw * 0.5f) ? 4 : 5;

    if (mode == 6) {                                    /* loading screen: a box of LoadingAspect, full height, centred; the rest stays black */
        float bw = (float)g_H * g_load_aspect;
        if (bw > (float)g_W) bw = (float)g_W;
        pic_w = bw; pic_ofs = ((float)g_W - bw) * 0.5f;
        if (pic_ofs > 0.5f) g_clear_black = 1;
    }
    ofs = (mode == 0 || mode == 4) ? 0.0f : (mode == 2) ? ((float)g_W - vw * s)
        : (mode == 5) ? ((float)g_W * 0.5f - vw * 0.5f * s) : (((float)g_W - vw * s) * 0.5f);
    if (hud && mode != 3 && mode != 1) {              /* optional extra margin for the score/cash HUD */
        ix = (mode == 0 ? g_inset_x : -g_inset_x) * s;
        iy = g_inset_y * s;
    }

    for (i = 0; i < nv; i++) {
        float *f = (float *)(buf + (size_t)i * stride);
        float xv = (f[0] + 0.5f) * kx, yv = (f[1] + 0.5f) * ky;
        if (mode == 3) { f[0] = xv * ((float)g_W / vw) - 0.5f; f[1] = yv * ((float)g_H / (float)g_vh) - 0.5f; }
        else if (mode == 6) { f[0] = xv * (pic_w / vw) + pic_ofs - 0.5f; f[1] = yv * ((float)g_H / (float)g_vh) - 0.5f; }
        else           { f[0] = xv * s + ofs + ix - 0.5f;      f[1] = yv * s + oy + iy - 0.5f; }
        if (f[0] < nminx) nminx = f[0];
        if (f[0] > nmaxx) nmaxx = f[0];
    }

    if (have_h && ri < 0 && mode == 1 && (maxyv - minyv) <= 32.0f && g_ncp < 128) {   /* remember centred text for gluing */
        g_cp[g_ncp].x0 = minx; g_cp[g_ncp].x1 = maxx; g_cp[g_ncp].y0 = minyv; g_cp[g_ncp].y1 = maxyv; g_ncp++;
    }

    if (g_dumping && g_dump)
        fprintf(g_dump, "  REMAP %s%s%s rule=%d  game x %.1f..%.1f  virtual x %.1f..%.1f y %.1f..%.1f  ->  x %.1f..%.1f\n",
                names[mode], g_menu_cur ? " [menu]" : "", hud ? " [hud]" : "", ri,
                gminx, gmaxx, minx, maxx, minyv, maxyv, nminx, nmaxx);
    return 0;
}

/* Old engines tie animation and physics to frames; running far above 60 fps shows up as flashing
 * menus and odd physics. A precise cap keeps the game at the speed it was built for. */
static void frame_limit(void)
{
    static LARGE_INTEGER freq, next;
    static int init;
    LARGE_INTEGER now;
    LONGLONG step;
    if (g_fps_limit <= 0) return;
    if (!init) {
        HMODULE wm = LoadLibraryA("winmm.dll");
        typedef UINT (WINAPI *TBP)(UINT);
        TBP tbp = wm ? (TBP)GetProcAddress(wm, "timeBeginPeriod") : NULL;
        if (tbp) tbp(1);                                   /* make Sleep(1) accurate */
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&next);
        init = 1;
    }
    step = freq.QuadPart / g_fps_limit;
    next.QuadPart += step;
    for (;;) {
        LONGLONG remain;
        QueryPerformanceCounter(&now);
        remain = next.QuadPart - now.QuadPart;
        if (remain <= 0) break;
        if (remain * 1000 / freq.QuadPart >= 2) Sleep(1); else Sleep(0);
    }
    if (now.QuadPart - next.QuadPart > step * 4) next = now;   /* fell far behind (loading): resync */
}

/* ------------------------------------------------------------ culling fix */
/* The engine builds its culling frustum in one place:
 *     fptan ; fstp st(0) ; fstp dword [FRUSTUM_TAN]        <- horizontal half-tangent
 * and derives every other frustum number from the stored value. We redirect that store through a
 * tiny stub that multiplies the tangent by g_cull_k (the same factor the picture was widened by),
 * so the engine culls against the wider view. The patch only installs if the bytes match. */
static void cull_update_k(void)
{
    float k = 1.0f;
    if (g_ws_active && g_fov_fix == 1) {
        k = ((float)g_W / (float)g_H) / ((float)g_vw / (float)g_vh);
        if (k < 1.0f) k = 1.0f;
    }
    g_cull_k = k;
}

static void cull_patch_try(void)
{
    static const unsigned char expect[8] = { 0xDD, 0xD8, 0xD9, 0x1D, 0x8C, 0x2A, 0xA7, 0x00 };
    unsigned char *site = (unsigned char *)(size_t)g_cull_site, *st;
    DWORD old, tmp, target, a, rel;
    if (!g_cull_enable || g_cull_done) return;
    if (IsBadReadPtr(site - 2, 10) || site[-2] != 0xD9 || site[-1] != 0xF2 || memcmp(site, expect, 8) != 0) {
        logf_("culling fix: the code at %08lX is not the expected frustum setup, not patching\n", (unsigned long)g_cull_site);
        return;
    }
    st = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!st) { logf_("culling fix: could not allocate the stub\n"); return; }
    memcpy(&target, site + 4, 4);                       /* the frustum global the original instruction stores to */
    a = (DWORD)(size_t)&g_cull_k;
    st[0] = 0xDD; st[1] = 0xD8;                         /* fstp st(0)                (the instruction we replaced) */
    st[2] = 0xD8; st[3] = 0x0D; memcpy(st + 4, &a, 4);  /* fmul dword ptr [g_cull_k]                              */
    st[8] = 0xD9; st[9] = 0x1D; memcpy(st + 10, &target, 4);   /* fstp dword ptr [frustum tangent]               */
    st[14] = 0xE9;                                      /* jmp back to the code after the patched bytes           */
    rel = (DWORD)((size_t)(site + 8) - ((size_t)(st + 14) + 5));
    memcpy(st + 15, &rel, 4);
    if (!VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &old)) { logf_("culling fix: could not unprotect the code\n"); return; }
    site[0] = 0xE9;
    rel = (DWORD)((size_t)st - ((size_t)site + 5));
    memcpy(site + 1, &rel, 4);
    site[5] = site[6] = site[7] = 0x90;
    VirtualProtect(site, 8, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), site, 8);
    g_cull_done = 1;
    cull_update_k();
    logf_("culling fix installed at %08lX (stub %p), frustum widening factor %.3f\n", (unsigned long)g_cull_site, (void *)st, g_cull_k);
}


/* The camera's view distance (the number the engine uses for fog AND for its distance culling) is copied
 * every frame from a table of floats in the exe: 0x44200B reads  fld [esi*4+0x5A00B0] ; fstp [0xA729C4].
 * Scaling the table scales the distance. Only for the exe build these bytes were found in. */
static float g_vd_orig[3];
static int g_vd_have;

static void viewdist_apply(void)                     /* originals * percent (100 = back to the game's own values) */
{
    float *tab = (float *)0x005A00B0;
    DWORD old, tmp;
    int i;
    if (!g_vd_have) return;
    if (!VirtualProtect(tab, 32, PAGE_READWRITE, &old)) { logf_("view distance: could not unprotect the table\n"); return; }
    for (i = 0; i < 3; i++) if (g_vd_orig[i] >= 300.0f && g_vd_orig[i] <= 20000.0f) tab[i] = g_vd_orig[i] * (float)g_viewdist_pct / 100.0f;   /* leave the 24000 far-plane entry alone */
    VirtualProtect(tab, 32, old, &tmp);
    logf_("view distance: %d%% -> table %.1f %.1f %.1f\n", g_viewdist_pct, tab[0], tab[1], tab[2]);
}

static void viewdist_patch_try(void)
{
    static const unsigned char load[7] = { 0xD9, 0x04, 0xB5, 0xB0, 0x00, 0x5A, 0x00 };
    static const unsigned char store[16] = { 0xC7, 0x05, 0xC0, 0x29, 0xA7, 0x00, 0x00, 0x00, 0x00, 0x40, 0xD9, 0x1D, 0xC4, 0x29, 0xA7, 0x00 };
    const unsigned char *a = (const unsigned char *)0x00441FE0, *b = (const unsigned char *)0x00441FEB;
    float *tab = (float *)0x005A00B0;
    if (g_viewdist_done) { viewdist_apply(); return; }
    if (IsBadReadPtr(a, 7) || IsBadReadPtr(b, 16) || IsBadReadPtr(tab, 32) || memcmp(a, load, 7) != 0 || memcmp(b, store, 16) != 0) {
        logf_("view distance: the code at 00441FE0 is not the expected one, not touching it\n");
        return;
    }
    g_viewdist_done = 1;
    g_vd_orig[0] = tab[0]; g_vd_orig[1] = tab[1]; g_vd_orig[2] = tab[2]; g_vd_have = 1;
    logf_("view distance: table at 005A00B0 = %.1f %.1f %.1f (originals)\n", tab[0], tab[1], tab[2]);
    if (g_viewdist_pct != 100) viewdist_apply();
}


/* F5: re-read the values that can be tuned while the game runs (no restart needed). */
static void reload_ini(void)
{
    char buf[64];
    const char *ini = g_ini;
    if (!ini[0]) return;
    g_hud_frames = (int)GetPrivateProfileIntA("Dump", "HudFrames", 1, ini);
    if (g_hud_frames < 1) g_hud_frames = 1;
    GetPrivateProfileStringA("Debug", "Group", "Level", g_group, sizeof g_group, ini);
    { char *q; for (q = g_group; *q; q++) if (!((*q >= '0' && *q <= '9') || (*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || *q == '_' || *q == '-')) *q = '_'; if (!g_group[0]) strcpy(g_group, "Level"); }
    g_probe_enable = (int)GetPrivateProfileIntA("Culling", "Probe", 0, ini);
    g_seq_pct = (int)GetPrivateProfileIntA("Culling", "PatchSeqPercent", 100, ini);
    GetPrivateProfileStringA("Culling", "PatchSeq", "1200,700,900,900,1100", buf, sizeof buf, ini);
    { char *c = buf; g_nseq = 0; while (*c && g_nseq < 8) { g_seq[g_nseq++] = (float)atof(c); c = strchr(c, ','); if (!c) break; c++; } }
    GetPrivateProfileStringA("Culling", "ScanValues", "900,1764", buf, sizeof buf, ini);
    { char *c = buf; g_nscanvals = 0; while (*c && g_nscanvals < 4) { g_scanvals[g_nscanvals++] = (float)atof(c); c = strchr(c, ','); if (!c) break; c++; } }
    g_viewdist_pct = (int)GetPrivateProfileIntA("Culling", "ViewDistancePercent", 100, ini);
    if (g_viewdist_pct < 10) g_viewdist_pct = 10;
    viewdist_patch_try();
    vin_config(ini);
    logf_("settings reloaded: Probe=%d Group=%s ViewDistancePercent=%d PatchSeqPercent=%d Vibration=%d/%d%%\n", g_probe_enable, g_group, g_viewdist_pct, g_seq_pct, g_in_vib, g_in_vib_scale);
    MessageBeep(MB_OK);
}

/* ------------------------------------------------------------ culling probe */
/* The engine decides what to draw (culling) from its own camera settings. To make the wide view
 * cull correctly we must change those settings, so first we have to FIND them. This scans memory
 * for numbers that match the projection the game just used (its aspect ratio constant and FOV in a
 * few common forms), then briefly changes each candidate and watches whether the projection reacts.
 * Every candidate is restored right after its test. Off by default: [Culling] Probe=1 */

static int addr_in_module(const void *p, HMODULE m)
{
    const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)m;
    const IMAGE_NT_HEADERS *nt;
    if (!m || dh->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (const IMAGE_NT_HEADERS *)((const unsigned char *)m + dh->e_lfanew);
    return (const unsigned char *)p >= (const unsigned char *)m &&
           (const unsigned char *)p < (const unsigned char *)m + nt->OptionalHeader.SizeOfImage;
}

static void probe_context(const float *a, int before, int after)
{
    char line[1200];
    size_t pos = 0;
    int i;
    line[0] = 0;
    if (IsBadReadPtr(a - before, (UINT)((before + after) * 4))) return;
    for (i = -before; i <= after && pos + 24 < sizeof line; i++)
        pos += (size_t)snprintf(line + pos, sizeof line - pos, i == 0 ? "[%.5g] " : "%.5g ", a[i]);
    logf_("      context (%d floats before, %d after): %s\n", before, after, line);
}

static void probe_add(float *a, char kind, char region)
{
    int i, same = 0;
    for (i = 0; i < g_npc; i++) if (g_pc[i].kind == kind) same++;
    if (same >= 30 || g_npc >= 200) return;
    g_pc[g_npc].addr = a; g_pc[g_npc].orig = *a; g_pc[g_npc].kind = kind; g_pc[g_npc].region = region; g_pc[g_npc].confirmed = 0;
    g_npc++;
    logf_("probe candidate %d: kind %c at %p (%s) value %.6f  neighbours: %.4f %.4f [%.4f] %.4f %.4f\n",
          g_npc - 1, kind, (void *)a, region == 'x' ? "exe image" : "heap", *a,
          a[-2], a[-1], *a, a[1], a[2]);
    if (region == 'x') probe_context(a, 24, 24);
}

static void stack_range(size_t *lo, size_t *hi)
{
    MEMORY_BASIC_INFORMATION m;
    unsigned long base = 0;
    VirtualQuery(&m, &m, sizeof m);
    *lo = (size_t)m.AllocationBase;
    __asm__ volatile("movl %%fs:4, %0" : "=r"(base));      /* top of this thread's stack (TEB.StackBase) */
    *hi = (size_t)base;
}

/* The engine builds the projection from a fixed aspect (1.4666667). If it is a constant inside an
 * instruction it can't be found by the float scan above, so look for its bytes in the code. */
static void scan_aspect_immediates(void)
{
    static const unsigned char pat[2][4] = { {0xBC, 0xBB, 0xBB, 0x3F}, {0xBB, 0xBB, 0xBB, 0x3F} };
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *p = (unsigned char *)0x10000;
    HMODULE exe = GetModuleHandleA(NULL);
    int found = 0, k;
    while ((size_t)p < 0x7FFE0000u && VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
        int code = (mbi.State == MEM_COMMIT) && (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (code && addr_in_module(mbi.BaseAddress, exe)) {
            unsigned char *c = (unsigned char *)mbi.BaseAddress, *end = c + mbi.RegionSize;
            for (; c + 4 <= end && found < 16; c++)
                for (k = 0; k < 2; k++)
                    if (c[0] == pat[k][0] && c[1] == pat[k][1] && c[2] == pat[k][2] && c[3] == pat[k][3]) {
                        char hex[160];
                        size_t pos = 0;
                        int j;
                        hex[0] = 0;
                        if (c - 16 >= (unsigned char *)mbi.BaseAddress && c + 12 <= end)
                            for (j = -16; j < 12 && pos + 4 < sizeof hex; j++) pos += (size_t)snprintf(hex + pos, sizeof hex - pos, "%02X ", c[j]);
                        logf_("aspect immediate %s in code at %p, bytes (16 before, 4 constant, 8 after): %s\n",
                              k ? "3FBBBBBB" : "3FBBBBBC", (void *)c, hex);
                        found++;
                    }
        }
        p = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (p <= (unsigned char *)mbi.BaseAddress) break;
    }
    logf_("probe: %d code location(s) contain the aspect constant\n", found);
}

/* Probe=3: find where the game keeps given float values (for example the fog distances 900 and 1764), as data
 * and as constants inside instructions. ScanValues=a,b,c,d in [Culling], press ProbeKey in the level. */
static void probe_scan_values(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *p = (unsigned char *)0x10000;
    HMODULE exe = GetModuleHandleA(NULL);
    size_t slo, shi;
    int counts[4] = {0, 0, 0, 0}, icnt[4] = {0, 0, 0, 0}, v;
    stack_range(&slo, &shi);
    logf_("scan: looking for %d value(s) as data and as instruction constants\n", g_nscanvals);
    while ((size_t)p < 0x7FFE0000u && VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
        DWORD pr = mbi.Protect;
        int readable = (mbi.State == MEM_COMMIT) && !(pr & (PAGE_GUARD | PAGE_NOACCESS)) &&
                       (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        int isexe = addr_in_module(mbi.BaseAddress, exe);
        int ours = addr_in_module(mbi.BaseAddress, g_self) || addr_in_module(mbi.BaseAddress, g_real);
        if (readable && !ours && mbi.RegionSize < 0x20000000u && (isexe || mbi.Type == MEM_PRIVATE)) {
            unsigned char *b = (unsigned char *)mbi.BaseAddress, *end = b + mbi.RegionSize, *c;
            int code = (pr & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            for (c = b + 8; c + 12 <= end; c++) {
                for (v = 0; v < g_nscanvals; v++) {
                    float f;
                    memcpy(&f, c, 4);
                    if (f != g_scanvals[v]) continue;
                    if ((((size_t)c) & 3) == 0 && !code) {
                        if ((size_t)c >= slo && (size_t)c < shi) continue;                    /* stack temporary */
                        if (counts[v]++ < 40) {
                            const float *q = (const float *)c;
                            logf_("scan: %.1f as data at %p (%s), neighbours: %.3f %.3f [%.3f] %.3f %.3f\n", g_scanvals[v], (void *)c,
                                  isexe ? "exe image" : "heap", q[-2], q[-1], q[0], q[1], q[2]);
                        }
                    } else if (code && isexe) {
                        if (icnt[v]++ < 20) {
                            char hex[120]; size_t pos = 0; int j;
                            hex[0] = 0;
                            for (j = -8; j < 8 && pos + 4 < sizeof hex; j++) pos += (size_t)snprintf(hex + pos, sizeof hex - pos, "%02X ", c[j]);
                            logf_("scan: %.1f as an instruction constant at %p, bytes (8 before, 4 value, 4 after): %s\n", g_scanvals[v], (void *)c, hex);
                        }
                    }
                }
            }
        }
        p = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (p <= (unsigned char *)mbi.BaseAddress) break;
    }
    for (v = 0; v < g_nscanvals; v++) logf_("scan: value %.1f: %d data hit(s), %d code constant(s)\n", g_scanvals[v], counts[v], icnt[v]);
    MessageBeep(MB_ICONEXCLAMATION);
}

/* Probe=4: find a run of floats in game memory (like the 1200,700,900,900,1100 table that holds the 900 seen
 * in the fog numbers) and multiply it by PatchSeqPercent. Press ProbeKey once the level is loaded. */
static void probe_patch_seq(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *p = (unsigned char *)0x10000;
    HMODULE exe = GetModuleHandleA(NULL);
    size_t slo, shi;
    int hits = 0, i;
    stack_range(&slo, &shi);
    logf_("seq: looking for %d floats starting %.1f %.1f %.1f ...; scale %d%%\n", g_nseq, g_nseq > 0 ? g_seq[0] : 0.0f, g_nseq > 1 ? g_seq[1] : 0.0f, g_nseq > 2 ? g_seq[2] : 0.0f, g_seq_pct);
    if (g_nseq < 3) { logf_("seq: need at least 3 values in PatchSeq\n"); return; }
    while ((size_t)p < 0x7FFE0000u && VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
        DWORD pr = mbi.Protect;
        int writable = (mbi.State == MEM_COMMIT) && !(pr & (PAGE_GUARD | PAGE_NOACCESS)) && (pr & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
        int ours = addr_in_module(mbi.BaseAddress, g_self) || addr_in_module(mbi.BaseAddress, g_real);
        if (writable && !ours && mbi.RegionSize < 0x20000000u && (addr_in_module(mbi.BaseAddress, exe) || mbi.Type == MEM_PRIVATE)) {
            float *f = (float *)mbi.BaseAddress, *end = (float *)((unsigned char *)mbi.BaseAddress + mbi.RegionSize);
            for (; f + g_nseq <= end; f++) {
                if (f[0] != g_seq[0] || f[1] != g_seq[1] || f[2] != g_seq[2]) continue;
                for (i = 3; i < g_nseq; i++) if (f[i] != g_seq[i]) break;
                if (i < g_nseq) continue;
                if ((size_t)f >= slo && (size_t)f < shi) continue;
                if (hits++ < 20) {
                    logf_("seq: match at %p (%s)\n", (void *)f, addr_in_module(f, exe) ? "exe image" : "heap");
                    if (g_seq_pct != 100) for (i = 0; i < g_nseq; i++) f[i] = g_seq[i] * (float)g_seq_pct / 100.0f;
                }
            }
        }
        p = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (p <= (unsigned char *)mbi.BaseAddress) break;
    }
    logf_("seq: %d match(es)%s\n", hits, g_seq_pct != 100 ? ", scaled" : " (scale is 100, nothing changed)");
    MessageBeep(MB_ICONEXCLAMATION);
}

static void probe_scan(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *p = (unsigned char *)0x10000;
    HMODULE exe = GetModuleHandleA(NULL);
    float r, fov_rad, fov_deg, tan_h;
    size_t slo, shi;
    int stack_logged = 0;
    g_npc = 0; g_pci = 0; g_pconfirmed = 0;
    if (g_raw_m11 == 0.0f) { logf_("probe: no projection seen yet, aim at the game world first\n"); return; }
    stack_range(&slo, &shi);
    r = g_raw_m22 / g_raw_m11;
    tan_h = 1.0f / g_raw_m11;
    fov_rad = 2.0f * atanf(tan_h);
    fov_deg = fov_rad * 57.29578f;
    logf_("probe: scanning for aspect %.5f, fov %.4f deg / %.5f rad, tan(fov/2) %.5f (m11 %.5f m22 %.5f)\n",
          r, fov_deg, fov_rad, tan_h, g_raw_m11, g_raw_m22);

    while ((size_t)p < 0x7FFE0000u && VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
        DWORD pr = mbi.Protect;
        int readable = (mbi.State == MEM_COMMIT) && !(pr & (PAGE_GUARD | PAGE_NOACCESS)) &&
                       (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                              PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        int isexe = addr_in_module(mbi.BaseAddress, exe);
        int ours = addr_in_module(mbi.BaseAddress, g_self) || addr_in_module(mbi.BaseAddress, g_real);
        if (readable && !ours && mbi.RegionSize < 0x20000000u && (isexe || mbi.Type == MEM_PRIVATE)) {
            float *f = (float *)mbi.BaseAddress, *end = (float *)((unsigned char *)mbi.BaseAddress + mbi.RegionSize);
            for (f += 2; f + 2 < end; f++) {
                float v = *f;
                char kind = 0;
                if (v < 0.01f || v > 400.0f) continue;
                if (fabsf(v - r) < 3e-5f) kind = 'A';
                else if (fabsf(v - fov_rad) < 1e-4f) kind = 'R';
                else if (fabsf(v - fov_deg) < 2e-3f) kind = 'D';
                else if (fabsf(v - tan_h) < 1e-5f) kind = 'T';
                else if (fabsf(v - fov_rad * 0.5f) < 1e-4f) kind = 'r';
                else if (fabsf(v - fov_deg * 0.5f) < 2e-3f) kind = 'd';
                if (!kind) continue;
                if ((size_t)f >= slo && (size_t)f < shi) {          /* a function-local temporary: never touch these */
                    if (stack_logged < 6) { stack_logged++; logf_("probe: kind %c is a stack temporary at %p (%.6f), skipped\n", kind, (void *)f, v); }
                    continue;
                }
                probe_add(f, kind, isexe ? 'x' : 'h');
            }
        }
        p = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (p <= (unsigned char *)mbi.BaseAddress) break;
    }
    scan_aspect_immediates();
    logf_("probe: %d candidates found. Testing them now: stand still and do not touch the controls.\n", g_npc);
    g_pstate = g_npc ? 1 : 0; g_pphase = 0; g_pwait = 30;
}

static void probe_poke(float *a, float v)
{
    DWORD old, tmp;
    if (VirtualProtect(a, 4, PAGE_EXECUTE_READWRITE, &old)) { *a = v; VirtualProtect(a, 4, old, &tmp); }
}

/* ---- hardware watchpoints: who reads / writes the camera numbers? ----
 * The CPU can trap every access to a chosen address. We record the address of each instruction that
 * touches the engine's camera variables, plus the machine code around it, so the code that builds the
 * projection and the culling frustum can be found and patched exactly. */
typedef struct { DWORD eip, var; int is_write; unsigned hits; DWORD regs[8]; } WatchHit;
static WatchHit g_wh[96];
static volatile LONG g_nwh;
static volatile LONG g_totalhits;
static DWORD g_wvar[2];
static int g_nw;
typedef struct { HANDLE th; DWORD a[2]; int n; int on; } WpJob;
static WpJob g_job;
static DWORD g_pwatch_end;

static LONG CALLBACK wp_veh(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c;
    DWORD dr6;
    int v, i, write, any = -1;

    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    c = ep->ContextRecord;
    dr6 = c->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    for (v = 0; v < g_nw; v++) if (dr6 & (3u << (2 * v))) { any = v; break; }
    if (any < 0) { c->Dr6 = 0; return EXCEPTION_CONTINUE_EXECUTION; }
    write = (dr6 & (1u << (2 * any))) != 0;          /* the write-only register fired */
    for (i = 0; i < g_nwh; i++)
        if (g_wh[i].eip == c->Eip && g_wh[i].var == g_wvar[any] && g_wh[i].is_write == write) { g_wh[i].hits++; break; }
    if (i == g_nwh && g_nwh < 96) {
        WatchHit *h = &g_wh[g_nwh];
        h->eip = c->Eip; h->var = g_wvar[any]; h->is_write = write; h->hits = 1;
        h->regs[0] = c->Eax; h->regs[1] = c->Ecx; h->regs[2] = c->Edx; h->regs[3] = c->Ebx;
        h->regs[4] = c->Esi; h->regs[5] = c->Edi; h->regs[6] = c->Ebp; h->regs[7] = c->Esp;
        g_nwh++;
    }
    c->Dr6 = 0;
    if (++g_totalhits > 30000) c->Dr7 = 0;            /* far too hot: switch the watchpoints off for safety */
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI wp_worker(LPVOID p)
{
    WpJob *j = (WpJob *)p;
    CONTEXT ctx;
    if (SuspendThread(j->th) == (DWORD)-1) return 1;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(j->th, &ctx)) {
        if (j->on) {
            DWORD dr7 = 0;
            int v;
            for (v = 0; v < j->n && v < 2; v++) {
                int k0 = 2 * v, k1 = 2 * v + 1;                 /* k0: write only, k1: read or write */
                if (v == 0) { ctx.Dr0 = j->a[0]; ctx.Dr1 = j->a[0]; } else { ctx.Dr2 = j->a[1]; ctx.Dr3 = j->a[1]; }
                dr7 |= (1u << (2 * k0)) | (1u << (16 + 4 * k0)) | (3u << (18 + 4 * k0));
                dr7 |= (1u << (2 * k1)) | (3u << (16 + 4 * k1)) | (3u << (18 + 4 * k1));
            }
            ctx.Dr7 = dr7;
        } else {
            ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = 0; ctx.Dr7 = 0;
        }
        ctx.Dr6 = 0;
        SetThreadContext(j->th, &ctx);
    }
    ResumeThread(j->th);
    return 0;
}

static void wp_run(int on)
{
    HANDLE th;
    g_job.on = on;
    th = CreateThread(NULL, 0, wp_worker, &g_job, 0, NULL);
    if (th) CloseHandle(th);
}

static void wp_arm(const DWORD *addr, int n)
{
    static int veh_added;
    HANDLE h = NULL;
    int i;
    if (!veh_added) { AddVectoredExceptionHandler(1, wp_veh); veh_added = 1; }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    for (i = 0; i < n && i < 2; i++) { g_wvar[i] = addr[i]; g_job.a[i] = addr[i]; }
    g_nw = n; g_job.n = n; g_job.th = h; g_nwh = 0; g_totalhits = 0;
    wp_run(1);
}

static void probe_arm(const DWORD *addr, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        logf_("probe: watching %08lX\n", (unsigned long)addr[i]);
        probe_context((const float *)(size_t)addr[i], 24, 24);
    }
    wp_arm(addr, n);
    g_pwatch_end = GetTickCount() + 12000;
    g_pstate = 2;
    MessageBeep(MB_ICONASTERISK);
    logf_("probe: watching for 12 seconds. Skate around and turn the camera so it moves, then wait for the second beep.\n");
}

/* Probe=2: skip the scan and watch the addresses in WatchAddresses directly. */
static void probe_watch_cfg(void)
{
    if (g_nwatchcfg == 0) { logf_("probe: no WatchAddresses configured\n"); return; }
    scan_aspect_immediates();
    probe_arm(g_watchcfg, g_nwatchcfg);
}

static void probe_start_watch(void)
{
    int i, n = 0;
    DWORD addr[2];
    /* only ever watch fixed addresses inside the exe: things that really controlled the projection, or exe-image candidates */
    for (i = 0; i < g_npc && n < 2; i++) if (g_pc[i].confirmed && g_pc[i].region == 'x') addr[n++] = (DWORD)(size_t)g_pc[i].addr;
    for (i = 0; i < g_npc && n < 2; i++) if (!g_pc[i].confirmed && g_pc[i].region == 'x') addr[n++] = (DWORD)(size_t)g_pc[i].addr;
    if (n == 0) {
        g_pstate = 0;
        logf_("probe: no fixed exe-image candidates to watch, stopping here (heap values are not watched)\n");
        MessageBeep(MB_ICONASTERISK);
        return;
    }
    probe_arm(addr, n);
}

static void probe_end_watch(void)
{
    int i, j;
    HMODULE exe = GetModuleHandleA(NULL);
    wp_run(0);
    Sleep(150);
    logf_("probe: watch finished, %ld distinct accessing instructions, %ld total accesses%s\n", (long)g_nwh, (long)g_totalhits,
          g_totalhits > 30000 ? " (stopped early: that address is read very often)" : "");
    for (i = 0; i < g_nwh; i++) {
        const WatchHit *h = &g_wh[i];
        char hex[256];
        size_t pos = 0;
        hex[0] = 0;
        if (addr_in_module((const void *)(size_t)h->eip, exe) && addr_in_module((const void *)(size_t)(h->eip - 48), exe)) {
            const unsigned char *code = (const unsigned char *)(size_t)(h->eip - 48);
            for (j = 0; j < 56 && pos + 4 < sizeof hex; j++) pos += (size_t)snprintf(hex + pos, sizeof hex - pos, "%02X ", code[j]);
        }
        logf_("watch: var %08lX %s at eip %08lX (%u hits) eax=%08lX ecx=%08lX edx=%08lX ebx=%08lX esi=%08lX edi=%08lX ebp=%08lX\n",
              (unsigned long)h->var, h->is_write ? "WRITE" : "read ", (unsigned long)h->eip, h->hits,
              (unsigned long)h->regs[0], (unsigned long)h->regs[1], (unsigned long)h->regs[2], (unsigned long)h->regs[3],
              (unsigned long)h->regs[4], (unsigned long)h->regs[5], (unsigned long)h->regs[6]);
        logf_("      code (48 bytes before eip, then 8 after): %s\n", hex);
    }
    g_pstate = 0;
    MessageBeep(MB_ICONEXCLAMATION);
}

static void probe_tick(void)
{
    static SHORT prev;
    SHORT k;
    if (!g_probe_enable) return;
    k = GetAsyncKeyState(g_vk_probe);
    if ((k & 0x8000) && !(prev & 0x8000) && g_pstate == 0) {
        if (g_probe_enable == 2) probe_watch_cfg(); else if (g_probe_enable == 3) probe_scan_values(); else if (g_probe_enable == 4) probe_patch_seq(); else probe_scan();
    }
    prev = k;
    if (g_pstate == 2) { if ((int)(GetTickCount() - g_pwatch_end) >= 0) probe_end_watch(); return; }
    if (g_pstate != 1 || g_raw_m11 == 0.0f) return;
    if (g_pwait > 0) { g_pwait--; return; }
    if (g_pci >= g_npc) {
        logf_("probe finished: %d of %d candidates controlled the projection\n", g_pconfirmed, g_npc);
        probe_start_watch();
        return;
    }
    {
        ProbeCand *c = &g_pc[g_pci];
        if (g_pphase == 0) {
            g_pb11 = g_raw_m11; g_pb22 = g_raw_m22;
            probe_poke(c->addr, c->orig * 1.25f);
            g_pphase = 1; g_pwait = 3;
        } else {
            float a = g_raw_m11 / g_pb11, b = g_raw_m22 / g_pb22;
            int hit = (fabsf(a - 1.0f) > 0.04f || fabsf(b - 1.0f) > 0.04f);
            logf_("probe test %d kind %c %p: x1.25 -> m11 x%.3f, m22 x%.3f %s\n",
                  g_pci, c->kind, (void *)c->addr, a, b, hit ? "  <== CONTROLS THE PROJECTION" : "");
            probe_poke(c->addr, c->orig);
            if (hit) { g_pconfirmed++; c->confirmed = 1; }
            g_pci++; g_pphase = 0; g_pwait = 3;
        }
    }
}

/* ------------------------------------------------------------ device hooks */

/* ---------------------------------------------------------- texture import + controller glyphs */
/* Replacement textures: PNG files named like the dump (<hash>_<w>x<h>_<fmt>.png) in
 * THPS4Plus\import\textures\custom replace the game texture with that hash (any size, mipmaps are
 * generated). The button-glyph sheet is special: if a controller is detected it is replaced by
 * THPS4Plus\import\textures\controller\xbox.png / playstation.png / nintendo.png / generic.png. */
#define MAXREPL 512
typedef struct { unsigned long long h; unsigned w, hh, fmt; char path[MAX_PATH]; IDirect3DTexture8 *tex; int tried; } Repl;
static Repl g_repl[MAXREPL];
static int g_nrepl;
static char g_ctrl_path[5][MAX_PATH];                 /* 1 xbox, 2 playstation, 3 nintendo, 4 generic */
static IDirect3DTexture8 *g_ctrl_tex[5];
static int g_ctrl_tried[5];
static int g_ctrl_type;                               /* 0 = none (keyboard), 1..4 as above */
typedef struct { void *p; int idx; } RCache;          /* idx: -2 nothing to do, >= 0 registry entry, 100000 glyph sheet */
static RCache g_rc[1024];

static unsigned fmt_from_name(const char *n)
{
    static const char *names[] = { "A8R8G8B8", "X8R8G8B8", "R5G6B5", "X1R5G5B5", "A1R5G5B5", "A4R4G4B4", "A8", "L8", "DXT1", "DXT2", "DXT3", "DXT4", "DXT5" };
    static const unsigned codes[] = { TD_A8R8G8B8, TD_X8R8G8B8, TD_R5G6B5, TD_X1R5G5B5, TD_A1R5G5B5, TD_A4R4G4B4, TD_A8, TD_L8, TD_DXT1, TD_DXT2, TD_DXT3, TD_DXT4, TD_DXT5 };
    size_t i;
    for (i = 0; i < sizeof codes / sizeof codes[0]; i++) if (!strcmp(n, names[i])) return codes[i];
    return 0;
}

static void import_scan(void)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    static const char *ctrl[5] = { "", "xbox.png", "playstation.png", "nintendo.png", "generic.png" };
    int k;
    for (k = 1; k <= 4; k++) snprintf(g_ctrl_path[k], MAX_PATH, "%sTHPS4Plus\\import\\textures\\controller\\%s", g_base, ctrl[k]);
    snprintf(pat, sizeof pat, "%sTHPS4Plus\\import\\textures\\custom\\*.png", g_base);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char hs[17], fm[32];
        unsigned w = 0, hgt = 0;
        int i, n;
        Repl *r;
        if (g_nrepl >= MAXREPL || strlen(fd.cFileName) < 16) continue;
        memcpy(hs, fd.cFileName, 16); hs[16] = 0;
        for (i = 0; i < 16 && isxdigit((unsigned char)hs[i]); i++) {}
        if (i < 16) continue;
        r = &g_repl[g_nrepl++];
        memset(r, 0, sizeof *r);
        r->h = strtoull(hs, NULL, 16);
        fm[0] = 0;
        n = sscanf(fd.cFileName + 16, "_%ux%u_%31[A-Za-z0-9]", &w, &hgt, fm);
        if (n >= 2) { r->w = w; r->hh = hgt; }
        if (n >= 3) r->fmt = fmt_from_name(fm);
        snprintf(r->path, MAX_PATH, "%sTHPS4Plus\\import\\textures\\custom\\%s", g_base, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    logf_("import: %d custom texture(s) found\n", g_nrepl);
}

/* PNG file -> managed A8R8G8B8 texture with a full mip chain */
static IDirect3DTexture8 *import_load(IDirect3DDevice8 *dev, const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *buf, *rgba, *cur, *nxt;
    unsigned w, h, lw, lh;
    DWORD level, levels;
    const char *err = NULL;
    IDirect3DTexture8 *t = NULL;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    rgba = tl_png_decode(buf, (size_t)n, &w, &h, &err);
    free(buf);
    if (!rgba) { logf_("import: %s: %s\n", path, err ? err : "cannot read"); return NULL; }
    if (FAILED(g_devOrig.CreateTexture(dev, w, h, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) || !t) {
        if (FAILED(g_devOrig.CreateTexture(dev, w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) || !t) {
            logf_("import: could not create a %ux%u texture for %s\n", w, h, path); free(rgba); return NULL;
        }
    }
    levels = t->lpVtbl->GetLevelCount(t);
    cur = rgba; lw = w; lh = h;
    for (level = 0; level < levels; level++) {
        D3DLOCKED_RECT lr;
        unsigned y, x;
        if (SUCCEEDED(t->lpVtbl->LockRect(t, level, &lr, NULL, 0))) {
            for (y = 0; y < lh; y++) {
                unsigned char *dst = (unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch;
                const unsigned char *src = cur + (size_t)y * lw * 4;
                for (x = 0; x < lw; x++) { dst[x*4] = src[x*4+2]; dst[x*4+1] = src[x*4+1]; dst[x*4+2] = src[x*4]; dst[x*4+3] = src[x*4+3]; }
            }
            t->lpVtbl->UnlockRect(t, level);
        }
        if (level + 1 < levels) {
            nxt = (unsigned char *)malloc((size_t)(lw > 1 ? lw / 2 : 1) * (lh > 1 ? lh / 2 : 1) * 4);
            if (!nxt) break;
            tl_box_down(cur, lw, lh, nxt);
            if (cur != rgba) free(cur);
            cur = nxt; lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
        }
    }
    if (cur != rgba) free(cur);
    free(rgba);
    logf_("import: loaded %s (%ux%u, %lu mip levels)\n", path, w, h, (unsigned long)levels);
    return t;
}

static IDirect3DTexture8 *repl_texture(IDirect3DDevice8 *dev, int i)
{
    Repl *r = &g_repl[i];
    if (!r->tex && !r->tried) { r->tried = 1; r->tex = import_load(dev, r->path); }
    return r->tex;
}

static IDirect3DTexture8 *glyph_texture(IDirect3DDevice8 *dev)
{
    int t = g_ctrl_force >= 0 ? g_ctrl_force : g_ctrl_type, i, k;
    for (k = 0; k < 2 && t >= 1 && t <= 4; k++) {            /* the controller's own sheet, then generic.png */
        int u = k == 0 ? t : 4;
        if (!g_ctrl_tex[u] && !g_ctrl_tried[u]) { g_ctrl_tried[u] = 1; g_ctrl_tex[u] = import_load(dev, g_ctrl_path[u]); }
        if (g_ctrl_tex[u]) return g_ctrl_tex[u];
    }
    for (i = 0; i < g_nrepl; i++) if (g_repl[i].h == g_glyph_hash) return repl_texture(dev, i);   /* keyboard: custom folder */
    return NULL;
}

static int import_wanted(unsigned w, unsigned h, unsigned fmt)
{
    int i;
    if (w == 256 && h == 64) return 1;                      /* the button-glyph sheet */
    for (i = 0; i < g_nrepl; i++) {
        const Repl *r = &g_repl[i];
        if ((!r->w || r->w == w) && (!r->hh || r->hh == h) && (!r->fmt || r->fmt == fmt)) return 1;
    }
    return 0;
}

static IDirect3DBaseTexture8 *import_texture_for(IDirect3DDevice8 *dev, IDirect3DBaseTexture8 *b)
{
    unsigned idx = (unsigned)(((size_t)b >> 4) & 1023);
    IDirect3DTexture8 *r = NULL;
    int ri;
    if (g_rc[idx].p != (void *)b) {
        D3DSURFACE_DESC d;
        unsigned long long hsh;
        int i;
        g_rc[idx].p = (void *)b; g_rc[idx].idx = -2;
        if (b->lpVtbl->GetType(b) == D3DRTYPE_TEXTURE) {
            IDirect3DTexture8 *t = (IDirect3DTexture8 *)b;
            if (SUCCEEDED(t->lpVtbl->GetLevelDesc(t, 0, &d)) && import_wanted(d.Width, d.Height, (unsigned)d.Format) && tex_hash_cached(b, &hsh)) {
                if (hsh == g_glyph_hash) g_rc[idx].idx = 100000;
                else for (i = 0; i < g_nrepl; i++) if (g_repl[i].h == hsh) { g_rc[idx].idx = i; break; }
            }
        }
    }
    ri = g_rc[idx].idx;
    if (ri == -2) return NULL;
    r = (ri == 100000) ? glyph_texture(dev) : repl_texture(dev, ri);
    return (IDirect3DBaseTexture8 *)r;
}

/* ---- which controller is plugged in? (SDL2 if SDL2.dll is present, then DirectInput-era devices, then XInput) ---- */
static int type_from_name(const char *name)
{
    char low[96];
    size_t i;
    if (!name) return 0;
    for (i = 0; i + 1 < sizeof low && name[i]; i++) low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = 0;
    if (strstr(low, "xbox") || strstr(low, "x-box") || strstr(low, "xinput")) return 1;
    if (strstr(low, "playstation") || strstr(low, "dualshock") || strstr(low, "dualsense") || strstr(low, "ps4") ||
        strstr(low, "ps5") || strstr(low, "ps3") || strstr(low, "wireless controller")) return 2;
    if (strstr(low, "nintendo") || strstr(low, "pro controller") || strstr(low, "joy-con") || strstr(low, "switch")) return 3;
    return 0;
}

typedef int (*SDL_Init_t)(DWORD);
typedef int (*SDL_Int_t)(void);
typedef void (*SDL_Void_t)(void);
typedef int (*SDL_IntInt_t)(int);
typedef const char *(*SDL_Name_t)(int);
static int g_sdl_tried, g_sdl_ok;
static SDL_Int_t p_NumJoy;
static SDL_Void_t p_JoyUpdate, p_Pump;
static SDL_IntInt_t p_IsGC, p_GCType;
static SDL_Name_t p_JoyName;

static int sdl_detect(void)
{
    int n, i;
    if (!g_sdl_tried) {
        HMODULE m = LoadLibraryA("SDL2.dll");
        SDL_Init_t init;
        g_sdl_tried = 1;
        if (!m) return 0;
        init = (SDL_Init_t)GetProcAddress(m, "SDL_Init");
        p_NumJoy = (SDL_Int_t)GetProcAddress(m, "SDL_NumJoysticks");
        p_JoyUpdate = (SDL_Void_t)GetProcAddress(m, "SDL_JoystickUpdate");
        p_Pump = (SDL_Void_t)GetProcAddress(m, "SDL_PumpEvents");
        p_IsGC = (SDL_IntInt_t)GetProcAddress(m, "SDL_IsGameController");
        p_GCType = (SDL_IntInt_t)GetProcAddress(m, "SDL_GameControllerTypeForIndex");
        p_JoyName = (SDL_Name_t)GetProcAddress(m, "SDL_JoystickNameForIndex");
        if (init && p_NumJoy && init(0x00000200u | 0x00002000u) == 0) { g_sdl_ok = 1; logf_("controller: SDL2 loaded\n"); }
    }
    if (!g_sdl_ok) return 0;
    if (p_Pump) p_Pump();
    if (p_JoyUpdate) p_JoyUpdate();
    n = p_NumJoy();
    for (i = 0; i < n; i++) {
        int t = 0;
        if (p_IsGC && p_GCType && p_IsGC(i)) {
            switch (p_GCType(i)) {
            case 1: case 2: t = 1; break;                          /* Xbox 360 / One */
            case 3: case 4: case 7: t = 2; break;                  /* PS3 / PS4 / PS5 */
            case 5: case 11: case 12: case 13: t = 3; break;       /* Switch Pro / Joy-Cons */
            default: t = 0; break;
            }
        }
        if (!t && p_JoyName) t = type_from_name(p_JoyName(i));
        return t ? t : 4;
    }
    return 0;
}

static int winmm_detect(void)
{
    typedef UINT (WINAPI *PNum)(void);
    typedef MMRESULT (WINAPI *PCaps)(UINT_PTR, LPJOYCAPSA, UINT);
    typedef MMRESULT (WINAPI *PPos)(UINT, LPJOYINFO);
    static PNum pnum; static PCaps pcaps; static PPos ppos; static int tried;
    UINT n, i;
    if (!tried) {
        HMODULE m = LoadLibraryA("winmm.dll");
        tried = 1;
        if (m) { pnum = (PNum)GetProcAddress(m, "joyGetNumDevs"); pcaps = (PCaps)GetProcAddress(m, "joyGetDevCapsA"); ppos = (PPos)GetProcAddress(m, "joyGetPos"); }
    }
    if (!pnum || !pcaps || !ppos) return 0;
    n = pnum();
    for (i = 0; i < n && i < 16; i++) {
        JOYCAPSA caps;
        JOYINFO ji;
        int t;
        memset(&caps, 0, sizeof caps);
        if (pcaps(i, &caps, sizeof caps) != JOYERR_NOERROR) continue;
        if (ppos(i, &ji) != JOYERR_NOERROR) continue;          /* not actually plugged in */
        if (caps.wMid == 0x054C) t = 2;                        /* Sony */
        else if (caps.wMid == 0x057E) t = 3;                   /* Nintendo */
        else if (caps.wMid == 0x045E) t = 1;                   /* Microsoft */
        else t = type_from_name(caps.szPname);
        return t ? t : 4;
    }
    return 0;
}

typedef struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XIState;
static int xinput_detect(void)
{
    typedef DWORD (WINAPI *PGet)(DWORD, XIState *);
    static PGet pget; static int tried;
    DWORD i;
    if (!tried) {
        static const char *dlls[3] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        int k;
        tried = 1;
        for (k = 0; k < 3 && !pget; k++) { HMODULE m = LoadLibraryA(dlls[k]); if (m) pget = (PGet)GetProcAddress(m, "XInputGetState"); }
    }
    if (!pget) return 0;
    for (i = 0; i < 4; i++) { XIState st; if (pget(i, &st) == 0) return 1; }
    return 0;
}

static void ctrl_poll(void)
{
    int t = 0, b = g_ctrl_backend;
    if (!g_ctrl_detect) return;
    if (b == 0 || b == 1) t = sdl_detect();
    if (!t && (b == 0 || b == 2)) t = winmm_detect();
    if (!t && (b == 0 || b == 3)) t = xinput_detect();
    if (t != g_ctrl_type) {
        logf_("controller: now %d (0 none/keyboard, 1 Xbox, 2 PlayStation, 3 Nintendo, 4 generic)\n", t);
        g_ctrl_type = t;
    }
}

static void reload_ini(void);

static HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice8 *This, CONST RECT *a, CONST RECT *b, HWND c, CONST RGNDATA *d)
{
    static SHORT prev_hud, prev_tex, prev_all, prev_reload;
    SHORT k;
    HRESULT hr;

    if (g_texcollect) { dump_textures(0); g_texcollect = 0; }   /* frame just finished */
    else if (g_collect_all && g_ntex) dump_textures(1);
    hr = g_devOrig.Present(This, a, b, c, d);
    frame_limit();
    {
        static int frames;
        cull_update_k();
        if (++frames == 120) { cull_patch_try(); viewdist_patch_try(); }     /* second chance in case the exe was still unpacking at startup */
        if ((frames % 60) == 1) ctrl_poll();       /* about once a second */
    }
    g_raw_set = 0;
    probe_tick();
    if (g_menu_cur != g_menu_next) logf_("menu kind %d -> %d\n", g_menu_cur, g_menu_next);
    g_menu_cur = g_menu_next;
    g_menu_next = 0;
    memcpy(g_seen_prev, g_seen_cur, sizeof g_seen_prev); g_nseen_prev = g_nseen_cur; g_nseen_cur = 0;
    g_drect_prev = g_drect_cur; g_drect_cur.valid = 0; g_ncp = 0; g_split = 0;
    if (g_dumping) {
        if (--g_hud_left <= 0) hud_end();
        else if (g_dump) fprintf(g_dump, "# ---- next frame ----\n");
    }

    k = GetAsyncKeyState(g_vk_hud);
    if ((k & 0x8000) && !(prev_hud & 0x8000)) hud_begin();
    prev_hud = k;

    k = GetAsyncKeyState(g_vk_tex);
    if ((k & 0x8000) && !(prev_tex & 0x8000)) { g_ntex = 0; g_texcollect = 1; }
    prev_tex = k;

    k = GetAsyncKeyState(g_vk_all);
    if ((k & 0x8000) && !(prev_all & 0x8000)) collect_all_toggle();
    prev_all = k;

    k = GetAsyncKeyState(g_vk_reload);
    if ((k & 0x8000) && !(prev_reload & 0x8000)) reload_ini();
    prev_reload = k;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_SetVertexShader(IDirect3DDevice8 *This, DWORD h)
{
    if (!(h & 0x80000000UL)) g_fvf = h;       /* plain FVF code, not a shader handle */
    return g_devOrig.SetVertexShader(This, h);
}

static HRESULT STDMETHODCALLTYPE hk_SetTexture(IDirect3DDevice8 *This, DWORD stage, IDirect3DBaseTexture8 *t)
{
    IDirect3DBaseTexture8 *use = t;
    if (stage == 0) g_tex0 = t;                       /* our own logic always sees the ORIGINAL texture */
    if (g_texcollect && t) tex_collect(t);
    else if (g_collect_all && t && !seen_has(t) && g_nseen_tex < MAXSEEN) { g_seen_tex[g_nseen_tex++] = t; tex_collect(t); }
    if (g_import_enable && t) { IDirect3DBaseTexture8 *r = import_texture_for(This, t); if (r) use = r; }
    return g_devOrig.SetTexture(This, stage, use);
}

static HRESULT STDMETHODCALLTYPE hk_DrawPrimitiveUP(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT n, CONST void *data, UINT stride)
{
    if (g_dumping && g_dump && (g_fvf & D3DFVF_XYZRHW)) hud_draw(t, n, data, stride);   /* logs the ORIGINAL numbers */

    if (g_ws_active && g_remap_hud && g_rt_main && (g_fvf & D3DFVF_XYZRHW) && data && stride >= 16) {
        int nv = vert_count(t, n);
        size_t need = (size_t)nv * stride;
        if (nv > 0 && need <= 4u * 1024u * 1024u) {
            if (need > g_vcap) {
                free(g_vbuf);
                g_vbuf = (unsigned char *)malloc(need);
                g_vcap = g_vbuf ? need : 0;
            }
            if (g_vbuf) {
                unsigned long long th = 0;
                int have = g_tex0 ? tex_hash_cached(g_tex0, &th) : 0;
                if (have) { int k = menu_kind_of(th); if (k > g_menu_next) g_menu_next = k; }
                memcpy(g_vbuf, data, need);
                if (remap_2d(g_vbuf, nv, stride, g_tex0, have, th)) return 0;   /* hidden HUD piece */
                if (g_clear_black) { g_clear_black = 0; g_devOrig.Clear(This, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0); }
                return g_devOrig.DrawPrimitiveUP(This, t, n, g_vbuf, stride);
            }
        }
    }
    return g_devOrig.DrawPrimitiveUP(This, t, n, data, stride);
}

/* 2D drawn through other D3D calls. The indexed "UP" form carries its vertices in memory like
 * DrawPrimitiveUP, so it is remapped the same way. Vertex-buffer forms are only reported for now. */
static HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitiveUP(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT minIdx, UINT numIdx,
                                                           UINT prims, CONST void *idx, D3DFORMAT ifmt, CONST void *vdata, UINT vstride)
{
    if ((g_fvf & D3DFVF_XYZRHW) && vdata && vstride >= 16 && numIdx > 0) {
        if (g_dumping && g_dump)
            hud_draw_ex("idxUP", t, prims, (const unsigned char *)vdata + (size_t)minIdx * vstride, vstride, (int)numIdx);
        if (g_ws_active && g_remap_hud && g_rt_main) {
            size_t total = (size_t)minIdx + numIdx, need = total * vstride;
            if (need <= 4u * 1024u * 1024u) {
                if (need > g_vcap) {
                    free(g_vbuf);
                    g_vbuf = (unsigned char *)malloc(need);
                    g_vcap = g_vbuf ? need : 0;
                }
                if (g_vbuf) {
                    unsigned long long th = 0;
                    int have = g_tex0 ? tex_hash_cached(g_tex0, &th) : 0;
                    if (have) { int k = menu_kind_of(th); if (k > g_menu_next) g_menu_next = k; }
                    memcpy(g_vbuf, vdata, need);
                    if (remap_2d(g_vbuf + (size_t)minIdx * vstride, (int)numIdx, vstride, g_tex0, have, th)) return 0; g_clear_black = 0;
                    return g_devOrig.DrawIndexedPrimitiveUP(This, t, minIdx, numIdx, prims, idx, ifmt, g_vbuf, vstride);
                }
            }
        }
    }
    return g_devOrig.DrawIndexedPrimitiveUP(This, t, minIdx, numIdx, prims, idx, ifmt, vdata, vstride);
}

static void note_vb_2d(const char *kind, D3DPRIMITIVETYPE t, UINT first, UINT prims)
{
    static int warned;
    if (!(g_fvf & D3DFVF_XYZRHW) || !g_ws_active || !g_rt_main) return;
    if (warned < 12) {
        warned++;
        logf_("2D draw through %s (vertex buffer) is NOT remapped yet: fvf=0x%03lX prims=%u\n", kind, (unsigned long)g_fvf, prims);
    }
    if (g_dumping && g_dump) {
        char tex[96], fname[96];
        tex_info(tex, sizeof tex, fname, sizeof fname);
        fprintf(g_dump, "UNREMAPPED_DRAW kind=%s type=%d first=%u prims=%u fvf=0x%03lX tex=%s file=%s\n",
                kind, (int)t, first, prims, (unsigned long)g_fvf, tex, fname);
    }
}

static HRESULT STDMETHODCALLTYPE hk_DrawPrimitive(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT start, UINT prims)
{
    note_vb_2d("DrawPrimitive", t, start, prims);
    return g_devOrig.DrawPrimitive(This, t, start, prims);
}

static HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitive(IDirect3DDevice8 *This, D3DPRIMITIVETYPE t, UINT minIdx, UINT numVerts,
                                                         UINT startIdx, UINT prims)
{
    note_vb_2d("DrawIndexedPrimitive", t, startIdx, prims);
    return g_devOrig.DrawIndexedPrimitive(This, t, minIdx, numVerts, startIdx, prims);
}

static HRESULT STDMETHODCALLTYPE hk_SetViewport(IDirect3DDevice8 *This, CONST D3DVIEWPORT8 *vp)
{
    if (g_dumping && g_dump && vp)
        fprintf(g_dump, "VIEWPORT x=%lu y=%lu w=%lu h=%lu\n", vp->X, vp->Y, vp->Width, vp->Height);

    if (g_ws_active && g_rt_main && vp) {
        D3DVIEWPORT8 v = *vp;
        if (vp->Y == 0 && vp->Height == g_rh && vp->Width * 2 == g_rw && (vp->X == 0 || vp->X * 2 == g_rw)) {
            g_split = 1;                                         /* 2-player split screen: keep the game's own full-width halves */
            if (g_dumping && g_dump) fprintf(g_dump, "  VIEWPORT split half (passed through)\n");
            return g_devOrig.SetViewport(This, vp);
        }
        if (vp->Width > g_rw || vp->Height > g_rh) {
            if (g_dumping && g_dump) fprintf(g_dump, "  VIEWPORT passthrough (bigger than the screen)\n");
            return g_devOrig.SetViewport(This, vp);
        } else if (vp->X == 0 && vp->Y == 0 && vp->Width == g_rw && vp->Height == g_rh) {
            v.Width = g_W; v.Height = g_H;                       /* full screen -> full screen */
            if (g_dumping && g_dump) fprintf(g_dump, "  VIEWPORT full -> %ux%u\n", g_W, g_H);
        } else {
            float kx = (float)g_vw / (float)g_rw, ky = (float)g_vh / (float)g_rh;
            float s = layout_scale();                              /* sub-viewport: through virtual space into the 4:3 box */
            float ox = ((float)g_W - (float)g_vw * s) * 0.5f, oy = ((float)g_H - (float)g_vh * s) * 0.5f;
            v.X = (DWORD)(vp->X * kx * s + ox); v.Y = (DWORD)(vp->Y * ky * s + oy);
            v.Width = (DWORD)(vp->Width * kx * s + 0.5f); v.Height = (DWORD)(vp->Height * ky * s + 0.5f);
            if (v.X + v.Width > g_W) v.Width = g_W - v.X;
            if (v.Y + v.Height > g_H) v.Height = g_H - v.Y;
        }
        return g_devOrig.SetViewport(This, &v);
    }
    return g_devOrig.SetViewport(This, vp);
}

/* Which game code sets the fog? Log the bytes around the call so the variables it reads can be found. */
static void fog_caller(int idx, const char *what, void *ra)
{
    static int n[2];
    unsigned char *a = (unsigned char *)ra;
    char hex[400];
    size_t pos = 0;
    int i;
    if (n[idx]++ >= 2) return;
    if (!addr_in_module(a, GetModuleHandleA(NULL))) { logf_("fog: %s set from %p (not inside the exe)\n", what, ra); return; }
    hex[0] = 0;
    for (i = -56; i < 8 && pos + 4 < sizeof hex; i++) pos += (size_t)snprintf(hex + pos, sizeof hex - pos, "%02X ", a[i]);
    logf_("fog: %s set by exe code returning to %p; bytes (56 before, 8 after): %s\n", what, ra, hex);
    {   /* the three numbers that code multiplies (addresses read from its bytes; only valid for this exe build) */
        HMODULE ex = GetModuleHandleA(NULL);
        const float *g = (const float *)0x00A729C4, *c1 = (const float *)0x0058930C, *c2 = (const float *)0x00589874;
        if (addr_in_module(g, ex) && addr_in_module(c1, ex) && addr_in_module(c2, ex))
            logf_("fog: value at A729C4 = %.3f, constant at 58930C = %.5f, constant at 589874 = %.5f\n", *g, *c1, *c2);
    }
}

static HRESULT STDMETHODCALLTYPE hk_SetRenderState(IDirect3DDevice8 *This, D3DRENDERSTATETYPE s, DWORD v)
{
    if (s == D3DRS_FOGSTART) fog_caller(0, "FOGSTART", __builtin_return_address(0));
    else if (s == D3DRS_FOGEND) fog_caller(1, "FOGEND", __builtin_return_address(0));
    if ((s == D3DRS_FOGENABLE || s == D3DRS_FOGCOLOR || s == D3DRS_FOGTABLEMODE || s == D3DRS_FOGSTART || s == D3DRS_FOGEND ||
         s == D3DRS_FOGDENSITY || s == D3DRS_RANGEFOGENABLE || s == D3DRS_FOGVERTEXMODE) && (unsigned)s < 160 && g_fog_logged < 60 &&
        (!g_fog_seen[s] || g_fog_last[s] != v)) {
        union { DWORD d; float f; } u;
        u.d = v;
        g_fog_seen[s] = 1; g_fog_last[s] = v; g_fog_logged++;
        if (s == D3DRS_FOGSTART || s == D3DRS_FOGEND || s == D3DRS_FOGDENSITY) logf_("fog: render state %d = %.3f\n", (int)s, u.f);
        else logf_("fog: render state %d = 0x%08lX\n", (int)s, (unsigned long)v);
    }
    return g_devOrig.SetRenderState(This, s, v);
}

static HRESULT STDMETHODCALLTYPE hk_SetTransform(IDirect3DDevice8 *This, D3DTRANSFORMSTATETYPE st, CONST D3DMATRIX *m)
{
    D3DMATRIX fm;                                       /* stretched-far-plane copy (must outlive the block that fills it) */
    if (g_dumping && g_dump && m && st == D3DTS_PROJECTION)
        fprintf(g_dump, "PROJECTION m11=%.5f m22=%.5f m33=%.5f m34=%.5f m43=%.5f\n",
                m->_11, m->_22, m->_33, m->_34, m->_43);

    if (st == D3DTS_PROJECTION && m && m->_34 != 0.0f && g_rt_main && !g_raw_set) {
        g_raw_m11 = m->_11; g_raw_m22 = m->_22; g_raw_set = 1;
    }

    /* perspective projections on the main screen: Hor+ (keep vertical FOV, widen horizontal) */
    { static int rawlog; if (st == D3DTS_PROJECTION && m && m->_34 != 0.0f && rawlog < 6) { rawlog++; logf_("projection raw: m33=%.9f m43=%.6f m34=%.3f m44=%.3f\n", m->_33, m->_43, m->_34, m->_44); } }
    if (st == D3DTS_PROJECTION && m && m->_34 != 0.0f && m->_33 > 0.0f && m->_33 != 1.0f) {      /* depth range of perspective projections */
        float zn = -m->_43 / m->_33, zf = m->_33 * zn / (m->_33 - 1.0f);
        if (g_depth_logged < 12) { g_depth_logged++; logf_("projection depth: near %.2f far %.2f (m33=%.6f m43=%.4f)\n", zn, zf, m->_33, m->_43); }
        if (g_far_pct > 100 && g_rt_main) {
            float zf2 = zf * (float)g_far_pct / 100.0f;
            fm = *m;
            fm._33 = zf2 / (zf2 - zn); fm._43 = -zn * fm._33;
            if (!(g_ws_active && g_fov_fix)) return g_devOrig.SetTransform(This, st, &fm);
            m = &fm;                                    /* the widescreen branch below keeps working on the stretched copy */
        }
    }
    if (g_ws_active && g_fov_fix && g_rt_main && st == D3DTS_PROJECTION && m && m->_34 != 0.0f) {
        D3DMATRIX n = *m;
        if (g_fov_fix == 2) n._22 *= (((float)g_W / (float)g_H) / ((float)g_vw / (float)g_vh));   /* Vert- */
        else                n._11 *= (((float)g_vw / (float)g_vh) / ((float)g_W / (float)g_H));   /* Hor+  */
        if (g_proj_logged < 10) {
            g_proj_logged++;
            logf_("projection (mode %d): game m11=%.4f m22=%.4f (ratio %.3f) -> m11=%.4f m22=%.4f (ratio %.3f)\n",
                  g_fov_fix, m->_11, m->_22, m->_22 / m->_11, n._11, n._22, n._22 / n._11);
        }
        return g_devOrig.SetTransform(This, st, &n);
    }
    return g_devOrig.SetTransform(This, st, m);
}

static HRESULT STDMETHODCALLTYPE hk_SetRenderTarget(IDirect3DDevice8 *This, IDirect3DSurface8 *rt, IDirect3DSurface8 *zs)
{
    if (rt) {
        D3DSURFACE_DESC d;
        if (SUCCEEDED(rt->lpVtbl->GetDesc(rt, &d))) {
            if (g_dumping && g_dump) fprintf(g_dump, "RENDERTARGET %ux%u fmt=%d\n", d.Width, d.Height, (int)d.Format);
            g_rt_main = (d.Width == g_W && d.Height == g_H) ? 1 : 0;
        }
    }
    return g_devOrig.SetRenderTarget(This, rt, zs);
}

/* diagnostics for the loading-screen video: how does the game push video frames into textures? */
static HRESULT STDMETHODCALLTYPE hk_UpdateTexture(IDirect3DDevice8 *This, IDirect3DBaseTexture8 *src, IDirect3DBaseTexture8 *dst)
{
    static int n;
    if (n < 6 || n % 300 == 0) logf_("UpdateTexture call #%d (%p -> %p)\n", n + 1, (void *)src, (void *)dst);
    n++;
    return g_devOrig.UpdateTexture(This, src, dst);
}

static HRESULT STDMETHODCALLTYPE hk_CopyRects(IDirect3DDevice8 *This, IDirect3DSurface8 *src, CONST RECT *sr, UINT c, IDirect3DSurface8 *dst, CONST POINT *dp)
{
    static int n;
    if (n < 6 || n % 300 == 0) logf_("CopyRects call #%d (%u rects, %p -> %p)\n", n + 1, c, (void *)src, (void *)dst);
    n++;
    return g_devOrig.CopyRects(This, src, sr, c, dst, dp);
}

/* more video diagnostics: does the game write pixels straight into surfaces (the back buffer) ? */
static IDirect3DSurface8Vtbl g_surfOrig, g_surfVt;
static int g_surfPatched;

static HRESULT STDMETHODCALLTYPE hk_SurfLockRect(IDirect3DSurface8 *This, D3DLOCKED_RECT *lr, CONST RECT *rect, DWORD fl)
{
    static int n;
    HRESULT hr = g_surfOrig.LockRect(This, lr, rect, fl);
    if (n < 12 || FAILED(hr) || n % 300 == 0)
        logf_("surface LockRect #%d on %p: flags 0x%lX rect %s -> hr 0x%08lX pitch %ld\n", n + 1, (void *)This, (unsigned long)fl, rect ? "partial" : "whole", (unsigned long)hr,
              (SUCCEEDED(hr) && lr) ? (long)lr->Pitch : -1L);
    n++;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_GetBackBuffer(IDirect3DDevice8 *This, UINT idx, D3DBACKBUFFER_TYPE type, IDirect3DSurface8 **out)
{
    static int n;
    HRESULT hr = g_devOrig.GetBackBuffer(This, idx, type, out);
    if (SUCCEEDED(hr) && out && *out) {
        IDirect3DSurface8 *sf = *out;
        if (!g_surfPatched) {
            memcpy(&g_surfOrig, sf->lpVtbl, sizeof g_surfOrig);
            memcpy(&g_surfVt, &g_surfOrig, sizeof g_surfVt);
            g_surfVt.LockRect = hk_SurfLockRect;
            g_surfPatched = 1;
        }
        sf->lpVtbl = &g_surfVt;
    }
    if (n++ < 6) logf_("GetBackBuffer(%u, %d) -> hr 0x%08lX %p\n", idx, (int)type, (unsigned long)hr, (out && *out) ? (void *)*out : NULL);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateImageSurface(IDirect3DDevice8 *This, UINT w, UINT h, D3DFORMAT fmt, IDirect3DSurface8 **out)
{
    static int n;
    HRESULT hr = g_devOrig.CreateImageSurface(This, w, h, fmt, out);
    if (n++ < 12) logf_("CreateImageSurface %ux%u format %u -> hr 0x%08lX\n", w, h, (unsigned)fmt, (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateTexture(IDirect3DDevice8 *This, UINT w, UINT h, UINT levels, DWORD usage,
                                                  D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture8 **out)
{
    HRESULT hr;
    unsigned idx;
    if (usage & D3DUSAGE_RENDERTARGET) scale_dim(&w, &h);
    hr = g_devOrig.CreateTexture(This, w, h, levels, usage, fmt, pool, out);
    { static int n; if ((usage || pool != D3DPOOL_MANAGED) && n++ < 60) logf_("texture create: %ux%u levels %u usage 0x%lX format %u pool %d -> hr 0x%08lX %p\n", w, h, levels, (unsigned long)usage, (unsigned)fmt, (int)pool, (unsigned long)hr, (out && *out) ? (void *)*out : NULL); }
    if (SUCCEEDED(hr) && out && *out) {
        idx = (unsigned)(((size_t)*out >> 4) & 1023);
        if (g_collect_all) seen_forget((IDirect3DBaseTexture8 *)*out);
        if (g_tc[idx].p == (void *)*out) g_tc[idx].p = NULL;
        if (g_rc[idx].p == (void *)*out) g_rc[idx].p = NULL;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateRenderTarget(IDirect3DDevice8 *This, UINT w, UINT h, D3DFORMAT fmt,
                                                       D3DMULTISAMPLE_TYPE ms, BOOL lockable, IDirect3DSurface8 **out)
{
    scale_dim(&w, &h);
    return g_devOrig.CreateRenderTarget(This, w, h, fmt, ms, lockable, out);
}

static HRESULT STDMETHODCALLTYPE hk_CreateDepthStencilSurface(IDirect3DDevice8 *This, UINT w, UINT h, D3DFORMAT fmt,
                                                              D3DMULTISAMPLE_TYPE ms, IDirect3DSurface8 **out)
{
    scale_dim(&w, &h);
    return g_devOrig.CreateDepthStencilSurface(This, w, h, fmt, ms, out);
}

static HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice8 *This, D3DPRESENT_PARAMETERS *pp)
{
    HRESULT hr;
    D3DPRESENT_PARAMETERS orig;
    int changed = 0;
    if (pp) { orig = *pp; changed = apply_resolution(pp); apply_overrides(pp); }
    hr = g_devOrig.Reset(This, pp);
    if (changed) {
        if (FAILED(hr)) {
            *pp = orig; g_ws_active = 0;
            logf_("Reset at %ux%u failed (0x%08lX), falling back to the game's own size\n", g_W, g_H, (unsigned long)hr);
            hr = g_devOrig.Reset(This, pp);
        } else g_ws_active = 1;
    }
    g_rt_main = 1;
    return hr;
}

static void patch_device(IDirect3DDevice8 *dev)
{
    if (dev->lpVtbl == &g_devVt) return;
    if (!g_devPatched) {
        memcpy(&g_devOrig, dev->lpVtbl, sizeof g_devOrig);
        memcpy(&g_devVt, &g_devOrig, sizeof g_devVt);
        g_devVt.Present = hk_Present;
        g_devVt.Reset = hk_Reset;
        g_devVt.SetVertexShader = hk_SetVertexShader;
        g_devVt.SetTexture = hk_SetTexture;
        g_devVt.DrawPrimitiveUP = hk_DrawPrimitiveUP;
        g_devVt.DrawIndexedPrimitiveUP = hk_DrawIndexedPrimitiveUP;
        g_devVt.DrawPrimitive = hk_DrawPrimitive;
        g_devVt.DrawIndexedPrimitive = hk_DrawIndexedPrimitive;
        g_devVt.SetViewport = hk_SetViewport;
        g_devVt.SetTransform = hk_SetTransform;
        g_devVt.SetRenderState = hk_SetRenderState;
        g_devVt.SetRenderTarget = hk_SetRenderTarget;
        g_devVt.CreateTexture = hk_CreateTexture;
        g_devVt.UpdateTexture = hk_UpdateTexture;
        g_devVt.GetBackBuffer = hk_GetBackBuffer;
        g_devVt.CreateImageSurface = hk_CreateImageSurface;
        g_devVt.CopyRects = hk_CopyRects;
        g_devVt.CreateRenderTarget = hk_CreateRenderTarget;
        g_devVt.CreateDepthStencilSurface = hk_CreateDepthStencilSurface;
        g_devPatched = 1;
    }
    dev->lpVtbl = &g_devVt;
    logf_("device patched\n");
    cull_patch_try();
    viewdist_patch_try();
}

/* -------------------------------------------------------- IDirect3D8 hook */

static HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D8 *This, UINT adapter, D3DDEVTYPE type, HWND hwnd,
                                                 DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice8 **out)
{
    HRESULT hr;
    D3DPRESENT_PARAMETERS orig;
    int changed = 0;

    if (pp) { orig = *pp; changed = apply_resolution(pp); apply_overrides(pp); }
    hr = g_d3dOrig.CreateDevice(This, adapter, type, hwnd, flags, pp, out);
    if (changed) {
        if (FAILED(hr)) {
            *pp = orig; g_ws_active = 0;
            logf_("CreateDevice at %ux%u failed (0x%08lX), falling back to the game's own size\n",
                  g_W, g_H, (unsigned long)hr);
            hr = g_d3dOrig.CreateDevice(This, adapter, type, hwnd, flags, pp, out);
        } else g_ws_active = 1;
    }
    if (pp)
        logf_("CreateDevice hr=0x%08lX backbuffer=%ux%u windowed=%d widescreen=%d\n",
              (unsigned long)hr, pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->Windowed, g_ws_active);
    if (pp)
        logf_("CreateDevice parameters: format %u, %u back buffer(s), multisample %u, swap effect %u, flags 0x%lX, refresh %u, interval 0x%lX, depth %d/%u, behaviour 0x%lX, type %d\n",
              (unsigned)pp->BackBufferFormat, pp->BackBufferCount, (unsigned)pp->MultiSampleType, (unsigned)pp->SwapEffect, (unsigned long)pp->Flags,
              pp->FullScreen_RefreshRateInHz, (unsigned long)pp->FullScreen_PresentationInterval, (int)pp->EnableAutoDepthStencil, (unsigned)pp->AutoDepthStencilFormat,
              (unsigned long)flags, (int)type);
    if (g_ov_windowed && SUCCEEDED(hr) && hwnd && pp) {                       /* borderless window covering the screen */
        SetWindowLongA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, (int)pp->BackBufferWidth, (int)pp->BackBufferHeight, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    g_rt_main = 1;
    if (SUCCEEDED(hr) && out && *out) patch_device(*out);
    return hr;
}

/* ------------------------------------------------------- exported entry */

EXPORT IDirect3D8 *WINAPI Direct3DCreate8(UINT sdk)
{
    typedef IDirect3D8 *(WINAPI *PFN)(UINT);
    PFN real;
    IDirect3D8 *d3d;

    init_once();
    if (!g_real) {
        char path[MAX_PATH];
        if (!g_realname[0]) {                                   /* nothing configured: use d3d8to9 if it is next to us */
            snprintf(path, sizeof path, "%sd3d8to9.dll", g_base);
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) strcpy(g_realname, "d3d8to9.dll");
        }
        if (g_realname[0]) {
            snprintf(path, sizeof path, "%s%s", g_base, g_realname);   /* e.g. d3d8to9.dll or a renamed dgVoodoo */
        } else {
            GetSystemDirectoryA(path, MAX_PATH);      /* SysWOW64 for a 32-bit process */
            strcat(path, "\\d3d8.dll");
        }
        g_real = LoadLibraryA(path);
        logf_("loading real d3d8: %s -> %p\n", path, (void *)g_real);
        if (!g_real && g_realname[0]) {                         /* named file missing or broken: fall back to the system one */
            GetSystemDirectoryA(path, MAX_PATH);
            strcat(path, "\\d3d8.dll");
            g_real = LoadLibraryA(path);
            logf_("fallback: loading the system d3d8 instead: %s -> %p\n", path, (void *)g_real);
        }
        if (!g_real) return NULL;
    }
    real = (PFN)GetProcAddress(g_real, "Direct3DCreate8");
    if (!real) return NULL;
    d3d = real(sdk);
    if (!d3d) return NULL;

    if (!g_d3dPatched) {
        memcpy(&g_d3dOrig, d3d->lpVtbl, sizeof g_d3dOrig);
        memcpy(&g_d3dVt, &g_d3dOrig, sizeof g_d3dVt);
        g_d3dVt.CreateDevice = hk_CreateDevice;
        g_d3dPatched = 1;
    }
    d3d->lpVtbl = &g_d3dVt;
    return d3d;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        char ip[MAX_PATH], *sl;
        g_self = h; DisableThreadLibraryCalls(h);
        /* redirect the game's DirectInput8Create import as early as possible (it may create DirectInput before Direct3D) */
        GetModuleFileNameA(h, ip, MAX_PATH);
        sl = strrchr(ip, '\\');
        if (sl) { strcpy(sl + 1, "THPS4Plus.ini"); g_vin_quiet = 1; vin_config(ip); dsound_config(ip); vin_install(); dsound_install(); }
    }
    if (reason == DLL_PROCESS_DETACH && g_dump) fclose(g_dump);
    return TRUE;
}
