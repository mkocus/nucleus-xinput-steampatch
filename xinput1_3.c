/*
 * xinput1_3 proxy DLL with Steam overlay counter-hook.
 *
 * Drop-in replacement for xinput1_3.dll that filters XInput controllers
 * per-instance using an INI file, AND survives Steam overlay function hooking.
 *
 * Problem:  Steam overlay (gameoverlayrenderer.dll) patches the first bytes of
 *           XInputGetState with a JMP to its own handler, bypassing any proxy
 *           DLL filtering.  It does this even when the overlay is disabled in
 *           Steam settings.
 *
 * Solution: A background thread waits for the overlay to install its hook, then
 *           overwrites the JMP target to point at our filtering trampoline.
 *           Our trampoline applies the controller map and calls Steam's handler
 *           with the remapped index.
 *
 * Configuration:  XInputPlus.ini in the same directory as this DLL.
 *
 *   [ControllerNumber]
 *   Controller1=1        ; virtual pad 0 -> physical pad 0
 *   Controller2=0        ; virtual pad 1 -> disconnected
 *   Controller3=0
 *   Controller4=0
 *
 * Build (MSVC):
 *   cl /nologo /O2 /W3 /c xinput1_3.c
 *   link /nologo /DLL /DEF:xinput1_3.def /OUT:xinput1_3.dll /INCREMENTAL:NO
 *        xinput1_3.obj kernel32.lib user32.lib
 *
 * For x64, run from an x64 VS command prompt.
 * For xinput1_4.dll, use xinput1_4.def instead.
 *
 * License: MIT
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string.h>
#include <stdio.h>

/* ---- XInput types (no SDK dependency) ---- */

typedef struct {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XINPUT_GAMEPAD;

typedef struct {
    DWORD          dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE;

typedef struct {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
} XINPUT_VIBRATION;

typedef struct {
    BYTE             Type;
    BYTE             SubType;
    WORD             Flags;
    XINPUT_GAMEPAD   Gamepad;
    XINPUT_VIBRATION Vibration;
} XINPUT_CAPABILITIES;

typedef struct { BYTE Type; BYTE Level; }             XINPUT_BATTERY_INFORMATION;
typedef struct { WORD VirtualKey; WCHAR Unicode;
                 WORD Flags; BYTE UserIndex; BYTE HidCode; } XINPUT_KEYSTROKE;

#define XUSER_MAX_COUNT            4
#define ERROR_DEVICE_NOT_CONNECTED 1167L

/* ---- Function pointer types ---- */

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE*);
typedef DWORD (WINAPI *PFN_XInputSetState)(DWORD, XINPUT_VIBRATION*);
typedef DWORD (WINAPI *PFN_XInputGetCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES*);
typedef void  (WINAPI *PFN_XInputEnable)(BOOL);
typedef DWORD (WINAPI *PFN_XInputGetDSoundAudioDeviceGuids)(DWORD, GUID*, GUID*);
typedef DWORD (WINAPI *PFN_XInputGetBatteryInformation)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
typedef DWORD (WINAPI *PFN_XInputGetKeystroke)(DWORD, DWORD, XINPUT_KEYSTROKE*);
typedef DWORD (WINAPI *PFN_XInputGetAudioDeviceIds)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*);
typedef DWORD (WINAPI *PFN_XInputGetStateEx)(DWORD, XINPUT_STATE*);

/* ---- Hook size depends on architecture ---- */

#ifdef _WIN64
#define HOOK_SIZE 14   /* FF 25 [00 00 00 00] [8-byte addr] */
#else
#define HOOK_SIZE 5    /* E9 [rel32] */
#endif

/* ---- Globals ---- */

static HMODULE g_hReal;
static wchar_t g_our_dll_name[32];  /* "xinput1_3.dll" or "xinput1_4.dll" */

static PFN_XInputGetState                   pXInputGetState;
static PFN_XInputSetState                   pXInputSetState;
static PFN_XInputGetCapabilities            pXInputGetCapabilities;
static PFN_XInputEnable                     pXInputEnable;
static PFN_XInputGetDSoundAudioDeviceGuids  pXInputGetDSoundAudioDeviceGuids;
static PFN_XInputGetBatteryInformation      pXInputGetBatteryInformation;
static PFN_XInputGetKeystroke               pXInputGetKeystroke;
static PFN_XInputGetAudioDeviceIds          pXInputGetAudioDeviceIds;
static PFN_XInputGetStateEx                 pXInputGetStateEx;

