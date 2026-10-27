#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#define WM_DISPLAYCHANGE_ 0x007E

/* ======================================================================
 *  hudcinfo —— 桌面半透明系统状态悬浮面板
 * ======================================================================
 *  技术路线与原版一致：纯 C + Win32 GDI + PDH / IPHelper / PSAPI，
 *  单文件、单 exe、无第三方依赖，排版和原版一模一样。
 *
 *  半透明改成逐像素 alpha（UpdateLayeredWindow）：背景和文字各自独立，
 *  所以调背景不透明度不会再把字一起调淡。
 *      · 文字不透明度：固定 TEXT_ALPHA（= 原版 WIN_ALPHA 200，不再变化）
 *      · 背景不透明度：Ctrl+Alt + ↑ / ↓ 调节
 *
 *  交互（不按 Ctrl+Alt 时鼠标键盘 100% 穿透，和原版没有任何区别；
 *        所有操作都要求「Ctrl + Alt 同时按住，且鼠标停在面板上」）：
 *      Ctrl+Alt + 左键拖动    移动面板
 *      Ctrl+Alt + 滚轮        5 档预设尺寸缩放（等比，字号同步变）
 *      Ctrl+Alt + ↑ / ↓       背景不透明度（文字透明度不受影响）
 *      Ctrl+Alt + ← / →       背景色 8 档（黑 白 红 黄 绿 蓝 紫 透明）
 *      Ctrl+Alt + G/W/Y/R/B/P 字体颜色（绿/白/黄/红/蓝/紫）
 *
 *  位置、档位、颜色、不透明度以及上次的分辨率都记在 hudcinfo.ini：
 *      · 第一次运行（没有配置文件）   -> 屏幕正中
 *      · 分辨率没变                   -> 回到上次的状态
 *      · 分辨率变了                   -> 按新分辨率重新居中
 *  运行中改了分辨率 / 缩放（WM_DISPLAYCHANGE、WM_DPICHANGED，外加 1 秒
 *  一次的兜底比对），也会立刻重新居中并写回配置。
 * ==================================================================== */

/* 文字不透明度：原版只有 WIN_ALPHA 200 一个整体透明度（字和底一起 200），
 * 这里把字钉死在这个值上，背景的不透明度单独用 Ctrl+Alt+↑/↓ 调。 */
#define TEXT_ALPHA          200

#define UPDATE_INTERVAL_MS  1000         /* 刷新间隔 ms（顺带做分辨率兜底检测） */
#define FONT_NAME           L"Consolas"

#define GPU_PATH L"\\GPU Engine(*)\\Utilization Percentage"
#define IO_PATH  L"\\PhysicalDisk(_Total)\\% Disk Time"

#define CFG_FILE_NAME       L"hudcinfo.ini"
#define CFG_SECTION         "[state]"
#define WHEEL_GAP_MS        60           /* 滚轮缩放的最小间隔，防止自由滚轮一下飞好几档 */

/* ---------------------------------------------------------------------
 * 预设：尺寸档位
 * 老版本是 100% 缩放下 422x118、字高 17、行高 26、内边距 10，
 * 这里以“字号”为模，长宽按同一比例推导，缩放时排版不会走样。
 * 运行时会再乘上系统 DPI 缩放（150% -> x1.5）。
 * ------------------------------------------------------------------- */
typedef struct {
    int font;    /* 字高（100% 缩放下的像素） */
    int w, h;    /* 面板宽高 */
    int line;    /* 行高 */
    int pad;     /* 内边距 */
} Preset;

static const Preset g_presets[] = {
    { 13, 323,  90, 20,  8 },
    { 15, 372, 104, 23,  9 },
    { 17, 422, 118, 26, 10 },   /* 默认档：与老版本完全一致 */
    { 20, 496, 139, 31, 12 },
    { 24, 596, 167, 37, 14 },
};
#define PRESET_COUNT    ((int)(sizeof(g_presets) / sizeof(g_presets[0])))
#define PRESET_DEFAULT  2

/* ---------------------------------------------------------------------
 * 预设：背景不透明度 / 背景色 / 字体色
 * ------------------------------------------------------------------- */
static const BYTE g_alphaPresets[] = { 30, 80, 120, 160, 200, 235, 255 };
#define ALPHA_COUNT     ((int)(sizeof(g_alphaPresets) / sizeof(g_alphaPresets[0])))
#define ALPHA_DEFAULT   4                        /* 200，与原版一致 */

typedef struct { COLORREF color; BOOL transparent; } BgPreset;
static const BgPreset g_bgPresets[] = {
    { RGB(0,0,0),      TRUE  },   /* 0 透明：背景 alpha 恒为 0，只剩文字 */
    { RGB(0,0,0),      FALSE },   /* 1 黑（默认） */
    { RGB(240,240,240),FALSE },   /* 2 白 */
    { RGB(190,40,40),  FALSE },   /* 3 红 */
    { RGB(215,185,45), FALSE },   /* 4 黄 */
    { RGB(40,145,70),  FALSE },   /* 5 绿 */
    { RGB(40,80,170),  FALSE },   /* 6 蓝 */
    { RGB(115,60,160), FALSE },   /* 7 紫 */
};
#define BG_COUNT        ((int)(sizeof(g_bgPresets) / sizeof(g_bgPresets[0])))
#define BG_DEFAULT      1                        /* 黑 */

