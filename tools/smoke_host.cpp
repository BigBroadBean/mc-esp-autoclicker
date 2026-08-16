// smoke_host.cpp — 无 JVM 的注入 DLL 冒烟测试宿主
// 创建类名为 GLFW30 的窗口后 LoadLibrary(mc_esp.dll)，验证 DllMain/ESP线程/
// 连点线程/覆盖层/日志能正常启动和停止。
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
    HWND hwnd = CreateWindowExW(0, cls, L"Minecraft smoke", WS_OVERLAPPEDWINDOW,
                                100, 100, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 2;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    wchar_t dll[MAX_PATH] = L"mc_esp.dll";
    HMODULE mod = LoadLibraryW(dll);
    printf("LoadLibrary=%p err=%lu\n", mod, GetLastError());
    if (!mod) return 3;

    // 等待 ESP 渲染线程进入宿主循环，再打开菜单验证鼠标捕获切换
    Sleep(300);
    keybd_event(VK_INSERT, 0, 0, 0);
    keybd_event(VK_INSERT, 0, KEYEVENTF_KEYUP, 0);
    Sleep(400);
    keybd_event(VK_INSERT, 0, 0, 0);
    keybd_event(VK_INSERT, 0, KEYEVENTF_KEYUP, 0);
    Sleep(200);

    for (int i = 0; i < 50; ++i) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(100);
    }

    typedef void (WINAPI *UnloadFn)();
    UnloadFn unload = (UnloadFn)GetProcAddress(mod, "dll_unload");
    if (unload) unload();
    printf("smoke done\n");
    return 0;
}
