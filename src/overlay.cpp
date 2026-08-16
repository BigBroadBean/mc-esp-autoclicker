// ============================================================
//  overlay.cpp — 顶层透明覆盖层 + GDI 绘制
// ============================================================
#include "common.h"
#include "overlay.h"
#include <emmintrin.h>  // SSE2
#include <cmath>

// 窗口类名用 GUID 风格随机串，避免被识别为已知覆盖层类名
static const wchar_t* kCls = L"{D3E74A7B-8E2C-4B2F-9A1C-6F2B3C4D5E6F}";

// 防截图核心：把覆盖层从所有截图 API 中排除，确保游戏 mod 截图上传服务器
// 时看不到 ESP 内容。
//   WDA_EXCLUDEFROMCAPTURE (0x11, Win10 2004+)：从 PrintWindow / BitBlt /
//   屏幕录制 / DXGI 桌面采集 / Java Robot 截屏中完全排除本窗口；
//   旧系统回退 WDA_MONITOR (0x01)：截图里本窗口显示为黑块。
static void apply_capture_exclusion(HWND hwnd) {
    typedef BOOL(WINAPI* SetWindowDisplayAffinity_t)(HWND, DWORD);
    auto fn = (SetWindowDisplayAffinity_t)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetWindowDisplayAffinity");
    if (!fn) { esp_log("[overlay] SetWindowDisplayAffinity 不可用"); return; }
    if (fn(hwnd, 0x11)) { esp_log("[overlay] 已启用 WDA_EXCLUDEFROMCAPTURE"); return; }
    if (fn(hwnd, 0x01)) { esp_log("[overlay] 回退 WDA_MONITOR（截图里显示黑块）"); return; }
    esp_log("[overlay] SetWindowDisplayAffinity 失败 %lu", GetLastError());
}

bool Overlay::create(HWND gameHwnd) {
    if (m_hwnd) return true;
    if (!gameHwnd || !IsWindow(gameHwnd)) return false;

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    // 顶层 owned popup 覆盖层（owner = 游戏窗口），与游戏是【两个独立窗口】：
    //  - 游戏 mod 用 BitBlt/PrintWindow 截游戏窗口时，只会截到游戏自身客户区，
    //    覆盖层内容不会被带上（若仍用子窗口则会被一起截走）；
    //  - 再用 SetWindowDisplayAffinity 排除本窗口，连整屏截图（Robot/DXGI/录制）
    //    也看不到 ESP；
    //  - WS_EX_TRANSPARENT + WS_EX_NOACTIVATE：鼠标点击/焦点透传给游戏；
    //  - WS_EX_TOOLWINDOW：不出现在任务栏/Alt-Tab。
    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kCls, L"", WS_POPUP, 0, 0, 8, 8, gameHwnd, nullptr, wc.hInstance, nullptr);
    if (!m_hwnd) { esp_log("[overlay] CreateWindowExW 失败 %lu", GetLastError()); return false; }

    apply_capture_exclusion(m_hwnd);

    // 32 位顶底 DIB + 内存 DC
    m_memDc = CreateCompatibleDC(nullptr);
    if (!m_memDc) return false;
    m_scratchDc = CreateCompatibleDC(nullptr);
    if (!m_scratchDc) return false;

    esp_log("[overlay] 覆盖层已创建（顶层 owned popup，截图排除）");
    return true;
}

void Overlay::destroy() {
    if (m_oldBmp) { SelectObject(m_memDc, m_oldBmp); m_oldBmp = nullptr; }
    if (m_dib) { DeleteObject(m_dib); m_dib = nullptr; }
    if (m_memDc) { DeleteDC(m_memDc); m_memDc = nullptr; }
    if (m_scratchDc) { DeleteDC(m_scratchDc); m_scratchDc = nullptr; }
    if (m_font) { DeleteObject(m_font); m_font = nullptr; }
    for (auto& kv : m_pens) DeleteObject(kv.second);
    m_pens.clear();
    m_textBmps.clear();
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    m_pixels = nullptr;
}