typedef struct { COLORREF color; } FgPreset;
static const FgPreset g_fgPresets[] = {
    { RGB(0,255,140) },    /* 0 G 绿（原版颜色，默认） */
    { RGB(255,255,255) },  /* 1 W 白 */
    { RGB(255,215,0) },    /* 2 Y 黄 */
    { RGB(255,70,70) },    /* 3 R 红 */
    { RGB(90,160,255) },   /* 4 B 蓝 */
    { RGB(200,120,255) },  /* 5 P 紫 */
};
#define FG_COUNT        ((int)(sizeof(g_fgPresets) / sizeof(g_fgPresets[0])))
#define FG_DEFAULT      0

/* ---------------------------------------------------------------------
 * 系统数据
 * ------------------------------------------------------------------- */
typedef struct {
    int cpu, mem, gpu, io;
    int procs, tcpTotal, tcpListen, tcpEstab;
    WCHAR disks[512];
} Stats;

static Stats g_s;
static ULONGLONG g_pi, g_pk, g_pu;
static BOOL g_cpuInit = FALSE;
static double g_cpuPct = 0.0;

static HQUERY g_gpuQ = NULL; static HCOUNTER g_gpuC = NULL; static BOOL g_gpuOk = FALSE; static int g_gpuFail = 0;
static HQUERY g_ioQ  = NULL; static HCOUNTER g_ioC  = NULL; static BOOL g_ioOk  = FALSE; static int g_ioFail  = 0;
static PDH_FMT_COUNTERVALUE_ITEM_W *g_gpuBuf = NULL; static DWORD g_gpuBufSz = 0;

static BYTE *g_tcpBuf = NULL; static DWORD g_tcpBufSize = 0;

/* ---------------------------------------------------------------------
 * 窗口 / 布局 / 外观状态
 * ------------------------------------------------------------------- */
static HDC     g_dc   = NULL;        /* 32bpp DIB 的内存 DC */
static HBITMAP g_dib  = NULL;
static HBITMAP g_oldBmp = NULL;
static DWORD  *g_bits = NULL;        /* DIB 像素：BGRA，预乘 alpha */
static HFONT   g_font = NULL;

static HWND  g_hwnd = NULL;

static int  g_screenW = 0, g_screenH = 0;    /* 当前屏幕物理尺寸 */
static int  g_level = PRESET_DEFAULT;        /* 尺寸档位 */
static int  g_w, g_h, g_x, g_y;              /* 当前窗口矩形 */
static int  g_fs, g_lh, g_pad;               /* 当前字号 / 行高 / 内边距 */
static double g_scale = 1.0;                 /* 系统 DPI 缩放 */

static int  g_alphaIdx = ALPHA_DEFAULT;      /* 背景不透明度档位 */
static int  g_bgIdx    = BG_DEFAULT;         /* 背景色档位 */
static int  g_fgIdx    = FG_DEFAULT;         /* 字体色档位 */

/* Ctrl + Alt 拖动 */
static HHOOK g_mouseHook = NULL;
static HHOOK g_kbdHook   = NULL;
static BOOL  g_dragging = FALSE;
static POINT g_dragStart;                    /* 按下时的鼠标屏幕坐标 */
static POINT g_dragWin;                      /* 按下时的窗口左上角 */
static DWORD g_lastWheel = 0;
static DWORD g_swallowVk = 0;                /* 被吃掉按下的那个键，抬起时一起吃掉 */

/* 配置文件 */
static WCHAR g_cfgPath[MAX_PATH];
static BOOL  g_savePending = FALSE;

/* =====================================================================
 *  1. 配置读写（exe 同目录 hudcinfo.ini）
 * ===================================================================*/
typedef struct {
    int x, y;
    int level;
    int alpha, bg, fg;
    int screenW, screenH;
} Cfg;

