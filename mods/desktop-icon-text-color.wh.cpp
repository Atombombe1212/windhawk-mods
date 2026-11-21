// ==WindhawkMod==
// @id              desktop-icon-text-color
// @name            black desktop icon text
// @description     Forces dark desktop icon labels (#141418 by default) with customizable color, adjustable weight, and shadow removal, ideal for white or bright wallpapers.
// @version         1.6
// @author          Atombombe1212
// @license         MIT
// @include         explorer.exe
// @compilerOptions -lgdi32 -luser32 -luxtheme -lcomctl32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Black Desktop Icon Text
Switches desktop icon label text to dark `#141418` by default (ideal for white or bright wallpapers), lets you customize the label color in the mod settings, allows tuning font weight, and keeps the styling persistent across window maximize/restore cycles.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- useHexColor: false
  $name: Use custom HEX color instead of RGB
  $description: "Enable this to pick a color using a HEX code (such as #141418) instead of the RGB values below."
- hexColor: "#141418"
  $name: Custom HEX color
  $description: "Used when Use custom HEX color instead of RGB is enabled (format: #RRGGBB or RRGGBB)."
- color:
  - red: 20
  - green: 20
  - blue: 24
  $name: Label color (RGB)
  $description: "Default is #141418 (20, 20, 24) for strong contrast on bright or white wallpapers."
- fontWeight: 550
  $name: Font weight (100 - 900)
  $description: "400 = regular, 550 = slightly bolder, 600 = semi-bold."
- removeShadow: true
  $name: Remove text shadow
  $description: "Disables icon text shadow for cleaner legibility on light backgrounds."
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>

struct ModConfig {
    COLORREF labelColor;
    int labelWeight;
    bool stripShadow;
};

static ModConfig g_cfg;
static HFONT g_iconFont = nullptr;
static HWND g_desktopList = nullptr;
static WNDPROC g_prevListProc = nullptr;

using SendMessageW_fn = decltype(&SendMessageW);
static SendMessageW_fn g_origSendMessageW = nullptr;

using ShowWindow_fn = decltype(&ShowWindow);
static ShowWindow_fn g_origShowWindow = nullptr;

using DrawThemeTextEx_fn = decltype(&DrawThemeTextEx);
static DrawThemeTextEx_fn g_origDrawThemeTextEx = nullptr;

using SetTextColor_fn = decltype(&SetTextColor);
static SetTextColor_fn g_origSetTextColor = nullptr;

static int HexDigitValue(WCHAR ch) {
    if (ch >= L'0' && ch <= L'9') {
        return ch - L'0';
    }
    if (ch >= L'a' && ch <= L'f') {
        return 10 + (ch - L'a');
    }
    if (ch >= L'A' && ch <= L'F') {
        return 10 + (ch - L'A');
    }
    return -1;
}

static bool TryParseHexColor(PCWSTR str, COLORREF* outColor) {
    if (!str || !outColor) {
        return false;
    }

    while (*str == L' ' || *str == L'\t') {
        ++str;
    }
    if (*str == L'#') {
        ++str;
    }

    int vals[6];
    for (int i = 0; i < 6; ++i) {
        int v = HexDigitValue(str[i]);
        if (v < 0) {
            return false;
        }
        vals[i] = v;
    }

    BYTE r = (BYTE)((vals[0] << 4) | vals[1]);
    BYTE g = (BYTE)((vals[2] << 4) | vals[3]);
    BYTE b = (BYTE)((vals[4] << 4) | vals[5]);
    *outColor = RGB(r, g, b);
    return true;
}

static int ClampByte(int v) {
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return v;
}

static bool IsDesktopListView(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    WCHAR cls[64];
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) == 0 || wcscmp(cls, L"SysListView32") != 0) {
        return false;
    }

    HWND parent = GetParent(hwnd);
    if (!parent || GetClassNameW(parent, cls, ARRAYSIZE(cls)) == 0) {
        return false;
    }

    return wcscmp(cls, L"SHELLDLL_DefView") == 0;
}

static HWND FindDesktopListView() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND defView = progman ? FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;

    if (!defView) {
        HWND worker = nullptr;
        while ((worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr) {
            defView = FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr);
            if (defView) {
                break;
            }
        }
    }

    return defView ? FindWindowExW(defView, nullptr, L"SysListView32", nullptr) : nullptr;
}

static bool IsDesktopDC(HDC hdc) {
    return IsDesktopListView(WindowFromDC(hdc));
}

