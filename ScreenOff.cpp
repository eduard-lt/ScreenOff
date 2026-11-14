// ScreenOff: tray icon that puts every monitor except the one under the cursor
// into DDC/CI standby. Left-click the icon (or Win+Alt+O) to toggle.
// Moving the cursor onto a sleeping screen wakes it.
//
// Build (Windhawk's bundled clang):
//   clang++ -std=c++23 -O2 -static -mwindows -DUNICODE -D_UNICODE ScreenOff.cpp
//           -o ScreenOff.exe -ldxva2 -lshell32 -lgdi32 -luser32 -ladvapi32

#include <windows.h>
#include <shellapi.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <vector>

constexpr BYTE kVcpPowerMode = 0xD6;
constexpr DWORD kPowerOn = 1;
constexpr DWORD kPowerStandby = 4;
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT kTimerId = 1;
constexpr UINT kPollMs = 150;
constexpr int kHotkeyId = 1;
constexpr UINT kHotkeyMods = MOD_WIN | MOD_ALT | MOD_NOREPEAT;
constexpr UINT kHotkeyVk = 'O';

enum : UINT {
    ID_TOGGLE = 100,
    ID_WAKE_ON_CURSOR,
    ID_HOTKEY,
    ID_AUTOSTART,
    ID_EXIT,
};

constexpr wchar_t kAppName[] = L"ScreenOff";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kCfgKey[] = L"Software\\ScreenOff";

struct OffMonitor {
    HMONITOR hmon;
    std::vector<PHYSICAL_MONITOR> phys;
};

HWND g_hwnd;
UINT g_taskbarCreatedMsg;
HICON g_iconOn, g_iconOff;
std::vector<OffMonitor> g_off;
bool g_wakeOnCursor = true;
bool g_hotkeyEnabled = true;
bool g_hotkeyRegistered = false;

// ---------- settings ----------

DWORD ReadCfg(PCWSTR name, DWORD def) {
    DWORD v = def, size = sizeof(v);
    RegGetValue(HKEY_CURRENT_USER, kCfgKey, name, RRF_RT_REG_DWORD, nullptr,
                &v, &size);
    return v;
}

void WriteCfg(PCWSTR name, DWORD v) {
    RegSetKeyValue(HKEY_CURRENT_USER, kCfgKey, name, REG_DWORD, &v, sizeof(v));
}