static void ensure_size(Overlay* o, int w, int h, HDC memDc, HBITMAP* dib,
                        uint32_t** pixels, int& curW, int& curH, HGDIOBJ& oldBmp) {
    if (w == curW && h == curH) return;
    if (*dib) { SelectObject(memDc, oldBmp); DeleteObject(*dib); *dib = nullptr; }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // 顶底
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    *dib = CreateDIBSection(memDc, &bi, DIB_RGB_COLORS, (void**)pixels, nullptr, 0);
    if (*dib) oldBmp = SelectObject(memDc, *dib);
    curW = w; curH = h;
}

bool Overlay::position(HWND gameHwnd) {
    if (!m_hwnd) return false;
    if (!gameHwnd || !IsWindow(gameHwnd)) return false;
    if (IsIconic(gameHwnd)) { if (IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE); return false; }

    RECT rc;
    GetClientRect(gameHwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return false;

    // 客户区左上角 → 屏幕坐标（顶层窗口必须用屏幕坐标定位，才能精确压住游戏客户区）
    POINT org{0, 0};
    ClientToScreen(gameHwnd, &org);
    int cw = rc.right, ch = rc.bottom;

    // 用覆盖层【实际屏幕矩形】与期望位置比较，偏差才 SetWindowPos：
    //  - 静止时零重排（不闪、不卡）；
    //  - 游戏窗口移动时 1 帧跟上（owned 窗口会被系统自动跟随，这里再校正偏差）；
    //  - SWP_NOZORDER：保持 owned 窗口 Z 序（创建时已在游戏之上），不重复提升。
    RECT win;
    GetWindowRect(m_hwnd, &win);
    bool changed = (win.left != org.x || win.top != org.y ||
                    (win.right - win.left) != cw || (win.bottom - win.top) != ch);
    if (changed) {
        m_lastX = org.x; m_lastY = org.y;
        SetWindowPos(m_hwnd, HWND_TOP, org.x, org.y, cw, ch,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOZORDER);
        ensure_size(this, cw, ch, m_memDc, &m_dib, &m_pixels, m_w, m_h, m_oldBmp);
    }
    if (!IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_SHOWNA);   // 显示但不激活
    return true;
}

uint32_t* Overlay::lock(int& w, int& h) {
    w = m_w; h = m_h;
    if (m_pixels && m_w > 0 && m_h > 0) memset(m_pixels, 0, (size_t)m_w * m_h * 4);
    return m_pixels;
}

uint32_t* Overlay::lockNoClear(int& w, int& h) {
    w = m_w; h = m_h;
    return m_pixels;
}

// 菜单打开时移除 WS_EX_TRANSPARENT：覆盖层截获鼠标，游戏保持前台（NOACTIVATE）。
// 菜单关闭时恢复穿透，鼠标输入完全还给游戏。
void Overlay::set_clickable(bool on) {
    if (!m_hwnd) return;
    LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (on) ex &= ~((LONG_PTR)WS_EX_TRANSPARENT);
    else    ex |= (LONG_PTR)WS_EX_TRANSPARENT;
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

bool Overlay::begin_offscreen(int w, int h, uint32_t* pixels) {
    if (!pixels || w <= 0 || h <= 0) return false;
    m_offscreenSavedPixels = m_pixels;
    m_offscreenSavedW = m_w;
    m_offscreenSavedH = m_h;
    m_pixels = pixels;
    m_w = w;
    m_h = h;
    return true;
}

void Overlay::end_offscreen() {
    m_pixels = m_offscreenSavedPixels;
    m_w = m_offscreenSavedW;
    m_h = m_offscreenSavedH;
    m_offscreenSavedPixels = nullptr;
    m_offscreenSavedW = 0;
    m_offscreenSavedH = 0;
}

// 预乘 alpha 后处理（SSE2 版本）：GDI 只写 RGB（alpha=0），这里把
// “alpha==0 但 RGB 非 0”的像素设为不透明；已有 alpha（半透明填充）保持不动。
// 逐 4 像素批量处理，避免 2M+ 像素的逐字节循环成为性能瓶颈。
static void fix_alpha_sse2(uint32_t* p, size_t n) {
    const __m128i A = _mm_set1_epi32(0xFF000000);   // alpha 掩码
    const __m128i R = _mm_set1_epi32(0x00FFFFFF);   // rgb 掩码
    const __m128i FF = _mm_set1_epi32(0xFF000000);  // 要写入的 alpha
    const __m128i Z = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128i v = _mm_loadu_si128((const __m128i*)(p + i));
        __m128i aZero = _mm_cmpeq_epi32(_mm_and_si128(v, A), Z);        // alpha==0 → FF
        __m128i rgbNz  = _mm_cmpeq_epi32(_mm_and_si128(v, R), Z);       // rgb==0 → FF
        rgbNz = _mm_xor_si128(rgbNz, _mm_set1_epi32(~0u));              // rgb!=0 → FF
        __m128i fix = _mm_and_si128(aZero, rgbNz);                      // 需要补 alpha
        __m128i opaque = _mm_or_si128(v, FF);
        __m128i res = _mm_or_si128(_mm_and_si128(fix, opaque),
                                   _mm_andnot_si128(fix, v));
        _mm_storeu_si128((__m128i*)(p + i), res);
    }
    for (; i < n; ++i) {
        uint32_t v = p[i];
        if ((v & 0xFF000000) == 0 && (v & 0x00FFFFFF) != 0) p[i] = v | 0xFF000000;
    }
}

void Overlay::present() {
    if (!m_hwnd || !m_pixels) return;
    int n = m_w * m_h;
    if (n > 0) fix_alpha_sse2(m_pixels, (size_t)n);
    POINT src{0, 0};
    // ptDst=NULL：只更新内容，绝不移动窗口（位置完全由 position() 的 SetWindowPos 管）。
    // 之前传 ptDst 每帧把窗口拉回旧位置，与 owned 窗口自动跟随互相打架 → 偏移/闪烁/卡顿。
    SIZE sz{m_w, m_h};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(m_hwnd, nullptr, nullptr, &sz, m_memDc, &src, 0, &bf, ULW_ALPHA);
}

HPEN Overlay::getPen(uint32_t rgb, int width) {
    uint64_t key = ((uint64_t)rgb << 8) | (uint32_t)(width & 0xFF);
    auto it = m_pens.find(key);
    if (it != m_pens.end()) return it->second;
    HPEN pen = CreatePen(PS_SOLID, width, toColorRef(rgb));
    m_pens.emplace(key, pen);
    return pen;
}

// ------------------------------------------------------------
// 抗锯齿线段光栅化（软件，预乘 alpha）
//   GDI 的 MoveToEx/LineTo 是走样（无抗锯齿）且每线段一次 GDI 状态切换。
//   这里改为 Xiaolin Wu 算法直接写像素缓冲：边缘按覆盖率渐隐，
//   得到 GPU 级别的丝滑线条，且整帧只一次 UpdateLayeredWindow。
// ------------------------------------------------------------
static inline double fract_(double v) { return v - std::floor(v); }
static inline double invfract_(double v) { return 1.0 - fract_(v); }

// 以预乘 alpha 的 source-over 混合写入一个像素
static inline void aa_plot(uint32_t* px, int W, int H, int x, int y,
                           double cover, uint32_t rgb, double baseA) {
    if (cover <= 0.0 || x < 0 || y < 0 || x >= W || y >= H) return;
    double a = baseA * cover;                       // 0..1
    if (a <= 0.0) return;
    double sr = ((rgb >> 16) & 255) * a;            // 预乘 src
    double sg = ((rgb >> 8) & 255) * a;
    double sb = (rgb & 255) * a;
    uint32_t d = px[(size_t)y * W + x];
    double dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255, da = (d >> 24) & 255;
    double ia = 1.0 - a;
    uint32_t oa = (uint32_t)(a * 255.0 + da * ia);
    uint32_t or_ = (uint32_t)(sr + dr * ia);
    uint32_t og = (uint32_t)(sg + dg * ia);
    uint32_t ob = (uint32_t)(sb + db * ia);
    if (oa > 255) oa = 255; if (or_ > 255) or_ = 255; if (og > 255) og = 255; if (ob > 255) ob = 255;
    px[(size_t)y * W + x] = (oa << 24) | (or_ << 16) | (og << 8) | ob;
}

// Xiaolin Wu：1px 抗锯齿线段（subpixel 精度）
static void wu_line(uint32_t* px, int W, int H,
                    double x0, double y0, double x1, double y1,
                    uint32_t rgb, double baseA) {
    bool steep = std::fabs(y1 - y0) > std::fabs(x1 - x0);
    if (steep) { std::swap(x0, y0); std::swap(x1, y1); }
    if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); }

    double dx = x1 - x0, dy = y1 - y0;
    double gradient = (dx == 0.0) ? 1.0 : dy / dx;

    auto plotP = [&](int xi, int yi, double c) {
        if (steep) aa_plot(px, W, H, yi, xi, c, rgb, baseA);
        else       aa_plot(px, W, H, xi, yi, c, rgb, baseA);
    };

    // 首端点
    int xpxl1 = (int)std::floor(x0);
    double yend = y0 + gradient * (xpxl1 - x0);
    double xgap = invfract_(x0 + 0.5);
    int ypxl1 = (int)std::floor(yend);
    plotP(xpxl1, ypxl1, invfract_(yend) * xgap);
    plotP(xpxl1, ypxl1 + 1, fract_(yend) * xgap);
    double intery = yend + gradient;

    // 尾端点
    int xpxl2 = (int)std::floor(x1);
    double yend2 = y1 + gradient * (xpxl2 - x1);
    double xgap2 = invfract_(x1 + 0.5);
    int ypxl2 = (int)std::floor(yend2);
    plotP(xpxl2, ypxl2, invfract_(yend2) * xgap2);
    plotP(xpxl2, ypxl2 + 1, fract_(yend2) * xgap2);

    // 主体
    for (int x = xpxl1 + 1; x <= xpxl2 - 1; ++x) {
        plotP(x, (int)intery, invfract_(intery));
        plotP(x, (int)intery + 1, fract_(intery));
        intery += gradient;
    }
}