static void RebuildLabelFont() {
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        return;
    }

    ncm.lfMessageFont.lfWeight = g_cfg.labelWeight;
    ncm.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;

    HFONT built = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!built) {
        return;
    }

    if (g_iconFont) {
        DeleteObject(g_iconFont);
    }
    g_iconFont = built;
}

static void PushLabelColors(HWND list) {
    if (!list || !IsWindow(list)) {
        return;
    }

    if (g_origSendMessageW) {
        g_origSendMessageW(list, LVM_SETTEXTCOLOR, 0, (LPARAM)g_cfg.labelColor);
        g_origSendMessageW(list, LVM_SETTEXTBKCOLOR, 0, (LPARAM)CLR_NONE);
    } else {
        SendMessageW(list, LVM_SETTEXTCOLOR, 0, (LPARAM)g_cfg.labelColor);
        SendMessageW(list, LVM_SETTEXTBKCOLOR, 0, (LPARAM)CLR_NONE);
    }
}

static LRESULT CALLBACK DesktopListProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == LVM_SETTEXTCOLOR) {
        lParam = (LPARAM)g_cfg.labelColor;
    } else if (msg == WM_SETFONT && g_iconFont) {
        wParam = (WPARAM)g_iconFont;
    }

    LRESULT result = CallWindowProcW(g_prevListProc, hWnd, msg, wParam, lParam);

    if (msg == WM_THEMECHANGED || msg == WM_SYSCOLORCHANGE || msg == WM_SETTINGCHANGE || msg == WM_WINDOWPOSCHANGED) {
        PushLabelColors(hWnd);
        if (g_iconFont && g_origSendMessageW) {
            g_origSendMessageW(hWnd, WM_SETFONT, (WPARAM)g_iconFont, FALSE);
        }
    }

    return result;
}

static void HookDesktopListProc() {
    HWND lv = FindDesktopListView();
    if (!lv) {
        return;
    }

    if (lv != g_desktopList) {
        if (g_desktopList && g_prevListProc && IsWindow(g_desktopList)) {
            SetWindowLongPtrW(g_desktopList, GWLP_WNDPROC, (LONG_PTR)g_prevListProc);
        }
        g_desktopList = lv;
        g_prevListProc = (WNDPROC)SetWindowLongPtrW(lv, GWLP_WNDPROC, (LONG_PTR)DesktopListProc);
    }
}

static void UnhookDesktopListProc() {
    if (g_desktopList && g_prevListProc && IsWindow(g_desktopList)) {
        SetWindowLongPtrW(g_desktopList, GWLP_WNDPROC, (LONG_PTR)g_prevListProc);
    }
    g_desktopList = nullptr;
    g_prevListProc = nullptr;
}

static void ApplyDesktopStyling() {
    HookDesktopListProc();
    HWND lv = FindDesktopListView();
    if (!lv) {
        return;
    }

    RebuildLabelFont();
    PushLabelColors(lv);
    if (g_iconFont && g_origSendMessageW) {
        g_origSendMessageW(lv, WM_SETFONT, (WPARAM)g_iconFont, TRUE);
    }
    InvalidateRect(lv, nullptr, TRUE);
    UpdateWindow(lv);
}

static LRESULT WINAPI SendMessageW_Detour(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    if (IsDesktopListView(hWnd)) {
        HookDesktopListProc();
        if (Msg == LVM_SETTEXTCOLOR) {
            lParam = (LPARAM)g_cfg.labelColor;
        } else if (Msg == WM_SETFONT && g_iconFont) {
            wParam = (WPARAM)g_iconFont;
        }
    }
    return g_origSendMessageW(hWnd, Msg, wParam, lParam);
}

static BOOL WINAPI ShowWindow_Detour(HWND hWnd, int nCmdShow) {
    BOOL ok = g_origShowWindow(hWnd, nCmdShow);
    HWND lv = FindDesktopListView();
    if (lv) {
        HookDesktopListProc();
        PushLabelColors(lv);
    }
    return ok;
}

