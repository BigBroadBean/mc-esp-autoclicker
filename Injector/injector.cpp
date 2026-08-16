// ============================================================
//  injector.cpp — MC ESP 注入器
//
//  自动查找 Minecraft (java.exe/javaw.exe) 进程，并把与
//  injector.exe 同目录下的 mc_esp.dll 注入进去。
//
//  用法:
//    injector.exe                自动查找 Minecraft 并注入
//    injector.exe -pid <PID>     注入指定 PID
//
//  说明:
//    - 全路径使用宽字符 API，路径含中文也能正常注入。
//    - 注入方式: CreateRemoteThread + LoadLibraryW。
//    - 注入后 mc_esp.dll 会在自身 DllMain 里完成模块隐藏并启动 ESP 线程。
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cwchar>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const wchar_t* kDllName = L"mc_esp.dll";
static const wchar_t* kTitleSub = L"Minecraft";

// ------------------------------------------------------------
// 控制台输出（WriteConsoleW，保证中文/宽路径不乱码）
// ------------------------------------------------------------
static void PrintW(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vswprintf(buf, 2047, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    wcscat(buf, L"\r\n");

    const size_t len = wcslen(buf);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    if (out && out != INVALID_HANDLE_VALUE &&
        WriteConsoleW(out, buf, (DWORD)len, &written, nullptr)) {
        return;
    }

    // 标准输出被重定向时 WriteConsoleW 会失败：转成 UTF-8 字节输出。
    // 优先 WriteFile 绕过 CRT 文本模式换行转换；再失败才回退 fwrite。
    char utf8[8192];
    int n8 = WideCharToMultiByte(CP_UTF8, 0, buf, (int)len, utf8, sizeof(utf8), nullptr, nullptr);
    if (n8 > 0) {
        DWORD w8 = 0;
        if (out && out != INVALID_HANDLE_VALUE &&
            WriteFile(out, utf8, (DWORD)n8, &w8, nullptr)) {
            return;
        }
        fwrite(utf8, 1, (size_t)n8, stdout);
        fflush(stdout);
    }
}

// ------------------------------------------------------------
// 进程判断
// ------------------------------------------------------------
static bool IsJavaProcessW(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    bool isJava = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid &&
                (_wcsicmp(pe.szExeFile, L"java.exe") == 0 ||
                 _wcsicmp(pe.szExeFile, L"javaw.exe") == 0)) {
                isJava = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return isJava;
}

static int ListJavaPids(DWORD* out, int max) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    int count = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"java.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"javaw.exe") == 0) {
                if (count < max) out[count++] = pe.th32ProcessID;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return count;
}

// ------------------------------------------------------------
// 自动查找 Minecraft 窗口
// ------------------------------------------------------------
struct FindCtx { const wchar_t* sub; DWORD pid; };