// Liang-Barsky：把线段裁剪到 [xmin,xmax]×[ymin,ymax]。
// 近平面裁剪出的交点可能投影到屏幕外极远处，若不裁剪，
// Wu 光栅化会按极长跨距循环导致卡顿。
static bool clip_segment(double& x0, double& y0, double& x1, double& y1,
                         double xmin, double ymin, double xmax, double ymax) {
    double dx = x1 - x0, dy = y1 - y0;
    double p[4] = {-dx, dx, -dy, dy};
    double q[4] = {x0 - xmin, xmax - x0, y0 - ymin, ymax - y0};
    double u1 = 0.0, u2 = 1.0;
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0) return false;
        } else {
            double r = q[i] / p[i];
            if (p[i] < 0.0) { if (r > u2) return false; if (r > u1) u1 = r; }
            else             { if (r < u1) return false; if (r < u2) u2 = r; }
        }
    }
    if (u2 < u1) return false;
    x0 += u1 * dx; y0 += u1 * dy;
    x1  = x0 + (u2 - u1) * dx; y1 = y0 + (u2 - u1) * dy;
    return true;
}

void Overlay::drawLine(float x1, float y1, float x2, float y2, uint32_t rgb, int width) {
    if (!m_pixels || m_w <= 0 || m_h <= 0) return;
    if (width < 1) width = 1;

    double ax = x1, ay = y1, bx = x2, by = y2;
    if (!clip_segment(ax, ay, bx, by, -1.0, -1.0, (double)m_w, (double)m_h)) return;
    double dx = bx - ax, dy = by - ay;
    double len = std::sqrt(dx * dx + dy * dy);

    if (width == 1) {
        wu_line(m_pixels, m_w, m_h, ax, ay, bx, by, rgb, 1.0);
        return;
    }

    // 粗线：沿法线偏移的若干条 1px Wu 线叠加（核心不透明、外缘抗锯齿）
    if (len < 1e-4) { wu_line(m_pixels, m_w, m_h, ax, ay, bx, by, rgb, 1.0); return; }
    double nx = -dy / len, ny = dx / len;   // 单位法线
    double half = width / 2.0;
    // 奇数宽：偏移 -half..half；偶数宽：偏移 -half+0.5..half-0.5
    double start = (width & 1) ? -std::floor(half) : -half + 0.5;
    for (double off = start; off <= half; off += 1.0) {
        wu_line(m_pixels, m_w, m_h,
                ax + nx * off, ay + ny * off,
                bx + nx * off, by + ny * off,
                rgb, 1.0);
    }
}

