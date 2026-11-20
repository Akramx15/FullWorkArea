#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

static const WCHAR kClassName[] = L"FullWorkArea_Daemon_v3";
static const WCHAR kMutexName[] = L"Local\\FullWorkArea_Daemon_v3_Mutex";
static const WCHAR kBridgeClass[] = L"Windows.UI.Composition.DesktopWindowContentBridge";

#define TIMER_WORKAREA 1
#define TIMER_TRAY     2
#define TIMER_VERIFY   3
#define WM_REFRESH_NOW (WM_APP + 1)

#define HK_TOGGLE_HIDE   1
#define HK_TOGGLE_CENTER 2

static HWND g_hwnd = NULL;
static UINT g_msgTaskbarCreated = 0;

static BOOL g_applyingWork = FALSE;
static ULONGLONG g_workBurstStart = 0;
static DWORD g_workBurstCount = 0;

static HWND g_hTray = NULL;
static HWINEVENTHOOK g_hTrayHook = NULL;
static BOOL g_centerMode = TRUE;
static BOOL g_hidden = FALSE;
static BOOL g_applyingTray = FALSE;
static ULONGLONG g_trayBurstStart = 0;
static DWORD g_trayBurstCount = 0;
static int g_lastShift = -1;
static int g_lastMonW = 0;
static int g_lastTrayH = 42;

static void TrimWorkingSet(void) {
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static BOOL StrEqW(LPCWSTR a, LPCWSTR b) {
    while (*a && *b) {
        if (*a != *b) return FALSE;
        ++a;
        ++b;
    }
    return *a == *b;
}

/* ---------- Work Area (Full Screen) ---------- */

static BOOL CALLBACK CheckMonitorProc(HMONITOR hMon, HDC hdc, LPRECT lprc, LPARAM lParam) {
    (void)hdc;
    (void)lprc;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        if (mi.rcWork.left != mi.rcMonitor.left ||
            mi.rcWork.top != mi.rcMonitor.top ||
            mi.rcWork.right != mi.rcMonitor.right ||
            mi.rcWork.bottom != mi.rcMonitor.bottom) {
            *(BOOL*)lParam = TRUE;
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL AnyMonitorNeedsFix(void) {
    BOOL needs = FALSE;
    EnumDisplayMonitors(NULL, NULL, CheckMonitorProc, (LPARAM)&needs);
    return needs;
}

static BOOL CALLBACK ApplyMonitorProc(HMONITOR hMon, HDC hdc, LPRECT lprc, LPARAM lParam) {
    (void)hdc;
    (void)lprc;
    (void)lParam;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        if (mi.rcWork.left != mi.rcMonitor.left ||
            mi.rcWork.top != mi.rcMonitor.top ||
            mi.rcWork.right != mi.rcMonitor.right ||
            mi.rcWork.bottom != mi.rcMonitor.bottom) {
            RECT rc = mi.rcMonitor;
            SystemParametersInfoW(SPI_SETWORKAREA, 0, &rc, 0);
        }
    }
    return TRUE;
}

static void ApplyFullWorkArea(void) {
    if (g_applyingWork) return;
    g_applyingWork = TRUE;
    EnumDisplayMonitors(NULL, NULL, ApplyMonitorProc, 0);
    g_applyingWork = FALSE;
    TrimWorkingSet();
}

static void RestoreDefaultWorkArea(void) {
    RECT rc;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0)) {
        if (rc.top == 0) {
            rc.top = 1;
        }
        SystemParametersInfoW(SPI_SETWORKAREA, 0, &rc, SPIF_SENDCHANGE);
    }
}

/* ---------- XAML Bridge Shift & Safe Region Clipping ---------- */

typedef struct {
    HWND parent;
    int offsetX;
    int width;
    int height;
    HWND firstBridge;
} BridgeCtx;