static BOOL CALLBACK EnumTitleProc(HWND hwnd, LPARAM lp) {
    auto* ctx = (FindCtx*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t title[256] = {};
    if (GetWindowTextW(hwnd, title, 256) == 0) return TRUE;
    if (!wcsstr(title, ctx->sub)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid && IsJavaProcessW(pid)) {
        ctx->pid = pid;
        return FALSE;   // 找到，停止枚举
    }
    return TRUE;
}

static BOOL CALLBACK EnumClassProc(HWND hwnd, LPARAM lp) {
    auto* ctx = (FindCtx*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t cls[128] = {};
    if (GetClassNameW(hwnd, cls, 128) == 0) return TRUE;
    if (!wcsstr(cls, L"GLFW") && !wcsstr(cls, L"LWJGL")) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid && IsJavaProcessW(pid)) {
        ctx->pid = pid;
        return FALSE;
    }
    return TRUE;
}

// 返回 0 表示未找到。
// 查找顺序：
//   1) 窗口标题包含 "Minecraft" 的 Java 进程
//   2) 窗口类为 GLFW/LWJGL 的 Java 进程（部分启动器会改窗口标题）
//   3) 系统里只有一个 java/javaw 进程时，直接使用它
static DWORD AutoFindMinecraftPid() {
    FindCtx ctx = {kTitleSub, 0};
    EnumWindows(EnumTitleProc, (LPARAM)&ctx);
    if (ctx.pid) return ctx.pid;

    PrintW(L"[*] 按窗口标题未找到，尝试按 GLFW/LWJGL 窗口类查找 ...");
    ctx = {nullptr, 0};
    EnumWindows(EnumClassProc, (LPARAM)&ctx);
    if (ctx.pid) return ctx.pid;

    DWORD pids[128];
    int n = ListJavaPids(pids, 128);
    if (n == 1) {
        PrintW(L"[*] 只发现一个 Java 进程，使用 PID=%lu", pids[0]);
        return pids[0];
    }
    if (n > 1) {
        PrintW(L"[!] 发现 %d 个 Java 进程，无法自动确定 Minecraft：", n);
        for (int i = 0; i < n; ++i) PrintW(L"    PID=%lu", pids[i]);
        PrintW(L"    请指定: injector.exe -pid <PID>");
        return 0;
    }

    PrintW(L"[!] 未找到 Minecraft 进程。");
    PrintW(L"    请先启动游戏，再运行 injector.exe；");
    PrintW(L"    或手动指定: injector.exe -pid <PID>");
    return 0;
}

// ------------------------------------------------------------
// 路径工具
// ------------------------------------------------------------
static bool GetSelfDir(wchar_t* out, size_t cap) {
    wchar_t exe[MAX_PATH] = {};
    DWORD got = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) {
        PrintW(L"[!] 无法获取 injector.exe 路径");
        return false;
    }
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = L'\0';
    if (wcslen(exe) >= cap) {
        PrintW(L"[!] injector.exe 路径过长");
        return false;
    }
    wcscpy(out, exe);
    return true;
}

static void JoinPath(wchar_t* out, size_t cap, const wchar_t* dir, const wchar_t* name) {
    wcscpy(out, dir);
    wcscat(out, name);
}

static ULONGLONG FileSizeW(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return 0;
    return ((ULONGLONG)data.nFileSizeHigh << 32) | (ULONGLONG)data.nFileSizeLow;
}

// ------------------------------------------------------------
// 注入: CreateRemoteThread + LoadLibraryW
// ------------------------------------------------------------
static bool InjectDllW(DWORD pid, const wchar_t* dllPath) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        PrintW(L"[!] OpenProcess(%lu) 失败，错误码 %lu", pid, GetLastError());
        PrintW(L"    请确认游戏正在运行；如仍失败，尝试以管理员身份运行 injector.exe。");
        return false;
    }

    // 位数检查：本注入器按 64 位编译，只能注入 64 位 Java
#ifdef _WIN64
    BOOL targetWow64 = FALSE;
    if (IsWow64Process(proc, &targetWow64) && targetWow64) {
        PrintW(L"[!] 目标 Minecraft 是 32 位进程，而 injector/mc_esp.dll 是 64 位。");
        PrintW(L"    请使用 64 位 Java 启动 Minecraft。");
        CloseHandle(proc);
        return false;
    }
#else
    PrintW(L"[!] 当前 injector 是 32 位版本，mc_esp.dll 为 64 位，无法注入。");
    PrintW(L"    请用 64 位 g++ 重新编译 injector（不要加 -m32）。");
    CloseHandle(proc);
    return false;