void Overlay::drawRect(float x1, float y1, float x2, float y2, uint32_t rgb, int width) {
    if (!m_memDc) return;
    HGDIOBJ oldPen = SelectObject(m_memDc, getPen(rgb, width));
    HGDIOBJ oldBrush = SelectObject(m_memDc, GetStockObject(NULL_BRUSH));
    Rectangle(m_memDc, (int)x1, (int)y1, (int)x2, (int)y2);
    SelectObject(m_memDc, oldBrush);
    SelectObject(m_memDc, oldPen);
}

// 半透明填充：直接写像素（预乘 alpha），alpha≈70
void Overlay::fillRect(float x1, float y1, float x2, float y2, uint32_t rgb) {
    fillRectAlpha(x1, y1, x2, y2, rgb, 70.0f / 255.0f);
}

void Overlay::fillRectAlpha(float x1, float y1, float x2, float y2, uint32_t rgb, float alpha) {
    if (!m_pixels || alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;
    int x0 = (int)x1, y0 = (int)y1, x1i = (int)x2, y1i = (int)y2;
    if (x0 > x1i) std::swap(x0, x1i);
    if (y0 > y1i) std::swap(y0, y1i);
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1i = std::min(m_w, x1i); y1i = std::min(m_h, y1i);
    const int a = (int)(alpha * 255.0f + 0.5f);
    uint32_t sr = (rgb >> 16) & 255, sg = (rgb >> 8) & 255, sb = rgb & 255;
    if (a >= 255) {
        uint32_t val = (255u << 24) | (sr << 16) | (sg << 8) | sb;
        for (int yy = y0; yy < y1i; ++yy) {
            uint32_t* row = m_pixels + (size_t)yy * m_w;
            for (int xx = x0; xx < x1i; ++xx) row[xx] = val;
        }
        return;
    }
    uint32_t pr = (sr * a) / 255, pg = (sg * a) / 255, pb = (sb * a) / 255;
    int inv = 255 - a;
    for (int yy = y0; yy < y1i; ++yy) {
        uint32_t* row = m_pixels + (size_t)yy * m_w;
        for (int xx = x0; xx < x1i; ++xx) {
            uint32_t d = row[xx];
            uint32_t da = (d >> 24) & 255;
            uint32_t dr = ((d >> 16) & 255) * inv / 255;
            uint32_t dg = ((d >> 8) & 255) * inv / 255;
            uint32_t db = (d & 255) * inv / 255;
            uint32_t oa = a + da * inv / 255;
            row[xx] = (oa << 24) | ((pr + dr) << 16) | ((pg + dg) << 8) | (pb + db);
        }
    }
}

void Overlay::fillRectOpaque(float x1, float y1, float x2, float y2, uint32_t rgb) {
    fillRectAlpha(x1, y1, x2, y2, rgb, 1.0f);
}

// 多边形扫描线填充（软件，source-over 预乘 alpha）。
// 对每个扫描线 y 求与各边的交点，排序后成对填充；支持凸/凹、任意走向的多边形。
void Overlay::fillPoly(const float* xs, const float* ys, int n, uint32_t rgb, float alpha) {
    if (!m_pixels || n < 3 || alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;
    float ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < n; ++i) { ymin = std::min(ymin, ys[i]); ymax = std::max(ymax, ys[i]); }
    int y0 = (int)std::floor(ymin), y1 = (int)std::ceil(ymax);
    if (y0 < 0) y0 = 0;
    if (y1 >= m_h) y1 = m_h - 1;

    const double a = alpha;
    const double sr0 = ((rgb >> 16) & 255) * a;
    const double sg0 = ((rgb >> 8) & 255) * a;
    const double sb0 = (rgb & 255) * a;

    for (int y = y0; y <= y1; ++y) {
        const double cy = y + 0.5;
        float xsec[8];
        int nxs = 0;
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            float ya = ys[i], yb = ys[j];
            if ((ya <= cy && yb > cy) || (yb <= cy && ya > cy)) {
                double t = (cy - ya) / (yb - ya);
                if (nxs < 8) xsec[nxs++] = xs[i] + (float)(t * (xs[j] - xs[i]));
            }
        }
        // 简单插入排序
        for (int a2 = 0; a2 < nxs; ++a2)
            for (int b2 = a2 + 1; b2 < nxs; ++b2)
                if (xsec[b2] < xsec[a2]) std::swap(xsec[a2], xsec[b2]);
        uint32_t* row = m_pixels + (size_t)y * m_w;
        for (int k = 0; k + 1 < nxs; k += 2) {
            int xa = (int)std::ceil(xsec[k]), xb = (int)std::floor(xsec[k + 1]);
            if (xa < 0) xa = 0;
            if (xb >= m_w) xb = m_w - 1;
            for (int x = xa; x <= xb; ++x) {
                uint32_t d = row[x];
                double dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255, da = (d >> 24) & 255;
                double ia = 1.0 - a;
                uint32_t oa = (uint32_t)(a * 255.0 + da * ia);
                uint32_t orr = (uint32_t)(sr0 + dr * ia);
                uint32_t og = (uint32_t)(sg0 + dg * ia);
                uint32_t ob = (uint32_t)(sb0 + db * ia);
                if (oa > 255) oa = 255;
                if (orr > 255) orr = 255;
                if (og > 255) og = 255;
                if (ob > 255) ob = 255;
                row[x] = (oa << 24) | (orr << 16) | (og << 8) | ob;
            }
        }
    }
}

