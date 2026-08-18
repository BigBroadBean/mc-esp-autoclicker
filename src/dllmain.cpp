// ============================================================
//  dllmain.cpp — DLL 入口
// ============================================================
#include <windows.h>
#include <intrin.h>   // __readgsqword
#include "common.h"
#include "glrender.h" // gl_remove_hook（卸载时兜底恢复 SwapBuffers 钩子）

// 由 esp.cpp 实现
extern "C" __declspec(dllexport) DWORD WINAPI esp_thread_main(LPVOID);
extern "C" __declspec(dllexport) void esp_stop();

static HANDLE g_thread = nullptr;

// 伪装线程起点：CreateThread(起点=ntdll!RtlUserThreadStart, 参数=真实函数)。
// 已本地验证：这样创建的线程，NtQueryInformationThread 读到的 StartAddress
// 是 RtlUserThreadStart（合法 ntdll 模块地址），且真实函数正常执行（忽略参数）。
HANDLE spawn_hidden_thread(LPTHREAD_START_ROUTINE fn) {
    static LPTHREAD_START_ROUTINE rtlStart = nullptr;
    if (!rtlStart)
        rtlStart = (LPTHREAD_START_ROUTINE)GetProcAddress(
            GetModuleHandleA("ntdll.dll"), "RtlUserThreadStart");
    return CreateThread(nullptr, 0, rtlStart, (LPVOID)fn, 0, nullptr);
}

// 手动映射注入时由注入器传入的信息（本仓库当前使用 loader.cpp 的 LoadLibrary 路径）
struct EspManualInfo {
    wchar_t dir[MAX_PATH * 2];   // DLL 目录（esp.ini / esp_log.txt 所在）
    uintptr_t base;              // 手动映射镜像基址（用于读自身 PE 头/注册 .pdata）
};

// 注入器把目录/基址写入此导出全局，供 esp_manual_entry 读取。
// 用导出全局而非线程参数：注入器远程线程起点已伪装为 ntdll!RtlUserThreadStart，
// 该伪装下真实函数只会收到 NULL 参数，无法直接传信息指针。
extern "C" EspManualInfo g_espInfo;                    // 声明（C 链接）
__declspec(dllexport) EspManualInfo g_espInfo = {};    // 定义（dllexport 保证导出）

// 手动映射的模块不在 PEB 链表，但映射镜像开头仍带完整 PE 头（MZ+DOS/NT+节表）。
// 初始化完成后 DLL 把 PE 头覆盖为哨兵（ESP_MAGIC），使该区域不再像“隐藏模块”，
// 卸载时注入器靠哨兵（而非 MZ）定位校验。ESP_MAGIC='M','S','P','X' 低位序，
// 刻意避开 MZ 0x5A4D。
#define ESP_MAGIC 0x5850534Du

// 手动映射模块的 .pdata（异常展开表）指针，esp_manual_entry 里注册、dll_unload 里注销。
static uintptr_t g_pdata = 0;

// ---- 反注入检测规避：从 PEB 模块链表摘除自身 ----
// 游戏可能通过枚举已加载模块（Toolhelp/GetModuleHandle 等）发现注入的 DLL 而自动退出。
// 在 DllMain ATTACH 内把本 DLL 的 LDR 表项从三条模块链表中摘除，
// 之后模块枚举与 GetModuleHandle 都看不到本 DLL，而 DLL 仍正常驻留执行。
// x64 布局：TEB->PEB 位于 gs:[0x60]；PEB->Ldr 位于 +0x18；
// Ldr->InMemoryOrderModuleList 位于 +0x20；表项 InMemoryOrderLinks 位于 +0x10，DllBase 位于 +0x30。
static void hide_module_from_peb(HMODULE self, bool& found) {
    found = false;
    BYTE* peb = (BYTE*)__readgsqword(0x60);
    if (!peb) return;
    BYTE* ldr = *(BYTE**)(peb + 0x18);
    if (!ldr) return;
    LIST_ENTRY* memHead = (LIST_ENTRY*)(ldr + 0x20);
    for (LIST_ENTRY* e = memHead->Flink; e != memHead; e = e->Flink) {
        BYTE* entry = (BYTE*)e - 0x10;                 // InMemoryOrderLinks 偏移 0x10
        if (*(void**)(entry + 0x30) == (void*)self) {  // DllBase 偏移 0x30
            LIST_ENTRY* il = (LIST_ENTRY*)(entry + 0x00);  // InLoadOrderLinks
            LIST_ENTRY* im = (LIST_ENTRY*)(entry + 0x10);  // InMemoryOrderLinks
            LIST_ENTRY* ii = (LIST_ENTRY*)(entry + 0x20);  // InInitializationOrderLinks
            il->Blink->Flink = il->Flink; il->Flink->Blink = il->Blink;
            im->Blink->Flink = im->Flink; im->Flink->Blink = im->Blink;
            ii->Blink->Flink = ii->Flink; ii->Flink->Blink = ii->Blink;
            il->Flink = il->Blink = il;
            im->Flink = im->Blink = im;
            ii->Flink = ii->Blink = ii;
            found = true;
            break;
        }
    }
}