/* g_map[virtual] = physical (0-3), or -1 = disconnected */
static int g_map[XUSER_MAX_COUNT] = { -1, -1, -1, -1 };

/* Steam overlay handler addresses captured by counter-hook */
static PFN_XInputGetState   g_steam_getstate;
static PFN_XInputGetStateEx g_steam_getstateex;

/* ---- Optional debug log ---- */

static wchar_t g_log_path[MAX_PATH];

static void dbg_log(const char *fmt, ...)
{
    FILE *f;
    SYSTEMTIME st;
    va_list ap;
    if (!g_log_path[0]) return;
    f = _wfopen(g_log_path, L"a");
    if (!f) return;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---- INI parsing ---- */

static void parse_ini(const wchar_t *path)
{
    static const wchar_t *keys[] = {
        L"Controller1", L"Controller2", L"Controller3", L"Controller4"
    };
    int i;
    for (i = 0; i < XUSER_MAX_COUNT; i++) {
        int v = GetPrivateProfileIntW(L"ControllerNumber", keys[i], 0, path);
        g_map[i] = (v >= 1 && v <= 4) ? v - 1 : -1;
        dbg_log("  %ls = %d => g_map[%d] = %d",
                keys[i], v, i, g_map[i]);
    }
}

/* ---- Load real xinput1_4.dll from System32 ---- */

static BOOL load_real(void)
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n + 15 >= MAX_PATH) return FALSE;
    wcscat(path, L"\\xinput1_4.dll");

    g_hReal = LoadLibraryW(path);
    if (!g_hReal) { dbg_log("Failed to load %ls", path); return FALSE; }

    pXInputGetState        = (PFN_XInputGetState)        GetProcAddress(g_hReal, "XInputGetState");
    pXInputSetState        = (PFN_XInputSetState)        GetProcAddress(g_hReal, "XInputSetState");
    pXInputGetCapabilities = (PFN_XInputGetCapabilities) GetProcAddress(g_hReal, "XInputGetCapabilities");
    pXInputEnable          = (PFN_XInputEnable)          GetProcAddress(g_hReal, "XInputEnable");
    pXInputGetDSoundAudioDeviceGuids = (PFN_XInputGetDSoundAudioDeviceGuids)
                             GetProcAddress(g_hReal, "XInputGetDSoundAudioDeviceGuids");
    pXInputGetBatteryInformation = (PFN_XInputGetBatteryInformation)
                             GetProcAddress(g_hReal, "XInputGetBatteryInformation");
    pXInputGetKeystroke    = (PFN_XInputGetKeystroke)    GetProcAddress(g_hReal, "XInputGetKeystroke");
    pXInputGetAudioDeviceIds = (PFN_XInputGetAudioDeviceIds)
                             GetProcAddress(g_hReal, "XInputGetAudioDeviceIds");
    pXInputGetStateEx      = (PFN_XInputGetStateEx)      GetProcAddress(g_hReal, (LPCSTR)(ULONG_PTR)100);

    dbg_log("Real xinput1_4.dll loaded at %p", g_hReal);
    return pXInputGetState != NULL;
}

/* ---- Controller index mapping ---- */

static BOOL map_index(DWORD in, DWORD *out)
{
    if (in >= XUSER_MAX_COUNT || g_map[in] < 0) return FALSE;
    *out = (DWORD)g_map[in];
    return TRUE;
}

/* ---- Steam overlay counter-hook ----
 *
 * Steam overlay hooks our exported XInputGetState with a JMP rel32 (E9 xx xx xx xx).
 * Its handler calls system XInput directly, bypassing our controller filtering.
 *
 * We counter this by:
 *  1. Waiting 3 seconds for Steam to install its hooks
 *  2. Reading the JMP target (Steam's handler address)
 *  3. Overwriting the JMP to point at our trampoline instead
 *  4. Our trampoline applies g_map[] filtering, then calls Steam's handler
 *
 * This preserves Steam's internal state while restoring our filtering.
 */

/* Forward declarations */
__declspec(dllexport) DWORD WINAPI XInputGetState(DWORD, XINPUT_STATE*);
__declspec(dllexport) DWORD WINAPI XInputGetStateEx(DWORD, XINPUT_STATE*);