static void BuildCfgPath(void) {
    WCHAR *slash;
    DWORD n = GetModuleFileNameW(NULL, g_cfgPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { wcsncpy(g_cfgPath, CFG_FILE_NAME, MAX_PATH - 1); g_cfgPath[MAX_PATH - 1] = 0; return; }
    slash = wcsrchr(g_cfgPath, L'\\');
    if (!slash) { wcsncpy(g_cfgPath, CFG_FILE_NAME, MAX_PATH - 1); g_cfgPath[MAX_PATH - 1] = 0; return; }
    slash[1] = 0;
    if (wcslen(g_cfgPath) + wcslen(CFG_FILE_NAME) + 1 >= MAX_PATH) {
        wcsncpy(g_cfgPath, CFG_FILE_NAME, MAX_PATH - 1); g_cfgPath[MAX_PATH - 1] = 0;
        return;
    }
    wcscat(g_cfgPath, CFG_FILE_NAME);
}

static BOOL ReadFileAll(const WCHAR *path, char *buf, DWORD cap) {
    HANDLE h;
    DWORD got = 0;
    BOOL ok;
    if (cap < 2) return FALSE;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = ReadFile(h, buf, cap - 1, &got, NULL);
    CloseHandle(h);
    if (!ok) return FALSE;
    buf[got] = 0;
    return TRUE;
}

static BOOL MatchKey(const char *line, const char *key, const char **val) {
    size_t n = strlen(key);
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (_strnicmp(p, key, n) != 0) return FALSE;
    p += n;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return FALSE;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    *val = p;
    return TRUE;
}

static BOOL CfgLoad(Cfg *c) {
    char buf[1024];
    char *line, *next;
    int ver = 0;
    memset(c, 0, sizeof(*c));
    c->level = PRESET_DEFAULT;
    c->alpha = ALPHA_DEFAULT;
    c->bg    = BG_DEFAULT;
    c->fg    = FG_DEFAULT;
    if (!ReadFileAll(g_cfgPath, buf, (DWORD)sizeof(buf))) return FALSE;

    for (line = buf; line && *line; line = next) {
        const char *v;
        next = strchr(line, '\n');
        if (next) { *next = 0; next++; }
        if (MatchKey(line, "x", &v))            c->x = (int)strtol(v, NULL, 10);
        else if (MatchKey(line, "y", &v))       c->y = (int)strtol(v, NULL, 10);
        else if (MatchKey(line, "level", &v))   c->level = (int)strtol(v, NULL, 10);
        else if (MatchKey(line, "alpha", &v))   c->alpha = (int)strtol(v, NULL, 10);
        else if (MatchKey(line, "bg", &v))      c->bg = (int)strtol(v, NULL, 10);
        else if (MatchKey(line, "fg", &v))      c->fg = (int)strtol(v, NULL, 10);
        else if (MatchKey(line, "screenw", &v)) c->screenW = (int)strtol(v, NULL, 10);
        else if (MatchKey(line, "screenh", &v)) c->screenH = (int)strtol(v, NULL, 10);
        else if (MatchKey(line, "ver", &v))     ver = (int)strtol(v, NULL, 10);
    }
    /* 旧配置没有 ver 键：那时背景不透明度只有 80/120/160/200/235/255 六档，
     * 现在最前面插入了 30 这一档，索引整体 +1，这里做一次平移 */
    if (ver < 2 && c->alpha >= 0 && c->alpha < 6) c->alpha += 1;
    /* ver 3 起去掉了「灰」这一档，它后面的档位索引整体 -1 */
    if (ver < 3 && c->bg > 2) c->bg -= 1;

    if (c->level < 0 || c->level >= PRESET_COUNT) c->level = PRESET_DEFAULT;
    if (c->alpha < 0 || c->alpha >= ALPHA_COUNT)  c->alpha = ALPHA_DEFAULT;
    if (c->bg    < 0 || c->bg    >= BG_COUNT)     c->bg    = BG_DEFAULT;
    if (c->fg    < 0 || c->fg    >= FG_COUNT)     c->fg    = FG_DEFAULT;
    return TRUE;
}

static void CfgSave(void) {
    char buf[512];
    int n;
    HANDLE h;
    DWORD written = 0;
    if (!g_cfgPath[0]) return;
    n = _snprintf(buf, sizeof(buf) - 1,
        CFG_SECTION "\r\n"
        "ver=3\r\n"
        "x=%d\r\n"
        "y=%d\r\n"
        "level=%d\r\n"
        "alpha=%d\r\n"
        "bg=%d\r\n"
        "fg=%d\r\n"
        "screenw=%d\r\n"
        "screenh=%d\r\n",
        g_x, g_y, g_level, g_alphaIdx, g_bgIdx, g_fgIdx, g_screenW, g_screenH);
    buf[sizeof(buf) - 1] = 0;
    if (n < 0) n = (int)strlen(buf);
    h = CreateFileW(g_cfgPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, buf, (DWORD)n, &written, NULL);
    CloseHandle(h);
    g_savePending = FALSE;
}

/* 交互中频繁变化的状态先标脏，由 1 秒定时器统一落盘，避免狂写文件 */
static void CfgTouch(void) { g_savePending = TRUE; }

/* =====================================================================
 *  2. 布局：DPI、预设档位、居中 / 夹取
 * ===================================================================*/
/* 绘制表面在第 3 节定义，这里先声明（分辨率 / 尺寸变化时要重建） */
static void DestroySurface(void);
static void CreateSurface(void);
static void Refresh(void);
static void PctStr(WCHAR *o, int v);

/* 逐显示器 DPI 感知 V2：
 * 系统 DPI 感知(system aware)的进程，在运行中改分辨率 / 改缩放后，Windows 会
 * 把它的坐标“虚拟化”放大（例如 144DPI 的进程落到 120DPI 的屏幕上时坐标 ×1.2），
 * 而 WH_MOUSE_LL 钩子拿到的鼠标坐标永远是物理像素 —— 两边坐标不一致，
 * 命中检测就会偏，表现为拖动 / 滚轮“时灵时不灵”。
 * 用 PER_MONITOR_AWARE_V2 后全程物理像素，两边就对齐了。 */
static void EnableDpiAwareness(void) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL (WINAPI *SetCtxFn)(void *);
        SetCtxFn pSetCtx = (SetCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (pSetCtx && pSetCtx((void *)(LONG_PTR)-4)) return;   /* PER_MONITOR_AWARE_V2 */
    }
    {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) {
            typedef HRESULT (WINAPI *SetAwareFn)(int);
            SetAwareFn pSetAware = (SetAwareFn)GetProcAddress(shcore, "SetProcessDpiAwareness");
            if (pSetAware && pSetAware(2) == S_OK) return;      /* PROCESS_PER_MONITOR_DPI_AWARE */
        }
    }
    SetProcessDPIAware();                                       /* 老系统兜底 */
}