bool Overlay::ensureFont(int px) {
    if (!m_memDc) return false;
    if (m_font && m_fontPx == px) return true;
    if (m_font) { DeleteObject(m_font); m_font = nullptr; }
    m_textBmps.clear();   // 字号变化时旧字号位图全部作废
    m_font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    m_fontPx = px;
    return m_font != nullptr;
}

// 预渲染文本位图：每 (文本,颜色,字号) 只离屏渲染一次（含深色描边），
// 之后每帧直接整块拷贝，避免每帧上千次 TextOutW(ClearType) 成为瓶颈。
const Overlay::TextBitmap* Overlay::getTextBitmap(const std::wstring& text, uint32_t rgb, int px) {
    if (!m_scratchDc || text.empty() || !ensureFont(px)) return nullptr;
    int n = (int)text.size();

    // key = hash(px, color, text)
    uint64_t key = ((uint64_t)(uint32_t)px << 48) ^ ((uint64_t)rgb << 16);
    uint64_t h = 1469598103934665603ULL;
    for (wchar_t c : text) { h ^= (uint64_t)c; h *= 1099511628211ULL; }
    key ^= h;
    auto it = m_textBmps.find(key);
    if (it != m_textBmps.end()) return &it->second;

    SelectObject(m_scratchDc, m_font);
    SIZE sz{};
    GetTextExtentPoint32W(m_scratchDc, text.c_str(), n, &sz);
    int tw = sz.cx + 4;                                  // 左右描边边距
    int th = (sz.cy < 0 ? -sz.cy : sz.cy) + 4;           // 上下描边边距
    if (tw <= 0 || th <= 0) return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = tw;
    bi.bmiHeader.biHeight = -th;                         // 顶底
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP tmp = CreateDIBSection(m_scratchDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!tmp) return nullptr;
    HGDIOBJ old = SelectObject(m_scratchDc, tmp);
    memset(bits, 0, (size_t)tw * th * 4);
    SetBkMode(m_scratchDc, TRANSPARENT);
    SetTextColor(m_scratchDc, RGB(25, 25, 25));          // 深色描边（8 方向 1px）
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            TextOutW(m_scratchDc, 2 + dx, 2 + dy, text.c_str(), n);
        }
    SetTextColor(m_scratchDc, toColorRef(rgb));          // 主色
    TextOutW(m_scratchDc, 2, 2, text.c_str(), n);
    SelectObject(m_scratchDc, old);

    TextBitmap tb;
    tb.w = tw; tb.h = th; tb.textW = sz.cx;
    tb.px.assign((size_t)tw * th, 0);
    const uint32_t* src = (const uint32_t*)bits;
    for (int i = 0; i < tw * th; ++i) {
        uint32_t v = src[i];
        if ((v & 0x00FFFFFF) != 0) tb.px[(size_t)i] = v | 0xFF000000;  // 不透明（预乘）
    }
    DeleteObject(tmp);

    auto [nit, inserted] = m_textBmps.try_emplace(key, std::move(tb));
    // 容量上限：海量实体名 / 场景切换时位图缓存会无限增长导致内存膨胀，
    // 超出 256 项即清空重来（清掉除刚插入项外的所有，保证本帧可绘制）。
    // 名字每 2s 才重读一次，正常场景命中率高，清空重建成本极低。
    if (m_textBmps.size() > 256) {
        TextBitmap keep = std::move(nit->second);
        m_textBmps.clear();
        m_textBmps.emplace(key, std::move(keep));
        nit = m_textBmps.find(key);
    }
    return &nit->second;
}

