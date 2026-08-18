#pragma once
// ============================================================
//  overlay.h — 顶层透明覆盖层 + GDI 绘制 + 真实窗口鼠标消息
// ============================================================
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

// 返回 true 表示该窗口消息已被菜单消费，不再交给 DefWindowProc。
using OverlayInputFn = bool (*)(UINT msg, WPARAM wParam, LPARAM lParam, void* user);

class Overlay {
public:
    // 预渲染文本位图（预乘 alpha，每 (文本,颜色,字号) 渲染一次后整帧复用）
    struct TextBitmap {
        std::vector<uint32_t> px;   // w*h 预乘像素（alpha=0 表示透明）
        int w = 0, h = 0;
        int textW = 0;              // 文本自身宽度（不含描边边距）
    };
public:
    Overlay() = default;
    ~Overlay() { destroy(); }

    // 创建覆盖层窗口（顶层 owned popup，压住游戏客户区；独立窗口 + 截图排除，
    // 游戏 mod 截图/录屏上传服务器时捕获不到 ESP 内容）
    bool create(HWND gameHwnd);
    void destroy();

    // 每帧调用：将覆盖层移动到游戏窗口客户区，返回 false 表示不可见
    bool position(HWND gameHwnd);

    // 安装真实鼠标消息回调。菜单打开时由 WndProc 直接调用；
    // 关闭（鼠标穿透）时不会有消息到达。
    void set_input_handler(OverlayInputFn fn, void* user) { m_inputFn = fn; m_inputUser = user; }

    // 返回像素缓冲（已清空为全透明），大小 w*h
    uint32_t* lock(int& w, int& h);

    // 返回当前像素缓冲但不做清屏（静态菜单复用时避免整屏 memset）
    uint32_t* lockNoClear(int& w, int& h);

    // 菜单打开时把覆盖层从鼠标穿透切换为可点击（不激活、不抢游戏焦点）
    void set_clickable(bool on);

    // 把缓冲呈现到窗口（会先做 alpha 后处理）
    void present();

    HWND hwnd() const { return m_hwnd; }

    // GDI 绘制原语（坐标在窗口客户区内，即游戏客户区坐标）
    void drawLine(float x1, float y1, float x2, float y2, uint32_t rgb, int width);
    void drawRect(float x1, float y1, float x2, float y2, uint32_t rgb, int width);
    void fillRect(float x1, float y1, float x2, float y2, uint32_t rgb);
    void fillRectAlpha(float x1, float y1, float x2, float y2, uint32_t rgb, float alpha);
    void fillRectOpaque(float x1, float y1, float x2, float y2, uint32_t rgb);
    // 填充凸/凹多边形（n 个顶点），50% 半透明（source-over 预乘 alpha）。
    // 用于 3D 落点方块 6 个面的平面渲染。
    void fillPoly(const float* xs, const float* ys, int n, uint32_t rgb, float alpha);
    // 抗锯齿实心圆（状态指示灯用）。
    void fillCircle(float cx, float cy, float r, uint32_t rgb);
    void drawText(float x, float y, const std::wstring& text, uint32_t rgb, int px);
    float measureText(const std::wstring& text, int px);

    // ---- 离屏渲染（菜单缓存用） ----
    // 临时把绘制目标切到外部像素缓冲。结束前必须调用 end_offscreen。
    // 注意：drawRect/GDI 仍指向主 DIB，离屏目标请使用 drawLine/fillRect*/drawText。
    bool begin_offscreen(int w, int h, uint32_t* pixels);
    void end_offscreen();

    bool visible = false;

private:
    static COLORREF toColorRef(uint32_t rgb) {
        return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    }
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    HPEN getPen(uint32_t rgb, int width);
    bool ensureFont(int px);
    const TextBitmap* getTextBitmap(const std::wstring& text, uint32_t rgb, int px);

    HWND     m_hwnd  = nullptr;
    HDC      m_memDc = nullptr;
    HDC      m_scratchDc = nullptr;   // 文本位图离线渲染用
    HBITMAP  m_dib   = nullptr;
    uint32_t* m_pixels = nullptr;
    int      m_w = 0, m_h = 0;
    uint32_t* m_offscreenSavedPixels = nullptr;
    int      m_offscreenSavedW = 0, m_offscreenSavedH = 0;
    int      m_lastX = 0, m_lastY = 0;   // 覆盖层窗口上次的屏幕位置（顶层窗口用屏幕坐标）
    HGDIOBJ  m_oldBmp = nullptr;
    HFONT    m_font  = nullptr;
    int      m_fontPx = 0;
    std::unordered_map<uint64_t, HPEN>   m_pens;      // 按 (颜色,线宽) 缓存画笔，跨帧复用（destroy 时统一释放）
    std::unordered_map<std::wstring, float> m_textCache; // 文本宽度缓存，跨帧复用
    std::unordered_map<uint64_t, TextBitmap> m_textBmps; // 预渲染文本位图缓存（destroy 时统一释放）

    OverlayInputFn m_inputFn = nullptr;   // 菜单鼠标消息回调
    void*          m_inputUser = nullptr;
};