static double GetUiScale(void) {
    UINT dpi = 0;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32 && g_hwnd) {
        typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
        GetDpiForWindowFn pWnd = (GetDpiForWindowFn)GetProcAddress(u32, "GetDpiForWindow");
        if (pWnd) dpi = pWnd(g_hwnd);
    }
    if (dpi == 0) {
        HDC dc = GetDC(NULL);
        dpi = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ReleaseDC(NULL, dc);
    }
    if (dpi < 96)  dpi = 96;
    if (dpi > 480) dpi = 480;
    return (double)dpi / 96.0;
}

static void ApplyPreset(int level) {
    const Preset *p;
    if (level < 0) level = 0;
    if (level >= PRESET_COUNT) level = PRESET_COUNT - 1;
    p = &g_presets[level];
    g_level = level;
    g_w   = (int)(p->w    * g_scale + 0.5);
    g_h   = (int)(p->h    * g_scale + 0.5);
    g_fs  = (int)(p->font * g_scale + 0.5);
    g_lh  = (int)(p->line * g_scale + 0.5);
    g_pad = (int)(p->pad  * g_scale + 0.5);
    if (g_pad < 2) g_pad = 2;
    if (g_lh < g_fs) g_lh = g_fs;
}

static void CenterHud(void) {
    g_x = (g_screenW - g_w) / 2;
    g_y = (g_screenH - g_h) / 2;
}

static void ClampToScreen(void) {
    if (g_x > g_screenW - g_w) g_x = g_screenW - g_w;
    if (g_y > g_screenH - g_h) g_y = g_screenH - g_h;
    if (g_x < 0) g_x = 0;
    if (g_y < 0) g_y = 0;
}

/* 启动时：读配置 -> 决定档位/外观 -> 决定坐标（居中 / 回到原位） */
static void InitLayout(void) {
    Cfg c;
    BOOL have;

    g_scale   = GetUiScale();
    g_screenW = GetSystemMetrics(SM_CXSCREEN);
    g_screenH = GetSystemMetrics(SM_CYSCREEN);
    if (g_screenW <= 0) g_screenW = 2560;
    if (g_screenH <= 0) g_screenH = 1440;

    have = CfgLoad(&c);
    if (have) {
        g_level    = c.level;
        g_alphaIdx = c.alpha;
        g_bgIdx    = c.bg;
        g_fgIdx    = c.fg;
    }
    ApplyPreset(g_level);

    if (have && c.screenW == g_screenW && c.screenH == g_screenH) {
        g_x = c.x;                             /* 分辨率没变：回到上次的位置 */
        g_y = c.y;
        ClampToScreen();
    } else {
        CenterHud();                           /* 首次运行 / 换了分辨率：居中 */
    }
    CfgSave();
}

/* 运行中分辨率 / 缩放变化：重新算尺寸并回到正中 */
static void CheckDisplayChanged(void) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    double s;
    if (sw <= 0 || sh <= 0) return;
    s = GetUiScale();
    if (sw == g_screenW && sh == g_screenH && s == g_scale) return;

    g_screenW = sw;
    g_screenH = sh;
    g_scale   = s;

    ApplyPreset(g_level);                      /* 按新分辨率 / DPI 重算尺寸 */
    CenterHud();                               /* 分辨率一变就回正中 */
    ClampToScreen();

    if (g_hwnd) {
        DestroySurface();
        CreateSurface();
        SetWindowPos(g_hwnd, HWND_TOPMOST, g_x, g_y, g_w, g_h,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        Refresh();
    }
    CfgSave();
}

/* =====================================================================
 *  3. 绘制表面：32bpp DIB + 逐像素合成 + UpdateLayeredWindow
 *     文字和背景分别有各自的 alpha，互不影响。
 * ===================================================================*/
static void DestroySurface(void) {
    if (g_dc && g_oldBmp) { SelectObject(g_dc, g_oldBmp); g_oldBmp = NULL; }
    if (g_dib) { DeleteObject(g_dib); g_dib = NULL; }
    if (g_dc)  { DeleteDC(g_dc); g_dc = NULL; }
    if (g_font) { DeleteObject(g_font); g_font = NULL; }
    g_bits = NULL;
}

static void CreateSurface(void) {
    BITMAPINFO bi;
    void *bits = NULL;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g_w;
    bi.bmiHeader.biHeight      = -g_h;          /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g_dc  = CreateCompatibleDC(NULL);
    g_dib = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (g_dib) g_oldBmp = (HBITMAP)SelectObject(g_dc, g_dib);
    g_bits = (DWORD *)bits;

    /* 灰度抗锯齿：ClearType 会产生彩色次像素，没法直接当覆盖率用 */
    g_font = CreateFontW(-g_fs, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FIXED_PITCH, FONT_NAME);
}

