/*
 * dsound_trace.h - THPS4+ audio, step 0: learn how the game plays its music.
 * The game imports DSOUND.dll. We redirect its DirectSoundCreate / DirectSoundCreate8 import (by name or by ordinal),
 * give the sound object and every buffer it creates a private vtable copy, and LOG what the game does with them
 * (buffer sizes and formats, Play / Stop / Lock / SetVolume calls). Nothing is changed or muted yet.
 * The big buffer that is Locked over and over is the music stream; that is the one a later step will silence
 * so that our own player can take over.
 *
 * Needs windows.h, logf_(), g_vin_quiet / VLOG from dinput_virtual.h, and <dsound.h>. [Audio] Trace=1 in the ini.
 */
#ifndef DSOUND_TRACE_H
#define DSOUND_TRACE_H
#include <dsound.h>

static int g_ds_trace = 1;

typedef struct { void *obj; int id; DWORD flags, bytes, rate; WORD ch, bits; int plays, stops, locks, setvol, setpos, released; DWORD lockbytes; } DsBuf;
#define DS_MAXBUF 512
static DsBuf g_dsb[DS_MAXBUF];
static int g_dsb_count, g_dsb_id;
#define DS_BIG 65536u                                   /* buffers at least this big are treated as streaming candidates */

static DsBuf *ds_find(void *obj)
{
    int i;
    for (i = g_dsb_count - 1; i >= 0; i--) if (g_dsb[i].obj == obj && !g_dsb[i].released) return &g_dsb[i];
    return NULL;
}

typedef HRESULT (WINAPI *DS_CreateBuf)(void *, const DSBUFFERDESC *, LPDIRECTSOUNDBUFFER *, LPUNKNOWN);
typedef HRESULT (WINAPI *DS_Lock)(IDirectSoundBuffer *, DWORD, DWORD, LPVOID *, LPDWORD, LPVOID *, LPDWORD, DWORD);
typedef HRESULT (WINAPI *DS_Play)(IDirectSoundBuffer *, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *DS_Stop)(IDirectSoundBuffer *);
typedef HRESULT (WINAPI *DS_SetVol)(IDirectSoundBuffer *, LONG);
typedef HRESULT (WINAPI *DS_SetPos)(IDirectSoundBuffer *, DWORD);
typedef ULONG (WINAPI *DS_Release)(IDirectSoundBuffer *);
static DS_CreateBuf ds_o_create;
static DS_Lock ds_o_lock;
static DS_Play ds_o_play;
static DS_Stop ds_o_stop;
static DS_SetVol ds_o_setvol;
static DS_SetPos ds_o_setpos;
static DS_Release ds_o_release;

static HRESULT WINAPI dsh_Lock(IDirectSoundBuffer *self, DWORD off, DWORD n, LPVOID *p1, LPDWORD b1, LPVOID *p2, LPDWORD b2, DWORD fl)
{
    DsBuf *d = ds_find(self);
    if (d) {
        d->locks++; d->lockbytes += n;
        if (g_ds_trace && d->bytes >= DS_BIG && (d->locks <= 6 || d->locks % 200 == 0))
            logf_("dsound: buffer #%d Lock #%d offset %lu, %lu bytes (flags 0x%lX)\n", d->id, d->locks, (unsigned long)off, (unsigned long)n, (unsigned long)fl);
    }
    return ds_o_lock(self, off, n, p1, b1, p2, b2, fl);
}

static HRESULT WINAPI dsh_Play(IDirectSoundBuffer *self, DWORD r, DWORD prio, DWORD fl)
{
    DsBuf *d = ds_find(self);
    if (d) {
        d->plays++;
        if (g_ds_trace && d->bytes >= DS_BIG && d->plays <= 8)
            logf_("dsound: buffer #%d Play #%d (priority %lu, flags 0x%lX%s)\n", d->id, d->plays, (unsigned long)prio, (unsigned long)fl, (fl & DSBPLAY_LOOPING) ? ", looping" : "");
    }
    return ds_o_play(self, r, prio, fl);
}

static HRESULT WINAPI dsh_Stop(IDirectSoundBuffer *self)
{
    DsBuf *d = ds_find(self);
    if (d) {
        d->stops++;
        if (g_ds_trace && d->bytes >= DS_BIG && d->stops <= 8) logf_("dsound: buffer #%d Stop #%d\n", d->id, d->stops);
    }
    return ds_o_stop(self);
}

static HRESULT WINAPI dsh_SetVolume(IDirectSoundBuffer *self, LONG v)
{
    DsBuf *d = ds_find(self);
    if (d) {
        d->setvol++;
        if (g_ds_trace && d->bytes >= DS_BIG && d->setvol <= 8) logf_("dsound: buffer #%d SetVolume %ld\n", d->id, (long)v);
    }
    return ds_o_setvol(self, v);
}

static HRESULT WINAPI dsh_SetCurrentPosition(IDirectSoundBuffer *self, DWORD p)
{
    DsBuf *d = ds_find(self);
    if (d) d->setpos++;
    return ds_o_setpos(self, p);
}

static ULONG WINAPI dsh_Release(IDirectSoundBuffer *self)
{
    DsBuf *d = ds_find(self);
    ULONG r;
    if (d && g_ds_trace && d->bytes >= DS_BIG)
        logf_("dsound: buffer #%d summary before Release: %d plays, %d stops, %d locks (%lu bytes), %d SetVolume, %d SetCurrentPosition\n",
              d->id, d->plays, d->stops, d->locks, (unsigned long)d->lockbytes, d->setvol, d->setpos);
    r = ds_o_release(self);
    if (d && r == 0) d->released = 1;
    return r;
}