// 内存痕迹清理：手动映射的模块不在 PEB 链表，但映射镜像开头仍带完整 PE 头
// （MZ + DOS/NT + 节表）。站在内存区域扫描器角度，“非模块地址上的 RWX 内存 + PE 头”
// 正是“隐藏的非预期模块”的典型特征。初始化全部完成后（构造 __main、.pdata 注册、
// 线程已启动），把 PE 头覆盖为普通数据并在开头写哨兵，使该区域不再像“隐藏模块”；
// 注入器卸载时靠哨兵（而非 MZ）定位与校验。
// 注意：必须在所有仍需读自身 PE 头的步骤结束后调用；本模块运行期不再读自身头。
static void scrub_pe_header(uintptr_t base) {
    if (!base) return;
    auto* dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;   // 非 PE，跳过
    DWORD headerSize = 0x1000;                         // 兜底
    auto* nt = (PIMAGE_NT_HEADERS64)(base + dos->e_lfanew);
    if (nt->Signature == IMAGE_NT_SIGNATURE)
        headerSize = nt->OptionalHeader.SizeOfHeaders;
    if (headerSize == 0 || headerSize > 0x1000) headerSize = 0x1000;
    // 节首地址恒 >= SizeOfHeaders，覆盖头区绝不会碰到 .text。
    volatile DWORD* p = (volatile DWORD*)base;
    p[0] = ESP_MAGIC;                                  // 覆盖 MZ 并写入哨兵
    for (size_t i = 1; i < headerSize / 4; ++i) p[i] = 0;   // 其余头区清零（普通匿名内存形态）
    esp_log("[dll] 已清理 PE 头痕迹 base=%p size=0x%X", (void*)base, headerSize);
}

// 供注入器热卸载调用（在 DllMain 之外执行，避免加载锁内等待导致死锁）：
// 先通知渲染线程退出并等待其彻底结束，然后【兜底】恢复 SwapBuffers 钩子，
// 注销 .pdata，之后注入器再释放映射内存即安全。
// 兜底 gl_remove_hook 是必要的：若渲染线程未及时退出（如卡在窗口交互，
// 3s 等待超时），SwapBuffers 仍指向本模块内钩子，此时释放内存会让游戏
// 下一次 SwapBuffers 跳进已释放内存而崩溃（崩溃地址在 0x2AF1xxxx 模块内）。
extern "C" __declspec(dllexport) void dll_unload() {
    esp_log("[dll] unload begin");
    // 先停渲染线程
    HANDLE t = g_thread;
    if (t) {
        esp_stop();                          // 让渲染循环退出
        DWORD wr = WaitForSingleObject(t, 3000);   // 等待 esp_thread_main 彻底结束
        esp_log("[dll] unload wait thread wr=%lu", wr);
        CloseHandle(t);
        g_thread = nullptr;
    }
    // 还原 SwapBuffers 钩子（幂等），阻止游戏渲染线程后续再进入本模块。
    gl_remove_hook();
    esp_log("[dll] unload hook removed");
    // 等待游戏渲染线程离开仍在途的 SwapHook（含 g_realSwap 调用）。
    gl_wait_hook_idle(5000);
    esp_log("[dll] unload done (保留 .pdata 与映射内存，避免卸载崩溃)");
}

// 手动映射入口：由注入器在映射完成后远程调用。
// 作用：传入 DLL 目录（手动映射没有文件路径，GetModuleFileNameW 取不到），
// 然后启动 ESP 线程。手动映射时模块不在 PEB 链表，无需 hide_module_from_peb。
// 关键：手动映射不经过 _DllMainCRTStartup，必须手动运行 C++ 静态构造，
// 否则 std::mutex / std::wstring / std::unordered_map 等全局对象未初始化，一用就崩。
// 注意：本 MinGW 构建的全局构造在 .ctors(__CTOR_LIST__) 表里，由 __main()->
// __do_global_ctors() 执行；__xc_a/__xc_z 在本构建里并不指向初始化表（曾误用
// _initterm(__xc_a,__xc_z)，导致 g_overlay 的 unordered_map 未构造、桶数为 0、
// 取桶下标时整型除零崩溃）。因此这里调用 __main() 运行真正的构造。
extern "C" void __main();