/* 在 DIB 上画一份「黑底白字」的覆盖率图（抗锯齿后每个像素 = 覆盖度） */
static void RenderCoverage(void) {
    RECT rc = { 0, 0, g_w, g_h };
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    WCHAR b[512];
    SYSTEMTIME st;
    int y = g_pad;

    if (!g_dc) return;
    FillRect(g_dc, &rc, black);
    SetBkMode(g_dc, TRANSPARENT);
    SetTextColor(g_dc, RGB(255,255,255));
    SelectObject(g_dc, g_font);

    GetLocalTime(&st);
    _snwprintf(b, 512, L"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    TextOutW(g_dc, g_pad, y, b, (int)wcslen(b)); y += g_lh;

    WCHAR cs[8], ms_[8], gs[8], is[8];
    PctStr(cs, g_s.cpu); PctStr(ms_, g_s.mem); PctStr(gs, g_s.gpu); PctStr(is, g_s.io);
    _snwprintf(b, 512, L"CPU %3s%%   MEM %3s%%   GPU %3s%%   IO %3s%%", cs, ms_, gs, is);
    TextOutW(g_dc, g_pad, y, b, (int)wcslen(b)); y += g_lh;

    _snwprintf(b, 512, L"PROC %d    TCP %d  (LISTEN %d / ESTAB %d)",
        g_s.procs, g_s.tcpTotal, g_s.tcpListen, g_s.tcpEstab);
    TextOutW(g_dc, g_pad, y, b, (int)wcslen(b)); y += g_lh;

    TextOutW(g_dc, g_pad, y, g_s.disks, (int)wcslen(g_s.disks));
}

/* 把「文字 @ TEXT_ALPHA」盖到「背景 @ 背景不透明度」上，写成预乘 BGRA。
 * 每个像素：cov = 文字覆盖率，
 *   at = TEXT_ALPHA * cov        （文字层 alpha）
 *   ab = 背景档位 alpha          （背景层 alpha，透明档为 0）
 *   A  = at + ab * (1 - at)      （文字叠在背景上的 source-over）
 *   C  = 文字色 * at + 背景色 * ab * (1 - at)   （预乘）
 */
static void Compose(void) {
    const BgPreset *bp = &g_bgPresets[g_bgIdx];
    COLORREF fg = g_fgPresets[g_fgIdx].color;
    int ab = bp->transparent ? 0 : (int)g_alphaPresets[g_alphaIdx];
    int fgR = GetRValue(fg), fgG = GetGValue(fg), fgB = GetBValue(fg);
    int bgR = GetRValue(bp->color), bgG = GetGValue(bp->color), bgB = GetBValue(bp->color);
    int n = g_w * g_h;
    BYTE *p = (BYTE *)g_bits;
    int i;

    if (!g_bits) return;
    for (i = 0; i < n; ++i, p += 4) {
        int cov = p[0];                         /* 黑底白字：B/G/R 相等 = 覆盖率 */
        int at  = TEXT_ALPHA * cov / 255;
        int inv = 255 - at;
        int bgT = ab * inv;                     /* 0..65025 */
        int a   = at + bgT / 255;
        int r   = fgR * at / 255 + bgR * bgT / 65025;
        int g   = fgG * at / 255 + bgG * bgT / 65025;
        int b   = fgB * at / 255 + bgB * bgT / 65025;
        if (a > 255) a = 255;
        if (r > a) r = a;
        if (g > a) g = a;
        if (b > a) b = a;
        p[0] = (BYTE)b;
        p[1] = (BYTE)g;
        p[2] = (BYTE)r;
        p[3] = (BYTE)a;
    }
}

static void Present(void) {
    HDC screen;
    POINT src, dst;
    SIZE sz;
    BLENDFUNCTION bf;

    if (!g_hwnd || !g_dc) return;
    screen = GetDC(NULL);
    src.x = 0; src.y = 0;
    dst.x = g_x; dst.y = g_y;
    sz.cx = g_w; sz.cy = g_h;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_hwnd, screen, &dst, &sz, g_dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
    /* UpdateLayeredWindow 会顺带把窗口的置顶状态刷掉，这里补一次，
     * 否则面板会被后来打开的窗口盖住 */
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

/* 重画 + 送显 */
static void Refresh(void) {
    if (!g_dc || !g_bits) return;
    RenderCoverage();
    Compose();
    Present();
}

/* =====================================================================
 *  4. 拖动 / 滚轮缩放 / 键盘调色
 * ===================================================================*/
static BOOL PointInWindow(POINT pt) {
    RECT r;
    if (!g_hwnd || !GetWindowRect(g_hwnd, &r)) return FALSE;
    return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
}

static BOOL CursorOverHud(void) {
    POINT pt;
    if (!g_hwnd) return FALSE;
    if (!GetCursorPos(&pt)) return FALSE;
    return PointInWindow(pt);
}

/* Ctrl + Alt 必须同时按住；键盘操作还要求鼠标停在面板上（与滚轮一致） */
static BOOL IsCtrlAltDown(void) {
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
           (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
}

static void MoveHudTo(int x, int y) {
    if (x > g_screenW - g_w) x = g_screenW - g_w;
    if (y > g_screenH - g_h) y = g_screenH - g_h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x == g_x && y == g_y) return;
    g_x = x; g_y = y;
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    Present();                                 /* 内容没变，只挪位置 */
}

/* 切换尺寸档位：窗口中心保持不动，表面 / 字号整体重建 */
static void SetLevel(int level) {
    int cx, cy;
    if (level < 0) level = 0;
    if (level >= PRESET_COUNT) level = PRESET_COUNT - 1;
    if (level == g_level) return;

    cx = g_x + g_w / 2;
    cy = g_y + g_h / 2;

    ApplyPreset(level);
    g_x = cx - g_w / 2;
    g_y = cy - g_h / 2;
    ClampToScreen();

    DestroySurface();
    CreateSurface();
    SetWindowPos(g_hwnd, HWND_TOPMOST, g_x, g_y, g_w, g_h,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    Refresh();
    CfgTouch();
}

static void SetAlphaIdx(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= ALPHA_COUNT) idx = ALPHA_COUNT - 1;
    if (idx == g_alphaIdx) return;
    g_alphaIdx = idx;
    Refresh();
    CfgTouch();
}

static void StepAlpha(int delta) {
    SetAlphaIdx(g_alphaIdx + delta);
}

static void SetBgIdx(int idx) {
    if (BG_COUNT <= 0) return;
    while (idx < 0) idx += BG_COUNT;
    idx %= BG_COUNT;
    if (idx == g_bgIdx) return;
    g_bgIdx = idx;
    Refresh();
    CfgTouch();
}

static void StepBg(int delta) {
    SetBgIdx(g_bgIdx + delta);
}

static void SetFgIdx(int idx) {
    if (idx < 0 || idx >= FG_COUNT) return;
    if (idx == g_fgIdx) return;
    g_fgIdx = idx;
    Refresh();
    CfgTouch();
}

/* 鼠标钩子：Ctrl+Alt + 左键拖动、Ctrl+Alt + 滚轮缩放 */
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HC_ACTION) {
        const MSLLHOOKSTRUCT *ms = (const MSLLHOOKSTRUCT *)lp;
        switch (wp) {
        case WM_LBUTTONDOWN:
            if (!g_dragging && IsCtrlAltDown() && PointInWindow(ms->pt)) {
                RECT r;
                g_dragging = TRUE;
                g_dragStart = ms->pt;
                if (GetWindowRect(g_hwnd, &r)) { g_dragWin.x = r.left; g_dragWin.y = r.top; }
                return 1;                       /* 吞掉这次按下，别传给下面的程序 */
            }
            break;

        case WM_MOUSEMOVE:
            if (g_dragging) {
                if (!IsCtrlAltDown()) {         /* 拖到一半松了 Ctrl/Alt，就地结束 */
                    g_dragging = FALSE;
                    CfgTouch();
                    break;
                }
                MoveHudTo(g_dragWin.x + (ms->pt.x - g_dragStart.x),
                          g_dragWin.y + (ms->pt.y - g_dragStart.y));
            }
            break;

        case WM_LBUTTONUP:
            if (g_dragging) {
                g_dragging = FALSE;
                CfgTouch();
                return 1;                       /* 松开也吃掉，避免下面收到落单的抬起 */
            }
            break;

        case WM_MOUSEWHEEL:
            if (IsCtrlAltDown() && PointInWindow(ms->pt)) {
                DWORD now = GetTickCount();
                if (now - g_lastWheel >= WHEEL_GAP_MS) {
                    int delta = (SHORT)HIWORD(ms->mouseData);
                    g_lastWheel = now;
                    if (delta > 0)      SetLevel(g_level + 1);
                    else if (delta < 0) SetLevel(g_level - 1);
                }
                return 1;                       /* 吃掉滚动，别让下面的程序跟着滚 */
            }
            break;
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wp, lp);
}

/* 键盘钩子：Ctrl+Alt + 方向键 / G W Y R B P（同样要求鼠标停在面板上） */
static LRESULT CALLBACK LowLevelKeyProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        BOOL down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);

        if (down && IsCtrlAltDown() && CursorOverHud()) {
            switch (k->vkCode) {
            case VK_UP:    StepAlpha(+1); g_swallowVk = k->vkCode; return 1;
            case VK_DOWN:  StepAlpha(-1); g_swallowVk = k->vkCode; return 1;
            case VK_RIGHT: StepBg(+1);    g_swallowVk = k->vkCode; return 1;
            case VK_LEFT:  StepBg(-1);    g_swallowVk = k->vkCode; return 1;
            case 'G':      SetFgIdx(0);   g_swallowVk = k->vkCode; return 1;
            case 'W':      SetFgIdx(1);   g_swallowVk = k->vkCode; return 1;
            case 'Y':      SetFgIdx(2);   g_swallowVk = k->vkCode; return 1;
            case 'R':      SetFgIdx(3);   g_swallowVk = k->vkCode; return 1;
            case 'B':      SetFgIdx(4);   g_swallowVk = k->vkCode; return 1;
            case 'P':      SetFgIdx(5);   g_swallowVk = k->vkCode; return 1;
            }
        }
        /* 吃掉被处理按键对应的抬起事件，避免下面的程序收到落单的 keyup */
        if (!down && g_swallowVk && k->vkCode == g_swallowVk) {
            g_swallowVk = 0;
            return 1;
        }
    }
    return CallNextHookEx(g_kbdHook, nCode, wp, lp);
}