static DWORD WINAPI hooked_getstate(DWORD idx, XINPUT_STATE *pState)
{
    PFN_XInputGetState fn = g_steam_getstate ? g_steam_getstate : pXInputGetState;
    DWORD real;
    if (!fn || !map_index(idx, &real)) {
        if (pState) memset(pState, 0, sizeof(*pState));
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    return fn(real, pState);
}

static DWORD WINAPI hooked_getstateex(DWORD idx, XINPUT_STATE *pState)
{
    PFN_XInputGetStateEx fn = g_steam_getstateex ? g_steam_getstateex : pXInputGetStateEx;
    DWORD real;
    if (!fn || !map_index(idx, &real)) {
        if (pState) memset(pState, 0, sizeof(*pState));
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    return fn(real, pState);
}

static void *get_jmp_target(BYTE *fn)
{
    /* E9 rel32 — used on x86, and x64 when target is within ±2GB */
    if (fn[0] == 0xE9)
        return fn + 5 + *(int*)(fn + 1);
#ifdef _WIN64
    /* FF 25 00 00 00 00 [8-byte addr] — common x64 detour (RIP-relative) */
    if (fn[0] == 0xFF && fn[1] == 0x25 &&
        fn[2] == 0 && fn[3] == 0 && fn[4] == 0 && fn[5] == 0)
        return *(void**)(fn + 6);
    /* 48 B8 [imm64] FF E0 — MOV RAX, addr; JMP RAX */
    if (fn[0] == 0x48 && fn[1] == 0xB8 && fn[10] == 0xFF && fn[11] == 0xE0)
        return *(void**)(fn + 2);
#endif
    return NULL;
}

static BOOL write_jmp(BYTE *fn, void *target)
{
    DWORD old;
#ifdef _WIN64
    /* Use 14-byte absolute indirect JMP for x64 (unlimited range) */
    if (!VirtualProtect(fn, 14, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    fn[0] = 0xFF;
    fn[1] = 0x25;
    *(DWORD*)(fn + 2) = 0;  /* RIP+0 → address follows immediately */
    *(void**)(fn + 6) = target;
    VirtualProtect(fn, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 14);
#else
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    fn[0] = 0xE9;
    *(int*)(fn + 1) = (int)((BYTE*)target - fn - 5);
    VirtualProtect(fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
#endif
    return TRUE;
}

static DWORD WINAPI counter_hook_thread(LPVOID param)
{
    BYTE *fn_gs, *fn_gse;
    void *target;
    int i;
    (void)param;

    Sleep(3000);

    fn_gs  = (BYTE*)GetProcAddress(GetModuleHandleW(g_our_dll_name), "XInputGetState");
    fn_gse = (BYTE*)GetProcAddress(GetModuleHandleW(g_our_dll_name), (LPCSTR)(ULONG_PTR)100);

    dbg_log("Counter-hook: XInputGetState at %p bytes=%02X %02X %02X %02X %02X %02X",
            fn_gs, fn_gs[0], fn_gs[1], fn_gs[2], fn_gs[3], fn_gs[4], fn_gs[5]);

    /* XInputGetState */
    if (fn_gs && (target = get_jmp_target(fn_gs)) != NULL) {
        g_steam_getstate = (PFN_XInputGetState)target;
        write_jmp(fn_gs, hooked_getstate);
        dbg_log("Counter-hooked XInputGetState: Steam(%p) -> filter(%p)", target, hooked_getstate);
    } else {
        dbg_log("XInputGetState not hooked by Steam, proxy works directly");
    }

    /* XInputGetStateEx (ordinal 100) */
    if (fn_gse && (target = get_jmp_target(fn_gse)) != NULL) {
        g_steam_getstateex = (PFN_XInputGetStateEx)target;
        write_jmp(fn_gse, hooked_getstateex);
        dbg_log("Counter-hooked XInputGetStateEx");
    }

    /* Monitor for re-hooks (60 seconds) */
    for (i = 0; i < 60; i++) {
        Sleep(1000);
        if (fn_gs) {
            void *cur = get_jmp_target(fn_gs);
            if (cur && cur != (void*)hooked_getstate) {
                g_steam_getstate = (PFN_XInputGetState)cur;
                write_jmp(fn_gs, hooked_getstate);
                dbg_log("Re-hooked XInputGetState (pass %d)", i);
            }
        }
    }
    return 0;
}

/* ---- DllMain ---- */

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t dir[MAX_PATH], ini[MAX_PATH];
        wchar_t *sep;
        DWORD len;
        size_t dlen;

        DisableThreadLibraryCalls(hInst);

        len = GetModuleFileNameW(hInst, dir, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return FALSE;
        sep = wcsrchr(dir, L'\\');
        if (!sep) return FALSE;
        dlen = (size_t)(sep - dir);

        /* Log file (same directory as DLL) */
        wcsncpy(g_log_path, dir, dlen);
        g_log_path[dlen] = 0;
        wcscat(g_log_path, L"\\xinput_proxy.log");

        /* Extract our DLL filename for GetModuleHandle later */
        {
            const wchar_t *fname = wcsrchr(dir, L'\\');
            if (fname) fname++; else fname = dir;
            wcsncpy(g_our_dll_name, fname, 31);
            g_our_dll_name[31] = 0;
        }

        dbg_log("=== XInput proxy loaded (PID %lu) ===", GetCurrentProcessId());
        dbg_log("DLL: %ls", dir);

        /* INI file */
        wcsncpy(ini, dir, dlen);
        ini[dlen] = 0;
        wcscat(ini, L"\\XInputPlus.ini");
        parse_ini(ini);

        if (!load_real()) return FALSE;

        CreateThread(NULL, 0, counter_hook_thread, NULL, 0, NULL);

    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_hReal) { FreeLibrary(g_hReal); g_hReal = NULL; }
    }
    return TRUE;
}

/* ---- Exported functions ---- */

__declspec(dllexport) DWORD WINAPI
XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState)
{
    DWORD real;
    if (!pXInputGetState || !map_index(dwUserIndex, &real)) {
        if (pState) memset(pState, 0, sizeof(*pState));
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    return pXInputGetState(real, pState);
}

__declspec(dllexport) DWORD WINAPI
XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration)
{
    DWORD real;
    if (!pXInputSetState || !map_index(dwUserIndex, &real))
        return ERROR_DEVICE_NOT_CONNECTED;
    return pXInputSetState(real, pVibration);
}

__declspec(dllexport) DWORD WINAPI
XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags,
                      XINPUT_CAPABILITIES *pCaps)
{
    DWORD real;
    if (!pXInputGetCapabilities || !map_index(dwUserIndex, &real)) {
        if (pCaps) memset(pCaps, 0, sizeof(*pCaps));
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    return pXInputGetCapabilities(real, dwFlags, pCaps);
}

__declspec(dllexport) void WINAPI XInputEnable(BOOL enable)
{
    if (pXInputEnable) pXInputEnable(enable);
}

__declspec(dllexport) DWORD WINAPI
XInputGetDSoundAudioDeviceGuids(DWORD dwUserIndex,
                                GUID *pRender, GUID *pCapture)
{
    DWORD real;
    if (!pXInputGetDSoundAudioDeviceGuids || !map_index(dwUserIndex, &real))
        return ERROR_DEVICE_NOT_CONNECTED;
    return pXInputGetDSoundAudioDeviceGuids(real, pRender, pCapture);
}

__declspec(dllexport) DWORD WINAPI
XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType,
                            XINPUT_BATTERY_INFORMATION *pBatt)
{
    DWORD real;
    if (!pXInputGetBatteryInformation || !map_index(dwUserIndex, &real)) {
        if (pBatt) memset(pBatt, 0, sizeof(*pBatt));
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    return pXInputGetBatteryInformation(real, devType, pBatt);
}

__declspec(dllexport) DWORD WINAPI
XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved,
                   XINPUT_KEYSTROKE *pKeystroke)
{
    DWORD real;
    if (dwUserIndex == 0xFF) return ERROR_DEVICE_NOT_CONNECTED;
    if (!pXInputGetKeystroke || !map_index(dwUserIndex, &real))
        return ERROR_DEVICE_NOT_CONNECTED;
    return pXInputGetKeystroke(real, dwReserved, pKeystroke);
}

__declspec(dllexport) DWORD WINAPI
XInputGetAudioDeviceIds(DWORD dwUserIndex,
                        LPWSTR pRender, UINT *pRenderCount,
                        LPWSTR pCapture, UINT *pCaptureCount)
{
    DWORD real;
    if (!pXInputGetAudioDeviceIds || !map_index(dwUserIndex, &real))
        return ERROR_DEVICE_NOT_CONNECTED;
    return pXInputGetAudioDeviceIds(real, pRender, pRenderCount,
                                    pCapture, pCaptureCount);
}

__declspec(dllexport) DWORD WINAPI
XInputGetStateEx(DWORD dwUserIndex, XINPUT_STATE *pState)
{
    DWORD real;
    if (!pXInputGetStateEx || !map_index(dwUserIndex, &real)) {
        if (pState) memset(pState, 0, sizeof(*pState));
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    return pXInputGetStateEx(real, pState);
}
