// smoke_mouse.cpp — 真实鼠标窗口消息冒烟测试
// 验证 Overlay::WndProc 能把 WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_LBUTTONUP /
// WM_MOUSEWHEEL 投递给输入回调，而不是靠 GetAsyncKeyState 模拟鼠标。
#include "../src/overlay.h"
#include "../src/common.h"

#include <windows.h>
#include <cstdio>

static int g_move = 0, g_down = 0, g_up = 0, g_wheel = 0, g_leave = 0;

static bool InputFn(UINT msg, WPARAM, LPARAM, void*) {
    switch (msg) {
    case WM_MOUSEMOVE:      ++g_move;   return true;
    case WM_LBUTTONDOWN:    ++g_down;   return true;
    case WM_LBUTTONUP:      ++g_up;     return true;
    case WM_MOUSEWHEEL:     ++g_wheel;  return true;
    case WM_MOUSELEAVE:     ++g_leave;  return true;
    default: return false;
    }
}

static LRESULT CALLBACK GameWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int main() {
    const wchar_t* cls = L"SmokeMouseGame";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = GameWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    HWND game = CreateWindowExW(0, cls, L"smoke", WS_OVERLAPPEDWINDOW,
                                40, 40, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    if (!game) return 1;

    Overlay ov;
    if (!ov.create(game)) return 2;
    if (!ov.position(game)) return 3;
    ov.set_input_handler(InputFn, nullptr);
    ov.set_clickable(true);   // 菜单打开状态：移除 WS_EX_TRANSPARENT

    SendMessageW(ov.hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(100, 100));
    SendMessageW(ov.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
    SendMessageW(ov.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(100, 100));
    SendMessageW(ov.hwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, 120), MAKELPARAM(100, 100));

    bool ok = (g_move > 0 && g_down == 1 && g_up == 1 && g_wheel == 1);
    printf("mouse messages: move=%d down=%d up=%d wheel=%d leave=%d => %s\n",
           g_move, g_down, g_up, g_wheel, g_leave, ok ? "PASS" : "FAIL");

    ov.set_clickable(false);
    ov.destroy();
    DestroyWindow(game);
    return ok ? 0 : 4;
}