/* =====================================================================
 *  5. 系统数据采集（与老版本逻辑一致）
 * ===================================================================*/
static void TryInitGPU(void) {
    if (g_gpuQ) { PdhCloseQuery(g_gpuQ); g_gpuQ = NULL; }
    g_gpuOk = FALSE;
    if (PdhOpenQueryW(NULL, 0, &g_gpuQ) != ERROR_SUCCESS) { g_gpuQ = NULL; return; }
    if (PdhAddEnglishCounterW(g_gpuQ, GPU_PATH, 0, &g_gpuC) != ERROR_SUCCESS) {
        PdhCloseQuery(g_gpuQ); g_gpuQ = NULL; return;
    }
    g_gpuOk = TRUE; g_gpuFail = 0;
}

static void TryInitIO(void) {
    if (g_ioQ) { PdhCloseQuery(g_ioQ); g_ioQ = NULL; }
    g_ioOk = FALSE;
    if (PdhOpenQueryW(NULL, 0, &g_ioQ) != ERROR_SUCCESS) { g_ioQ = NULL; return; }
    if (PdhAddEnglishCounterW(g_ioQ, IO_PATH, 0, &g_ioC) != ERROR_SUCCESS) {
        PdhCloseQuery(g_ioQ); g_ioQ = NULL; return;
    }
    g_ioOk = TRUE; g_ioFail = 0;
}