void Overlay::drawText(float x, float y, const std::wstring& text, uint32_t rgb, int px) {
    const TextBitmap* tb = getTextBitmap(text, rgb, px);
    if (!tb || !m_pixels) return;
    int x0 = (int)x, y0 = (int)y;
    if (x0 >= m_w || y0 >= m_h) return;
    int x1 = x0 + tb->w, y1 = y0 + tb->h;
    if (x1 <= 0 || y1 <= 0) return;
    int cxs = std::max(0, x0), cys = std::max(0, y0);
    int cxe = std::min(m_w, x1), cye = std::min(m_h, y1);
    for (int yy = cys; yy < cye; ++yy) {
        const uint32_t* srcRow = tb->px.data() + (size_t)(yy - y0) * tb->w;
        uint32_t* dstRow = m_pixels + (size_t)yy * m_w;
        for (int xx = cxs; xx < cxe; ++xx) {
            uint32_t v = srcRow[xx - x0];
            if (v & 0xFF000000) dstRow[xx] = v;
        }
    }
}

float Overlay::measureText(const std::wstring& text, int px) {
    if (!m_memDc || text.empty()) return 0;
    // 名字宽度缓存（同一帧内重复测量不重算）
    auto it = m_textCache.find(text);
    if (it != m_textCache.end()) return it->second;
    if (!ensureFont(px)) return 0;
    SelectObject(m_memDc, m_font);
    SIZE s{};
    GetTextExtentPoint32W(m_memDc, text.c_str(), (int)text.size(), &s);
    float w = (float)s.cx;
    m_textCache.emplace(text, w);
    // 宽度缓存容量上限：超出 512 项清空（重算一次 GetTextExtentPoint32 成本极低）
    if (m_textCache.size() > 512) m_textCache.clear();
    return w;
}