bool IsAutostart() {
    return RegGetValue(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ,
                       nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    if (on) {
        WCHAR path[MAX_PATH + 2] = L"\"";
        GetModuleFileName(nullptr, path + 1, MAX_PATH);
        lstrcat(path, L"\"");
        RegSetKeyValue(HKEY_CURRENT_USER, kRunKey, kAppName, REG_SZ, path,
                       (lstrlen(path) + 1) * sizeof(WCHAR));
    } else {
        RegDeleteKeyValue(HKEY_CURRENT_USER, kRunKey, kAppName);
    }
}

// ---------- icon ----------

HICON MakeIcon(bool screensOff) {
    int s = GetSystemMetrics(SM_CXSMICON) * 2;  // draw large, Windows scales
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    DWORD* px = nullptr;
    HBITMAP color =
        CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, color);

    // Draw in a key colour on black, then convert to alpha.
    RECT all{0, 0, s, s};
    FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    COLORREF ink = RGB(255, 255, 255);
    int t = s / 12 + 1;  // stroke width
    HPEN pen = CreatePen(PS_SOLID, t, ink);
    HBRUSH fill = CreateSolidBrush(ink);
    SelectObject(dc, pen);
    SelectObject(dc, screensOff ? fill : GetStockObject(NULL_BRUSH));

    int l = s / 12, r = s - s / 12, top = s / 6, bottom = s * 2 / 3;
    RoundRect(dc, l + t / 2, top, r - t / 2, bottom, s / 6, s / 6);
    MoveToEx(dc, s / 2, bottom, nullptr);
    LineTo(dc, s / 2, s * 5 / 6);
    MoveToEx(dc, s / 3, s * 5 / 6, nullptr);
    LineTo(dc, s * 2 / 3, s * 5 / 6);
    if (screensOff) {
        // a small "z" cut out of the filled screen
        HPEN cut = CreatePen(PS_SOLID, t, RGB(0, 0, 0));
        SelectObject(dc, cut);
        int zx = s * 3 / 8, zy = top + s / 9, zw = s / 4, zh = s / 5;
        MoveToEx(dc, zx, zy, nullptr);
        LineTo(dc, zx + zw, zy);
        LineTo(dc, zx, zy + zh);
        LineTo(dc, zx + zw, zy + zh);
        SelectObject(dc, pen);
        DeleteObject(cut);
    }
    GdiFlush();

    for (int i = 0; i < s * s; i++) {
        BYTE a = (BYTE)(px[i] & 0xFF);  // white intensity -> alpha
        px[i] = (a << 24) | (a << 16) | (a << 8) | a;  // premultiplied white
    }

    SelectObject(dc, old);
    HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(mask);
    DeleteObject(color);
    DeleteObject(pen);
    DeleteObject(fill);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

// ---------- tray ----------

void UpdateTray(DWORD msg) {
    NOTIFYICONDATA nid{sizeof(nid)};
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_off.empty() ? g_iconOn : g_iconOff;
    lstrcpy(nid.szTip, g_off.empty() ? L"ScreenOff: click to turn off other screens"
                                     : L"ScreenOff: click to wake screens");
    Shell_NotifyIcon(msg, &nid);
    if (msg == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIcon(NIM_SETVERSION, &nid);
    }
}

// ---------- monitors ----------

void Wake(OffMonitor& m) {
    for (auto& p : m.phys) {
        SetVCPFeature(p.hPhysicalMonitor, kVcpPowerMode, kPowerOn);
    }
    DestroyPhysicalMonitors((DWORD)m.phys.size(), m.phys.data());
}

void WakeAll() {
    for (auto& m : g_off) {
        Wake(m);
    }
    g_off.clear();
    UpdateTray(NIM_MODIFY);
}

BOOL CALLBACK EnumMonitorsProc(HMONITOR h, HDC, LPRECT, LPARAM lp) {
    ((std::vector<HMONITOR>*)lp)->push_back(h);
    return TRUE;
}

void TurnOffOthers() {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR current = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);

    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc, (LPARAM)&monitors);

    for (HMONITOR h : monitors) {
        if (h == current) {
            continue;
        }
        DWORD count = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(h, &count) || !count) {
            continue;
        }
        OffMonitor om{h, std::vector<PHYSICAL_MONITOR>(count)};
        if (!GetPhysicalMonitorsFromHMONITOR(h, count, om.phys.data())) {
            continue;
        }
        bool anyOff = false;
        for (auto& p : om.phys) {
            if (SetVCPFeature(p.hPhysicalMonitor, kVcpPowerMode,
                              kPowerStandby)) {
                anyOff = true;
            }
        }
        if (anyOff) {
            g_off.push_back(std::move(om));
        } else {
            DestroyPhysicalMonitors(count, om.phys.data());
        }
    }

    if (g_off.empty()) {
        NOTIFYICONDATA nid{sizeof(nid)};
        nid.hWnd = g_hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_WARNING;
        lstrcpy(nid.szInfoTitle, L"ScreenOff");
        lstrcpy(nid.szInfo,
                L"No other screen could be turned off. Click from the screen "
                L"you want to keep on; the other monitor must support DDC/CI.");
        Shell_NotifyIcon(NIM_MODIFY, &nid);
    }
    UpdateTray(NIM_MODIFY);
}

void Toggle() {
    if (g_off.empty()) {
        TurnOffOthers();
    } else {
        WakeAll();
    }
}