static BOOL CALLBACK MoveBridgeProc(HWND child, LPARAM lParam) {
    BridgeCtx* ctx = (BridgeCtx*)lParam;
    if (GetParent(child) != ctx->parent) return TRUE;

    WCHAR cls[96];
    if (GetClassNameW(child, cls, 96) > 0 && StrEqW(cls, kBridgeClass)) {
        if (!ctx->firstBridge) ctx->firstBridge = child;
        SetWindowPos(
            child, NULL, ctx->offsetX, 0, ctx->width, ctx->height,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    }
    return TRUE;
}

static BOOL CALLBACK FindBridgeProc(HWND child, LPARAM lParam) {
    BridgeCtx* ctx = (BridgeCtx*)lParam;
    if (GetParent(child) != ctx->parent) return TRUE;

    WCHAR cls[96];
    if (GetClassNameW(child, cls, 96) > 0 && StrEqW(cls, kBridgeClass)) {
        ctx->firstBridge = child;
        return FALSE;
    }
    return TRUE;
}

static void UnhookTray(void) {
    if (g_hTrayHook) {
        UnhookWinEvent(g_hTrayHook);
        g_hTrayHook = NULL;
    }
}

static void ApplyRegionForCurrentShift(int monLeft, int monTop, int monW, int trayTop, int trayH, HWND hBridge) {
    int actualShift = 0;
    if (hBridge) {
        RECT rcB;
        if (GetWindowRect(hBridge, &rcB)) {
            int s = monLeft - rcB.left;
            if (s > 10 && s < (monW / 3)) {
                actualShift = s;
            }
        }
    }

    if (actualShift == g_lastShift && monW == g_lastMonW && trayH == g_lastTrayH) {
        return;
    }
    g_lastShift = actualShift;
    g_lastMonW = monW;
    g_lastTrayH = trayH;

    /*
     * Left side (0 .. 27%): clipped out & 100% click-through so app menus
     *   and browser tabs remain directly clickable.
     * Right side: clipped by actualShift (~11% if the XAML bridge shifted left,
     *   freeing window controls completely; or 0 if Windows locked the bridge
     *   at x=0, guaranteeing no icons or clock are ever cut off).
     */
    int rgnLeft = (monW * 27) / 100;
    int rgnRight = monW - actualShift;
    if (rgnRight <= rgnLeft + 300) {
        rgnLeft = 0;
        rgnRight = monW;
    }

    BOOL isTop = (trayTop <= monTop + 4);
    int y1 = isTop ? -18 : 0;
    int y2 = isTop ? (trayH + 1) : (trayH + 19);

    HRGN hRgn = CreateRoundRectRgn(rgnLeft, y1, rgnRight + 1, y2, 20, 20);
    if (hRgn) {
        if (!SetWindowRgn(g_hTray, hRgn, TRUE)) {
            DeleteObject(hRgn);
        }
    }
}

static void ScheduleTrayApply(UINT delayMs) {
    if (!g_hwnd) return;
    ULONGLONG now = GetTickCount64();
    if (now - g_trayBurstStart > 2500) {
        g_trayBurstStart = now;
        g_trayBurstCount = 0;
    }
    g_trayBurstCount++;
    if (g_trayBurstCount > 6) {
        /* If shell fights shifting, verify region once without shifting so nothing gets clipped */
        SetTimer(g_hwnd, TIMER_VERIFY, 200, NULL);
        return;
    }
    SetTimer(g_hwnd, TIMER_TRAY, delayMs, NULL);
}

static void CALLBACK TrayWinEventProc(
    HWINEVENTHOOK hHook, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    (void)hHook; (void)event; (void)dwEventThread; (void)dwmsEventTime;
    if (!g_centerMode || g_hidden || g_applyingTray) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    if (hwnd == g_hTray || GetParent(hwnd) == g_hTray) {
        ScheduleTrayApply(120);
    }
}

static void RefreshTrayHandle(void) {
    HWND h = FindWindowW(L"Shell_TrayWnd", NULL);
    if (h != g_hTray) {
        UnhookTray();
        g_hTray = h;
        g_lastShift = -1;
    }
    if (g_hTray && g_centerMode && !g_hTrayHook) {
        DWORD pid = 0;
        DWORD tid = GetWindowThreadProcessId(g_hTray, &pid);
        if (pid && tid) {
            g_hTrayHook = SetWinEventHook(
                EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                NULL, TrayWinEventProc, pid, tid, WINEVENT_OUTOFCONTEXT);
        }
    }
}

static void ApplyCenteredPill(void) {
    RefreshTrayHandle();
    if (!g_hTray || g_hidden) return;

    HMONITOR hMon = MonitorFromWindow(g_hTray, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return;

    RECT rcTray;
    if (!GetWindowRect(g_hTray, &rcTray)) return;

    int monW = mi.rcMonitor.right - mi.rcMonitor.left;
    int trayH = rcTray.bottom - rcTray.top;
    if (trayH <= 0) trayH = 42;

    /* Shift XAML bridge left by 11% (~211px on 1920px) to clear - [] X on top-right */
    int shiftPx = (monW * 11) / 100;

    g_applyingTray = TRUE;
    BridgeCtx ctx;
    ctx.parent = g_hTray;
    ctx.offsetX = -shiftPx;
    ctx.width = monW;
    ctx.height = trayH;
    ctx.firstBridge = NULL;
    EnumChildWindows(g_hTray, MoveBridgeProc, (LPARAM)&ctx);

    ApplyRegionForCurrentShift(mi.rcMonitor.left, mi.rcMonitor.top, monW, rcTray.top, trayH, ctx.firstBridge);
    g_applyingTray = FALSE;

    /* Re-verify after 250ms in case Explorer snapped bridge back to x=0 */
    if (g_hwnd) {
        SetTimer(g_hwnd, TIMER_VERIFY, 250, NULL);
    }
    TrimWorkingSet();
}

static void VerifyRegionOnly(void) {
    if (!g_hTray || !g_centerMode || g_hidden) return;

    HMONITOR hMon = MonitorFromWindow(g_hTray, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return;

    RECT rcTray;
    if (!GetWindowRect(g_hTray, &rcTray)) return;

    int monW = mi.rcMonitor.right - mi.rcMonitor.left;
    int trayH = rcTray.bottom - rcTray.top;
    if (trayH <= 0) trayH = 42;

    BridgeCtx ctx;
    ctx.parent = g_hTray;
    ctx.offsetX = 0;
    ctx.width = monW;
    ctx.height = trayH;
    ctx.firstBridge = NULL;
    EnumChildWindows(g_hTray, FindBridgeProc, (LPARAM)&ctx);

    ApplyRegionForCurrentShift(mi.rcMonitor.left, mi.rcMonitor.top, monW, rcTray.top, trayH, ctx.firstBridge);
    TrimWorkingSet();
}

static void RestoreTaskbarLayout(void) {
    UnhookTray();
    HWND hTray = g_hTray ? g_hTray : FindWindowW(L"Shell_TrayWnd", NULL);
    if (!hTray) return;

    if (g_hidden) {
        ShowWindowAsync(hTray, SW_SHOW);
        g_hidden = FALSE;
    }

    HMONITOR hMon = MonitorFromWindow(hTray, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        RECT rcTray;
        int trayH = g_lastTrayH > 0 ? g_lastTrayH : 42;
        if (GetWindowRect(hTray, &rcTray) && (rcTray.bottom - rcTray.top) > 0) {
            trayH = rcTray.bottom - rcTray.top;
        }
        int monW = mi.rcMonitor.right - mi.rcMonitor.left;

        BridgeCtx ctx;
        ctx.parent = hTray;
        ctx.offsetX = 0;
        ctx.width = monW;
        ctx.height = trayH;
        ctx.firstBridge = NULL;
        EnumChildWindows(hTray, MoveBridgeProc, (LPARAM)&ctx);

        SetWindowRgn(hTray, NULL, TRUE);
    }
    g_lastShift = -1;
}

static void ToggleTaskbarVisibility(void) {
    RefreshTrayHandle();
    if (!g_hTray) return;

    g_hidden = !g_hidden;
    ShowWindowAsync(g_hTray, g_hidden ? SW_HIDE : SW_SHOW);
    if (!g_hidden && g_centerMode) {
        g_trayBurstCount = 0;
        ScheduleTrayApply(80);
    }
    TrimWorkingSet();
}

static void ToggleCenterMode(void) {
    g_centerMode = !g_centerMode;
    g_trayBurstCount = 0;
    if (g_centerMode) {
        ApplyCenteredPill();
    } else {
        RestoreTaskbarLayout();
    }
    ApplyFullWorkArea();
}

/* ---------- Window Procedure ---------- */

static void ScheduleWorkAreaApply(HWND hwnd, UINT delayMs) {
    ULONGLONG now = GetTickCount64();
    if (now - g_workBurstStart > 3000) {
        g_workBurstStart = now;
        g_workBurstCount = 0;
    }
    g_workBurstCount++;
    if (g_workBurstCount > 5) {
        delayMs = 2000;
    }
    SetTimer(hwnd, TIMER_WORKAREA, delayMs, NULL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY) {
        if (wParam == HK_TOGGLE_HIDE) {
            ToggleTaskbarVisibility();
        } else if (wParam == HK_TOGGLE_CENTER) {
            ToggleCenterMode();
        }
        return 0;
    }

    if (msg == WM_TIMER) {
        if (wParam == TIMER_WORKAREA) {
            KillTimer(hwnd, TIMER_WORKAREA);
            if (AnyMonitorNeedsFix()) {
                ApplyFullWorkArea();
            } else {
                TrimWorkingSet();
            }
            return 0;
        }
        if (wParam == TIMER_TRAY) {
            KillTimer(hwnd, TIMER_TRAY);
            if (g_centerMode && !g_hidden) {
                ApplyCenteredPill();
            }
            return 0;
        }
        if (wParam == TIMER_VERIFY) {
            KillTimer(hwnd, TIMER_VERIFY);
            VerifyRegionOnly();
            return 0;
        }
    }

    if (msg == WM_SETTINGCHANGE) {
        if (!g_applyingWork && wParam == SPI_SETWORKAREA && AnyMonitorNeedsFix()) {
            ScheduleWorkAreaApply(hwnd, 150);
        }
        return 0;
    }

    if (msg == WM_DISPLAYCHANGE) {
        g_trayBurstCount = 0;
        ScheduleWorkAreaApply(hwnd, 250);
        if (g_centerMode) ScheduleTrayApply(350);
        return 0;
    }

    if (msg == WM_POWERBROADCAST) {
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            g_trayBurstCount = 0;
            ScheduleWorkAreaApply(hwnd, 500);
            if (g_centerMode) ScheduleTrayApply(650);
        }
        return 0;
    }

    if (msg == WM_REFRESH_NOW) {
        g_trayBurstCount = 0;
        ApplyFullWorkArea();
        if (g_centerMode && !g_hidden) ApplyCenteredPill();
        return 0;
    }

    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        g_hTray = NULL;
        g_trayBurstCount = 0;
        ScheduleWorkAreaApply(hwnd, 500);
        if (g_centerMode) ScheduleTrayApply(700);
        return 0;
    }

    if (msg == WM_DESTROY) {
        UnregisterHotKey(hwnd, HK_TOGGLE_HIDE);
        UnregisterHotKey(hwnd, HK_TOGGLE_CENTER);
        RestoreTaskbarLayout();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ---------- CLI Argument Parsing (Zero-CRT) ---------- */

static BOOL HasFlag(LPCWSTR flag) {
    LPCWSTR cmd = GetCommandLineW();
    if (!cmd) return FALSE;
    for (LPCWSTR p = cmd; *p; ++p) {
        LPCWSTR a = p;
        LPCWSTR b = flag;
        while (*b) {
            WCHAR ca = *a;
            WCHAR cb = *b;
            if (ca >= L'A' && ca <= L'Z') ca = (WCHAR)(ca + 32);
            if (cb >= L'A' && cb <= L'Z') cb = (WCHAR)(cb + 32);
            if (ca != cb) break;
            ++a;
            ++b;
        }
        if (*b == L'\0' && (*a == L'\0' || *a == L' ' || *a == L'\t')) {
            return TRUE;
        }
    }
    return FALSE;
}

typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT);

void WinMainCRTStartup(void) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        union {
            FARPROC fp;
            SetProcessDpiAwarenessContext_t fn;
        } dpiProc;
        dpiProc.fp = GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (dpiProc.fn) {
            dpiProc.fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    if (HasFlag(L"--stop")) {
        HWND existing = FindWindowW(kClassName, NULL);
        if (existing) {
            PostMessageW(existing, WM_CLOSE, 0, 0);
            Sleep(120);
        } else {
            RestoreTaskbarLayout();
        }
        RestoreDefaultWorkArea();
        ExitProcess(0);
    }

    if (HasFlag(L"--no-center")) {
        g_centerMode = FALSE;
    }

    HANDLE hMutex = CreateMutexW(NULL, FALSE, kMutexName);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kClassName, NULL);
        if (existing) {
            PostMessageW(existing, WM_REFRESH_NOW, 0, 0);
        }
        CloseHandle(hMutex);
        ExitProcess(0);
    }

    HINSTANCE hInst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInst;
    wc.hIcon = NULL;
    wc.hCursor = NULL;
    wc.hbrBackground = NULL;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = kClassName;
    wc.hIconSm = NULL;

    if (!RegisterClassExW(&wc)) {
        ExitProcess(1);
    }

    g_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName,
        NULL,
        WS_POPUP,
        0, 0, 0, 0,
        NULL, NULL, hInst, NULL);

    if (!g_hwnd) {
        ExitProcess(2);
    }

    /* Hotkey 1: Win + ` (or fallback Alt + `) -> Hide/Show taskbar */
    if (!RegisterHotKey(g_hwnd, HK_TOGGLE_HIDE, MOD_WIN | MOD_NOREPEAT, VK_OEM_3)) {
        RegisterHotKey(g_hwnd, HK_TOGGLE_HIDE, MOD_ALT | MOD_NOREPEAT, VK_OEM_3);
    }
    /* Hotkey 2: Win + Shift + ` (or fallback Alt + Shift + `) -> Toggle Centered Pill */
    if (!RegisterHotKey(g_hwnd, HK_TOGGLE_CENTER, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, VK_OEM_3)) {
        RegisterHotKey(g_hwnd, HK_TOGGLE_CENTER, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_OEM_3);
    }

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    ApplyFullWorkArea();
    if (g_centerMode) {
        ApplyCenteredPill();
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        DispatchMessageW(&msg);
    }

    if (hMutex) {
        CloseHandle(hMutex);
    }
    ExitProcess(0);
}