static HRESULT WINAPI DrawThemeTextEx_Detour(HTHEME hTheme,
                                             HDC hdc,
                                             int iPartId,
                                             int iStateId,
                                             LPCWSTR pszText,
                                             int cchText,
                                             DWORD dwTextFlags,
                                             LPRECT pRect,
                                             const DTTOPTS* pOptions) {
    if (IsDesktopDC(hdc) && pOptions) {
        DTTOPTS opts = *pOptions;
        opts.dwFlags |= DTT_TEXTCOLOR;
        opts.crText = g_cfg.labelColor;

        if (g_cfg.stripShadow) {
            opts.dwFlags &= ~DTT_SHADOWTYPE;
            opts.iTextShadowType = 0;
        }

        HFONT activeFont = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
        LOGFONTW lf = {};
        HFONT weightedFont = nullptr;
        HFONT previousFont = nullptr;

        if (activeFont && GetObjectW(activeFont, sizeof(lf), &lf) > 0 && lf.lfWeight != g_cfg.labelWeight) {
            lf.lfWeight = g_cfg.labelWeight;
            lf.lfQuality = CLEARTYPE_QUALITY;
            weightedFont = CreateFontIndirectW(&lf);
            if (weightedFont) {
                previousFont = (HFONT)SelectObject(hdc, weightedFont);
            }
        }

        HRESULT hr = g_origDrawThemeTextEx(hTheme, hdc, iPartId, iStateId,
                                           pszText, cchText, dwTextFlags, pRect, &opts);

        if (previousFont) {
            SelectObject(hdc, previousFont);
        }
        if (weightedFont) {
            DeleteObject(weightedFont);
        }

        return hr;
    }

    return g_origDrawThemeTextEx(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);
}

static COLORREF WINAPI SetTextColor_Detour(HDC hdc, COLORREF color) {
    if (IsDesktopDC(hdc)) {
        return g_origSetTextColor(hdc, g_cfg.labelColor);
    }
    return g_origSetTextColor(hdc, color);
}

static void ReadConfig() {
    int r = ClampByte(Wh_GetIntSetting(L"color.red"));
    int g = ClampByte(Wh_GetIntSetting(L"color.green"));
    int b = ClampByte(Wh_GetIntSetting(L"color.blue"));
    COLORREF resolvedColor = RGB(r, g, b);

    bool useHex = Wh_GetIntSetting(L"useHexColor") != 0;
    if (useHex) {
        PCWSTR hexStr = Wh_GetStringSetting(L"hexColor");
        COLORREF parsed = 0;
        if (TryParseHexColor(hexStr, &parsed)) {
            resolvedColor = parsed;
        }
        if (hexStr) {
            Wh_FreeStringSetting(hexStr);
        }
    }

    g_cfg.labelColor = resolvedColor;

    int w = Wh_GetIntSetting(L"fontWeight");
    g_cfg.labelWeight = (w < 100 || w > 900) ? 550 : w;
    g_cfg.stripShadow = Wh_GetIntSetting(L"removeShadow") != 0;
}

BOOL Wh_ModInit() {
    ReadConfig();

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        void* pSendMsg = (void*)GetProcAddress(user32, "SendMessageW");
        if (pSendMsg) {
            Wh_SetFunctionHook(pSendMsg, (void*)SendMessageW_Detour, (void**)&g_origSendMessageW);
        }
        void* pShowWin = (void*)GetProcAddress(user32, "ShowWindow");
        if (pShowWin) {
            Wh_SetFunctionHook(pShowWin, (void*)ShowWindow_Detour, (void**)&g_origShowWindow);
        }
    }

    HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    if (uxtheme) {
        void* pDrawThemeTextEx = (void*)GetProcAddress(uxtheme, "DrawThemeTextEx");
        if (pDrawThemeTextEx) {
            Wh_SetFunctionHook(pDrawThemeTextEx, (void*)DrawThemeTextEx_Detour, (void**)&g_origDrawThemeTextEx);
        }
    }

    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (gdi32) {
        void* pSetTextColor = (void*)GetProcAddress(gdi32, "SetTextColor");
        if (pSetTextColor) {
            Wh_SetFunctionHook(pSetTextColor, (void*)SetTextColor_Detour, (void**)&g_origSetTextColor);
        }
    }

    ApplyDesktopStyling();
    return TRUE;
}

void Wh_ModUninit() {
    UnhookDesktopListProc();

    HWND lv = FindDesktopListView();
    if (lv) {
        if (g_origSendMessageW) {
            g_origSendMessageW(lv, LVM_SETTEXTCOLOR, 0, (LPARAM)RGB(255, 255, 255));
        }
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            HFONT stockFont = CreateFontIndirectW(&ncm.lfMessageFont);
            if (stockFont && g_origSendMessageW) {
                g_origSendMessageW(lv, WM_SETFONT, (WPARAM)stockFont, TRUE);
            }
        }
        InvalidateRect(lv, nullptr, TRUE);
    }

    if (g_iconFont) {
        DeleteObject(g_iconFont);
        g_iconFont = nullptr;
    }
}

void Wh_ModSettingsChanged() {
    ReadConfig();
    ApplyDesktopStyling();
}