static void UpdateCPU(void) {
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return;
    ULONGLONG i = ((ULONGLONG)fi.dwHighDateTime << 32) | fi.dwLowDateTime;
    ULONGLONG k = ((ULONGLONG)fk.dwHighDateTime << 32) | fk.dwLowDateTime;
    ULONGLONG u = ((ULONGLONG)fu.dwHighDateTime << 32) | fu.dwLowDateTime;
    if (!g_cpuInit) { g_pi = i; g_pk = k; g_pu = u; g_cpuInit = TRUE; return; }
    ULONGLONG di = i - g_pi, dk = k - g_pk, du = u - g_pu;
    ULONGLONG total = dk + du;
    if (total > 0) {
        double p = (double)(total - di) * 100.0 / (double)total;
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        g_cpuPct = p;
    }
    g_pi = i; g_pk = k; g_pu = u;
}

static int ReadGPU(void) {
    if (!g_gpuOk) return -1;
    if (PdhCollectQueryData(g_gpuQ) != ERROR_SUCCESS) {
        if (++g_gpuFail >= 10) TryInitGPU();
        return -1;
    }
    g_gpuFail = 0;
    DWORD sz = 0, cnt = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(g_gpuC, PDH_FMT_DOUBLE, &sz, &cnt, NULL);
    if ((DWORD)st != PDH_MORE_DATA) return st == ERROR_SUCCESS ? 0 : -1;
    if (sz > g_gpuBufSz) {
        PDH_FMT_COUNTERVALUE_ITEM_W *nb = (PDH_FMT_COUNTERVALUE_ITEM_W *)realloc(g_gpuBuf, sz);
        if (!nb) return -1;
        g_gpuBuf = nb; g_gpuBufSz = sz;
    }
    if (!g_gpuBuf) return -1;
    DWORD sz2 = g_gpuBufSz;
    st = PdhGetFormattedCounterArrayW(g_gpuC, PDH_FMT_DOUBLE, &sz2, &cnt, g_gpuBuf);
    if ((DWORD)st == PDH_MORE_DATA) {
        PDH_FMT_COUNTERVALUE_ITEM_W *nb = (PDH_FMT_COUNTERVALUE_ITEM_W *)realloc(g_gpuBuf, sz2);
        if (!nb) return -1;
        g_gpuBuf = nb; g_gpuBufSz = sz2;
        st = PdhGetFormattedCounterArrayW(g_gpuC, PDH_FMT_DOUBLE, &sz2, &cnt, g_gpuBuf);
    }
    if (st != ERROR_SUCCESS) return -1;
    double sum = 0;
    for (DWORD x = 0; x < cnt; ++x) {
        DWORD cs = g_gpuBuf[x].FmtValue.CStatus;
        if (cs == PDH_CSTATUS_VALID_DATA || cs == PDH_CSTATUS_NEW_DATA) {
            double v = g_gpuBuf[x].FmtValue.doubleValue;
            if (v > 0 && v < 1e6) sum += v;
        }
    }
    if (sum > 100.0) sum = 100.0;
    return (int)(sum + 0.5);
}

static int ReadIO(void) {
    if (!g_ioOk) return -1;
    if (PdhCollectQueryData(g_ioQ) != ERROR_SUCCESS) {
        if (++g_ioFail >= 10) TryInitIO();
        return -1;
    }
    g_ioFail = 0;
    PDH_FMT_COUNTERVALUE v;
    if (PdhGetFormattedCounterValue(g_ioC, PDH_FMT_DOUBLE, NULL, &v) != ERROR_SUCCESS) return -1;
    if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) return -1;
    double d = v.doubleValue;
    if (d < 0) d = 0;
    if (d > 100) d = 100;
    return (int)(d + 0.5);
}

static int CountProcs(void) {
    static DWORD pids[4096];
    DWORD needed = 0;
    if (!EnumProcesses(pids, (DWORD)sizeof(pids), &needed)) return -1;
    return (int)(needed / sizeof(DWORD));
}

static BOOL QueryTcp(ULONG fam, int *total, int *listen, int *estab) {
    DWORD sz = 0;
    DWORD rc = GetExtendedTcpTable(NULL, &sz, FALSE, fam, TCP_TABLE_OWNER_PID_ALL, 0);
    if (rc != ERROR_INSUFFICIENT_BUFFER || sz == 0) return FALSE;
    if (sz > g_tcpBufSize) {
        DWORD newsz = sz + sz / 2;
        BYTE *nb = (BYTE *)realloc(g_tcpBuf, newsz);
        if (!nb) return FALSE;
        g_tcpBuf = nb; g_tcpBufSize = newsz;
    }
    rc = GetExtendedTcpTable(g_tcpBuf, &sz, FALSE, fam, TCP_TABLE_OWNER_PID_ALL, 0);
    if (rc == ERROR_INSUFFICIENT_BUFFER) {
        BYTE *nb = (BYTE *)realloc(g_tcpBuf, sz);
        if (!nb) return FALSE;
        g_tcpBuf = nb; g_tcpBufSize = sz;
        rc = GetExtendedTcpTable(g_tcpBuf, &sz, FALSE, fam, TCP_TABLE_OWNER_PID_ALL, 0);
    }
    if (rc != ERROR_SUCCESS) return FALSE;
    DWORD n;
    if (fam == AF_INET) {
        PMIB_TCPTABLE_OWNER_PID t = (PMIB_TCPTABLE_OWNER_PID)g_tcpBuf;
        n = t->dwNumEntries;
        for (DWORD x = 0; x < n; ++x) {
            if (t->table[x].dwState == MIB_TCP_STATE_LISTEN) (*listen)++;
            else if (t->table[x].dwState == MIB_TCP_STATE_ESTAB) (*estab)++;
        }
    } else {
        PMIB_TCP6TABLE_OWNER_PID t = (PMIB_TCP6TABLE_OWNER_PID)g_tcpBuf;
        n = t->dwNumEntries;
        for (DWORD x = 0; x < n; ++x) {
            if (t->table[x].dwState == MIB_TCP_STATE_LISTEN) (*listen)++;
            else if (t->table[x].dwState == MIB_TCP_STATE_ESTAB) (*estab)++;
        }
    }
    *total += (int)n;
    return TRUE;
}