void CheckCursor() {
    if (g_off.empty() || !g_wakeOnCursor) {
        return;
    }
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR current = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
    for (auto it = g_off.begin(); it != g_off.end(); ++it) {
        if (it->hmon == current) {
            Wake(*it);
            g_off.erase(it);
            UpdateTray(NIM_MODIFY);
            break;
        }
    }
}

void ApplyHotkey() {
    if (g_hotkeyRegistered) {
        UnregisterHotKey(g_hwnd, kHotkeyId);
        g_hotkeyRegistered = false;
    }
    if (g_hotkeyEnabled) {
        g_hotkeyRegistered =
            RegisterHotKey(g_hwnd, kHotkeyId, kHotkeyMods, kHotkeyVk);
    }
}

void ShowMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenu(m, MF_STRING, ID_TOGGLE,
               g_off.empty() ? L"Turn off other screens" : L"Wake all screens");
    AppendMenu(m, MF_SEPARATOR, 0, nullptr);
    AppendMenu(m, MF_STRING | (g_wakeOnCursor ? MF_CHECKED : 0),
               ID_WAKE_ON_CURSOR, L"Wake a screen when the cursor moves onto it");
    AppendMenu(m,
               MF_STRING | (g_hotkeyEnabled ? MF_CHECKED : 0) |
                   (g_hotkeyEnabled && !g_hotkeyRegistered ? MF_GRAYED : 0),
               ID_HOTKEY,
               g_hotkeyEnabled && !g_hotkeyRegistered
                   ? L"Hotkey Win+Alt+O (in use by another app)"
                   : L"Hotkey Win+Alt+O");
    AppendMenu(m, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), ID_AUTOSTART,
               L"Start with Windows");
    AppendMenu(m, MF_SEPARATOR, 0, nullptr);
    AppendMenu(m, MF_STRING, ID_EXIT, L"Exit");
    SetMenuDefaultItem(m, ID_TOGGLE, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessage(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && msg) {
        UpdateTray(NIM_ADD);  // Explorer restarted
        return 0;
    }
    switch (msg) {
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    Toggle();
                    break;
                case WM_CONTEXTMENU:
                    ShowMenu();
                    break;
            }
            return 0;
        case WM_HOTKEY:
            if (wp == kHotkeyId) {
                Toggle();
            }
            return 0;
        case WM_TIMER:
            CheckCursor();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_TOGGLE:
                    Toggle();
                    break;
                case ID_WAKE_ON_CURSOR:
                    g_wakeOnCursor = !g_wakeOnCursor;
                    WriteCfg(L"WakeOnCursor", g_wakeOnCursor);
                    break;
                case ID_HOTKEY:
                    g_hotkeyEnabled = !g_hotkeyEnabled;
                    WriteCfg(L"Hotkey", g_hotkeyEnabled);
                    ApplyHotkey();
                    break;
                case ID_AUTOSTART:
                    SetAutostart(!IsAutostart());
                    break;
                case ID_EXIT:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;
        case WM_ENDSESSION:
            if (wp) {
                WakeAll();
            }
            return 0;
        case WM_DESTROY:
            WakeAll();
            UpdateTray(NIM_DELETE);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutex(nullptr, TRUE, L"ScreenOff_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_wakeOnCursor = ReadCfg(L"WakeOnCursor", 1);
    g_hotkeyEnabled = ReadCfg(L"Hotkey", 1);

    WNDCLASS wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kAppName;
    RegisterClass(&wc);
    g_hwnd = CreateWindow(kAppName, kAppName, 0, 0, 0, 0, 0, nullptr, nullptr,
                          inst, nullptr);

    g_taskbarCreatedMsg = RegisterWindowMessage(L"TaskbarCreated");
    g_iconOn = MakeIcon(false);
    g_iconOff = MakeIcon(true);
    UpdateTray(NIM_ADD);
    ApplyHotkey();
    SetTimer(g_hwnd, kTimerId, kPollMs, nullptr);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CloseHandle(mutex);
    return 0;
}