extern "C" __declspec(dllexport) DWORD WINAPI esp_manual_entry(LPVOID param) {
    (void)param;
    __main();   // 运行全局构造（必须先于任何全局对象使用）
    // 信息从注入器预写入的导出全局读取：注入器远程线程起点已伪装为
    // ntdll!RtlUserThreadStart，该伪装下本函数只能收到 NULL 参数。
    const EspManualInfo& info = g_espInfo;
    if (info.dir[0]) dll_set_directory(info.dir);
    // 注册 .pdata（异常展开信息）。手动映射的模块不在已加载模块列表里，
    // 异常派发/栈回溯（含 JVM 在 JNI 边界做的 SEH 处理）找不到展开信息会直接崩溃。
    // 必须在目标进程内调用 RtlAddFunctionTable（本代码运行在目标进程）。
    if (info.base) {
        auto* dos = (PIMAGE_DOS_HEADER)info.base;
        auto* nt = (PIMAGE_NT_HEADERS64)((BYTE*)info.base + dos->e_lfanew);
        DWORD excRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress;
        DWORD excSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size;
        if (excRva && excSize) {
            PRUNTIME_FUNCTION rt = (PRUNTIME_FUNCTION)(info.base + excRva);
            DWORD count = excSize / sizeof(RUNTIME_FUNCTION);
            if (RtlAddFunctionTable(rt, count, info.base)) {
                g_pdata = (uintptr_t)rt;
                esp_log("[dll] 已注册异常展开表 %u 项", count);
            } else {
                esp_log("[dll] 注册异常展开表失败");
            }
        }
    }
    // 清理镜像 PE 头内存痕迹（本模块运行期不再读自身头）。
    // 【必须在 spawn 线程之前同步执行】：若在 spawn 之后再清理，清理与刚创建的
    // ESP 线程并发，存在罕见竞态（注入普通进程时约 1/7 概率整进程崩溃）。而先清理、
    // 后 spawn 时，此刻本模块除注入线程外无其他活动线程，清理绝对安全；
    // 后续 ESP 线程各阶段（config_load/找窗/装钩/渲染）均不读自身 PE 头。
    scrub_pe_header(info.base);
    if (!g_thread) {
        esp_log("[dll] esp_manual_entry 启动 ESP 线程");
        g_thread = spawn_hidden_thread(esp_thread_main);
    }
    return 0;
}

// 单文件 EXE 加载器会在 CreateRemoteThread 之前创建命名内存映射，
// 把 exe 所在目录告诉即将进入 DllMain 的本 DLL。这样 esp.ini / esp_log.txt
// 仍在用户双击的 exe 旁边，而不是解包后临时 DLL 的目录。
// 名称只依赖当前进程 PID；加载器与 DLL 两端固定 4096 字节宽字符容量。
static void apply_injected_directory() {
    wchar_t name[96];
    swprintf(name, 96, L"Local\\mc_esp_dir_%lu", GetCurrentProcessId());
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!h) {
        esp_log("[dll] 目录映射不存在（回退 DLL 自身目录）");
        return;
    }
    void* view = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    if (view) {
        wchar_t buf[MAX_PATH * 2] = {0};
        memcpy(buf, view, sizeof(buf) - sizeof(wchar_t));
        if (buf[0]) {
            dll_set_directory(buf);
            esp_log("[dll] 使用加载器目录映射: %ls", buf);
        }
        UnmapViewOfFile(view);
    }
    CloseHandle(h);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hModule);
            // 手动映射时 GetModuleFileNameW 会失败（无文件、无 PEB 项），
            // 此时不启动线程，改由注入器调用 esp_manual_entry 启动并传目录。
            bool manualMap = false;
            {
                wchar_t buf[MAX_PATH * 2] = {0};
                if (GetModuleFileNameW(hModule, buf, MAX_PATH * 2)) {
                    wchar_t* s = wcsrchr(buf, L'\\');
                    if (s) *(s + 1) = L'\0';
                    dll_set_directory(buf);
                } else {
                    manualMap = true;
                }
            }
            // 单文件 EXE：最后再读加载器传入的 exe 目录，覆盖临时 DLL 目录。
            // 否则 GetModuleFileNameW 会先写入 %TEMP% 的 DLL 路径。
            apply_injected_directory();
            if (manualMap) {
                // 手动映射：无模块项、无文件路径。线程由 esp_manual_entry 启动。
                esp_log("[dll] mc_esp 手动映射加载（PEB 无模块项）");
                break;
            }
            // 反注入检测规避：从 PEB 模块链表摘除自身（LoadLibrary 注入路径）
            bool hidden = false;
            hide_module_from_peb(hModule, hidden);
            esp_log("[dll] mc_esp 已加载（模块隐藏:%s），启动 ESP 线程",
                    hidden ? "成功" : "失败");
            // 新线程运行，避免在 DllMain 加载锁内调用 JNI 造成死锁
            g_thread = spawn_hidden_thread(esp_thread_main);
            break;
        }
        case DLL_PROCESS_DETACH:
            // 进程退出，或经 dll_unload 干净停止后 FreeLibrary 到达这里。
            // dll_unload 已停掉线程时 g_thread 为 null；否则（进程退出）做快速停止。
            if (g_thread) {
                esp_stop();
                WaitForSingleObject(g_thread, 1500);
                CloseHandle(g_thread);
                g_thread = nullptr;
            }
            esp_log("[dll] mc_esp 卸载");
            break;
    }
    return TRUE;
}