static HRESULT WINAPI dsh_CreateSoundBuffer(void *self, const DSBUFFERDESC *desc, LPDIRECTSOUNDBUFFER *pp, LPUNKNOWN outer)
{
    HRESULT hr = ds_o_create(self, desc, pp, outer);
    if (SUCCEEDED(hr) && pp && *pp && desc) {
        IDirectSoundBuffer *b = *pp;
        DsBuf *d = NULL;
        if (g_dsb_count < DS_MAXBUF) {
            d = &g_dsb[g_dsb_count++];
            memset(d, 0, sizeof *d);
            d->obj = b; d->id = ++g_dsb_id; d->flags = desc->dwFlags; d->bytes = desc->dwBufferBytes;
            if (desc->lpwfxFormat) { d->rate = desc->lpwfxFormat->nSamplesPerSec; d->ch = desc->lpwfxFormat->nChannels; d->bits = desc->lpwfxFormat->wBitsPerSample; }
        }
        if (b->lpVtbl != NULL && d) {
            IDirectSoundBufferVtbl *nv = (IDirectSoundBufferVtbl *)malloc(sizeof *nv);
            if (nv) {
                *nv = *b->lpVtbl;
                if (!ds_o_lock) { ds_o_lock = (DS_Lock)nv->Lock; ds_o_play = (DS_Play)nv->Play; ds_o_stop = (DS_Stop)nv->Stop;
                                  ds_o_setvol = (DS_SetVol)nv->SetVolume; ds_o_setpos = (DS_SetPos)nv->SetCurrentPosition; ds_o_release = (DS_Release)nv->Release; }
                nv->Lock = (void *)dsh_Lock; nv->Play = (void *)dsh_Play; nv->Stop = (void *)dsh_Stop;
                nv->SetVolume = (void *)dsh_SetVolume; nv->SetCurrentPosition = (void *)dsh_SetCurrentPosition; nv->Release = (void *)dsh_Release;
                b->lpVtbl = nv;
            }
        }
        if (g_ds_trace && d && (d->bytes >= DS_BIG || g_dsb_id <= 40))
            logf_("dsound: buffer #%d created: %lu bytes, %lu Hz, %u ch, %u bit, flags 0x%lX\n", d->id, (unsigned long)d->bytes, (unsigned long)d->rate, (unsigned)d->ch, (unsigned)d->bits, (unsigned long)d->flags);
    }
    return hr;
}

typedef HRESULT (WINAPI *DS_Create)(LPCGUID, LPVOID *, LPUNKNOWN);
static DS_Create ds_o_dscreate, ds_o_dscreate8;
static int ds_created_objects;

static void ds_hook_object(void *obj, int is8)
{
    /* slot 3 of IDirectSound / IDirectSound8 is CreateSoundBuffer in both */
    void ***pp = (void ***)obj;
    size_t n = is8 ? sizeof(IDirectSound8Vtbl) : sizeof(IDirectSoundVtbl);
    void **nv = (void **)malloc(n);
    if (!nv) return;
    memcpy(nv, *pp, n);
    if (!ds_o_create) ds_o_create = (DS_CreateBuf)nv[3];
    nv[3] = (void *)dsh_CreateSoundBuffer;
    *pp = nv;
    ds_created_objects++;
    VLOG("dsound: DirectSound%s object hooked\n", is8 ? "8" : "");
}

static HRESULT WINAPI dsh_DirectSoundCreate(LPCGUID g, LPVOID *pp, LPUNKNOWN outer)
{
    HRESULT hr = ds_o_dscreate(g, pp, outer);
    if (SUCCEEDED(hr) && pp && *pp) ds_hook_object(*pp, 0);
    return hr;
}
static HRESULT WINAPI dsh_DirectSoundCreate8(LPCGUID g, LPVOID *pp, LPUNKNOWN outer)
{
    HRESULT hr = ds_o_dscreate8(g, pp, outer);
    if (SUCCEEDED(hr) && pp && *pp) ds_hook_object(*pp, 1);
    return hr;
}

/* redirect one import of the exe, found by name or by ordinal */
static int ds_patch_import(const char *func, WORD ordinal, void *newf, void **orig)
{
    HMODULE m = GetModuleHandleA(NULL), lib;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)m + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d;
    if (!dd->VirtualAddress) return 0;
    lib = GetModuleHandleA("dsound.dll");
    if (!lib) lib = LoadLibraryA("dsound.dll");
    if (!lib) return 0;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)m + dd->VirtualAddress); d->Name; d++) {
        IMAGE_THUNK_DATA *oft, *ft;
        if (lstrcmpiA((const char *)((BYTE *)m + d->Name), "dsound.dll") != 0) continue;
        oft = (IMAGE_THUNK_DATA *)((BYTE *)m + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        ft = (IMAGE_THUNK_DATA *)((BYTE *)m + d->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            DWORD old;
            int match;
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) match = (IMAGE_ORDINAL(oft->u1.Ordinal) == ordinal);
            else match = (strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)((BYTE *)m + oft->u1.AddressOfData))->Name, func) == 0);
            if (!match) continue;
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

static void dsound_config(const char *ini) { g_ds_trace = (int)GetPrivateProfileIntA("Audio", "Trace", 1, ini); }

static void dsound_install(void)
{
    static int ok1, ok2;
    if (!g_ds_trace) return;
    if (!ok1) ok1 = ds_patch_import("DirectSoundCreate", 1, (void *)dsh_DirectSoundCreate, (void **)&ds_o_dscreate);
    if (!ok2) ok2 = ds_patch_import("DirectSoundCreate8", 11, (void *)dsh_DirectSoundCreate8, (void **)&ds_o_dscreate8);
    VLOG("dsound: DirectSoundCreate import %s, DirectSoundCreate8 import %s\n", ok1 ? "redirected" : "not found", ok2 ? "redirected" : "not found");
}
#endif