static void UpdateDisks(WCHAR *out, size_t cap) {
    out[0] = 0;
    WCHAR drives[512];
    DWORD n = GetLogicalDriveStringsW(511, drives);
    if (n == 0 || n >= 511) { wcscpy(out, L"DISK --"); return; }
    size_t used = 0; BOOL any = FALSE;
    for (WCHAR *p = drives; *p; p += wcslen(p) + 1) {
        if (GetDriveTypeW(p) != DRIVE_FIXED) continue;
        ULARGE_INTEGER fb;
        if (!GetDiskFreeSpaceExW(p, &fb, NULL, NULL)) continue;
        double gb = (double)fb.QuadPart / 1073741824.0;
        WCHAR tmp[64];
        int k = _snwprintf(tmp, 64, L"%c: %.2fG   ", p[0], gb);
        if (k <= 0) continue;
        if (used + (size_t)k + 1 >= cap) break;
        wcscpy(out + used, tmp);
        used += (size_t)k;
        any = TRUE;
    }
    if (!any) wcscpy(out, L"DISK --");
}

static void UpdateAll(void) {
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    g_s.mem  = GlobalMemoryStatusEx(&m) ? (int)m.dwMemoryLoad : -1;
    g_s.cpu  = (int)(g_cpuPct + 0.5);
    g_s.gpu  = ReadGPU();
    g_s.io   = ReadIO();
    g_s.procs = CountProcs();
    int tot = 0, lst = 0, est = 0;
    QueryTcp(AF_INET,  &tot, &lst, &est);
    QueryTcp(AF_INET6, &tot, &lst, &est);
    g_s.tcpTotal = tot; g_s.tcpListen = lst; g_s.tcpEstab = est;
    UpdateDisks(g_s.disks, 512);
}

static void PctStr(WCHAR *o, int v) {
    if (v < 0) wcscpy(o, L"--");
    else _snwprintf(o, 8, L"%d", v);
}

/* =====================================================================
 *  6. 绘制：见第 3 节的 RenderCoverage / Compose / Present
 *     （WM_PAINT 只做重画，内容由 UpdateLayeredWindow 送显）
 * ===================================================================*/

/* =====================================================================
 *  7. 窗口过程 / 入口
 * ===================================================================*/
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateSurface();
        TryInitGPU();
        TryInitIO();
        UpdateCPU();
        UpdateAll();
        SetTimer(hwnd, 1, UPDATE_INTERVAL_MS, NULL);
        return 0;

    case WM_TIMER:
        if (wp == 1) {
            UpdateCPU();
            UpdateAll();
            CheckDisplayChanged();             /* 兜底：每秒比对一次分辨率 */
            if (g_savePending) CfgSave();
            Refresh();
        }
        return 0;

    case WM_DISPLAYCHANGE_:
        CheckDisplayChanged();                 /* 分辨率变了：回正中 */
        return 0;

    case WM_DPICHANGED:
        CheckDisplayChanged();                 /* DPI 缩放变了：重新算尺寸并回正中 */
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        Refresh();
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        CfgSave();
        if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = NULL; }
        if (g_kbdHook)   { UnhookWindowsHookEx(g_kbdHook);   g_kbdHook = NULL; }
        DestroySurface();
        if (g_gpuQ) PdhCloseQuery(g_gpuQ);
        if (g_ioQ)  PdhCloseQuery(g_ioQ);
        if (g_gpuBuf) free(g_gpuBuf);
        if (g_tcpBuf) free(g_tcpBuf);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd; (void)nShow;

    EnableDpiAwareness();            /* 逐显示器 V2：坐标永远是物理像素 */
    BuildCfgPath();                  /* exe 同目录 hudcinfo.ini */
    InitLayout();                    /* 读配置 -> 档位 / 外观 / 坐标 */

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"StatOverlayCls";
    if (!RegisterClassExW(&wc)) return 1;

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"StatOverlayCls", L"StatOverlay", WS_POPUP,
        g_x, g_y, g_w, g_h, NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    Refresh();                       /* 逐像素 alpha，第一次送显 */
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwnd);

    /* 全局钩子：Ctrl+Alt 拖动 / 缩放 / 调色。装不上也不影响原来的穿透行为。 */
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(NULL), 0);
    g_kbdHook   = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyProc, GetModuleHandleW(NULL), 0);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_kbdHook)   UnhookWindowsHookEx(g_kbdHook);
    return (int)msg.wParam;
}