#endif

    const SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        PrintW(L"[!] VirtualAllocEx 失败，错误码 %lu", GetLastError());
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remote, dllPath, bytes, nullptr)) {
        PrintW(L"[!] WriteProcessMemory 失败，错误码 %lu", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibraryW = GetProcAddress(k32, "LoadLibraryW");
    if (!loadLibraryW) {
        PrintW(L"[!] 找不到 kernel32!LoadLibraryW");
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0,
                                       (LPTHREAD_START_ROUTINE)loadLibraryW,
                                       remote, 0, nullptr);
    if (!thread) {
        PrintW(L"[!] CreateRemoteThread 失败，错误码 %lu", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    PrintW(L"[*] 远程线程已创建，等待 LoadLibraryW 返回 ...");
    DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD exitCode = 0;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeThread(thread, &exitCode);
    }
    CloseHandle(thread);

    if (wait != WAIT_OBJECT_0) {
        PrintW(L"[!] 等待远程线程超时，注入可能未完成。");
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    // LoadLibraryW 的 64 位返回值经线程退出码截断后可能为 0，不能仅凭它判定失败；
    // 这里只作提示，真正验证靠下方 esp_log.txt 是否增长。
    if (exitCode == 0) {
        PrintW(L"[!] 远程线程返回 0（64 位返回值截断或加载失败），继续检查日志确认 ...");
    }

    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return true;
}

// ------------------------------------------------------------
// 注入结果验证：mc_esp.dll 的 DllMain 会写 esp_log.txt / esp_log_new.txt
// ------------------------------------------------------------
static void VerifyByLog(const wchar_t* dir) {
    wchar_t log1[MAX_PATH];
    wchar_t log2[MAX_PATH];
    JoinPath(log1, MAX_PATH, dir, L"esp_log.txt");
    JoinPath(log2, MAX_PATH, dir, L"esp_log_new.txt");

    const ULONGLONG old1 = FileSizeW(log1);
    const ULONGLONG old2 = FileSizeW(log2);

    Sleep(1200);   // 给 DllMain 写日志留时间

    const ULONGLONG new1 = FileSizeW(log1);
    const ULONGLONG new2 = FileSizeW(log2);

    if (new1 > old1 || new2 > old2) {
        PrintW(L"[+] 日志已更新，DLL 已成功加载并执行 DllMain。");
    } else {
        PrintW(L"[!] 未观察到日志增长。若游戏内没有 ESP 效果，请检查：");
        PrintW(L"    1) mc_esp.dll 是否与 injector.exe 同目录");
        PrintW(L"    2) 游戏 Java 是否为 64 位");
        PrintW(L"    3) 是否以管理员身份运行 injector.exe");
    }
}

// ------------------------------------------------------------
// 主流程
// ------------------------------------------------------------
int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    DWORD pid = 0;
    bool findOnly = false;
    if (argc == 2 && _stricmp(argv[1], "-find") == 0) {
        findOnly = true;                    // 仅查找，不注入
    } else if (argc == 3 && _stricmp(argv[1], "-pid") == 0) {
        pid = (DWORD)strtoul(argv[2], nullptr, 10);
    } else if (argc != 1) {
        PrintW(L"用法:");
        PrintW(L"  injector.exe                自动查找 Minecraft 并注入");
        PrintW(L"  injector.exe -find          仅查找 Minecraft 进程，不注入");
        PrintW(L"  injector.exe -pid <PID>     注入指定 PID");
        return 1;
    }

    if (pid == 0) {
        PrintW(L"[*] 正在自动查找 Minecraft 进程 ...");
        pid = AutoFindMinecraftPid();
        if (pid == 0) return 1;
        PrintW(L"[*] 找到 Minecraft 进程，PID=%lu", pid);
    } else {
        if (!IsJavaProcessW(pid)) {
            PrintW(L"[!] 警告：PID %lu 不是 java/javaw 进程，仍将尝试注入。", pid);
        }
    }

    if (findOnly) {
        PrintW(L"[+] 查找完成，未执行注入。");
        return 0;
    }

    wchar_t dir[MAX_PATH];
    if (!GetSelfDir(dir, MAX_PATH)) return 1;

    wchar_t dllPath[MAX_PATH];
    JoinPath(dllPath, MAX_PATH, dir, kDllName);
    PrintW(L"[*] DLL 路径: %ls", dllPath);

    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        PrintW(L"[!] 找不到 %ls", dllPath);
        PrintW(L"    请把 mc_esp.dll 与 injector.exe 放在同一目录。");
        return 1;
    }

    if (!InjectDllW(pid, dllPath)) return 1;

    PrintW(L"[+] 注入流程完成，正在验证 DLL 初始化日志 ...");
    VerifyByLog(dir);

    PrintW(L"[*] 提示：游戏内按 INSERT 呼出连点器菜单（ESP 与连点设置均在菜单内）。");
    return 0;
}

