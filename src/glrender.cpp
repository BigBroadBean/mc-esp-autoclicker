// ============================================================
//  glrender.cpp — 游戏内数据采集钩子
//  钩住 gdi32!SwapBuffers（每帧在已附加 JVM 的渲染线程调用），
//  在钩子内采集实体数据，避免新建线程 + AttachCurrentThread。
// ============================================================
#include "common.h"
#include "glrender.h"

#include <atomic>

typedef BOOL(WINAPI* SwapBuffers_t)(HDC);

// 在游戏渲染线程执行的采集回调（esp.cpp 实现）
extern void esp_on_swap();

static SwapBuffers_t g_realSwap = nullptr;
static BYTE*         g_swapStub = nullptr;
static BYTE          g_savedStub[12] = {};
static bool          g_hooked = false;

// 在途 SwapHook 计数：卸载时用于确认游戏渲染线程已彻底离开本模块。
// 计数覆盖 SwapHook 全程（含 g_realSwap 调用），归零才表示游戏线程已
// 完全不在本模块内执行，此时才能安全释放映射内存。
static std::atomic<int> g_inSwap{0};

// 钩子入口：先采集 ESP 数据，再调用原始 SwapBuffers
static BOOL WINAPI SwapHook(HDC hdc) {
    g_inSwap.fetch_add(1, std::memory_order_acq_rel);
    esp_on_swap();
    BOOL ok = g_realSwap ? g_realSwap(hdc) : TRUE;
    g_inSwap.fetch_sub(1, std::memory_order_acq_rel);
    return ok;
}

// 等待游戏渲染线程彻底离开本模块 SwapHook（计数归零）。
void gl_wait_hook_idle(DWORD timeoutMs) {
    DWORD t0 = GetTickCount();
    while (g_inSwap.load(std::memory_order_acquire) != 0) {
        if (GetTickCount() - t0 >= timeoutMs) break;
        Sleep(1);
    }
}

// x64 12 字节绝对跳转：mov rax, imm64; jmp rax
static void WriteAbsJump(BYTE* dst, void* target) {
    DWORD old = 0;
    VirtualProtect(dst, 12, PAGE_EXECUTE_READWRITE, &old);
    dst[0] = 0x48; dst[1] = 0xB8;                 // mov rax, imm64
    memcpy(dst + 2, &target, 8);
    dst[10] = 0xFF; dst[11] = 0xE0;               // jmp rax
    VirtualProtect(dst, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, 12);
}

bool gl_install_hook() {
    if (g_hooked) return true;

    BYTE* stub = (BYTE*)GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "SwapBuffers");
    if (!stub) { esp_log("[hook] 找不到 gdi32!SwapBuffers"); return false; }

    // 期望前导：6 字节 FF 25 rel32 转发桩 + 6 字节 CC 填充
    if (stub[0] != 0xFF || stub[1] != 0x25) {
        esp_log("[hook] SwapBuffers 前导非预期 %02X %02X", stub[0], stub[1]);
        return false;
    }
    for (int i = 6; i < 12; ++i) {
        if (stub[i] != 0xCC) { esp_log("[hook] SwapBuffers 填充非 CC (i=%d %02X)", i, stub[i]); return false; }
    }

    // 从转发桩读出真实函数指针（无需 trampoline）
    int32_t rel = *(int32_t*)(stub + 2);
    g_realSwap = *(SwapBuffers_t*)(stub + 6 + rel);
    if (!g_realSwap) { esp_log("[hook] 读取真实 SwapBuffers 失败"); return false; }

    g_swapStub = stub;
    memcpy(g_savedStub, stub, 12);
    WriteAbsJump(stub, (void*)&SwapHook);
    g_hooked = true;
    esp_log("[hook] SwapBuffers 钩子已安装 stub=%p real=%p", (void*)stub, (void*)g_realSwap);
    return true;
}

void gl_remove_hook() {
    if (!g_hooked) return;
    if (g_swapStub) {
        DWORD old = 0;
        VirtualProtect(g_swapStub, 12, PAGE_EXECUTE_READWRITE, &old);
        memcpy(g_swapStub, g_savedStub, 12);
        VirtualProtect(g_swapStub, 12, old, &old);
        FlushInstructionCache(GetCurrentProcess(), g_swapStub, 12);
        esp_log("[hook] SwapBuffers 钩子已卸载");
    }
    g_hooked = false;
    g_swapStub = nullptr;
    g_realSwap = nullptr;
}
