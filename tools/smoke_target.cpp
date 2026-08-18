// smoke_target.cpp — 单文件 mc_esp.exe 注入目标（无 JVM 冒烟测试）。
// 创建 GLFW 窗口并运行消息循环，等待 mc_esp.exe -pid <pid> 注入。
#include <windows.h>
#include <cstdio>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int main() {
    const wchar_t* cls = L"GLFW30";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;
    HWND hwnd = CreateWindowExW(0, cls, L"Minecraft smoke target", WS_OVERLAPPEDWINDOW,
                                100, 100, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 2;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    printf("pid=%lu\n", (unsigned long)GetCurrentProcessId());
    fflush(stdout);

    for (;;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
            if (m.message == WM_QUIT) return 0;
        }
        Sleep(10);
    }
}
