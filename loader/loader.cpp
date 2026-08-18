// ============================================================
//  loader.cpp — mc_esp.exe 单文件自注入加载器
//
//  mc_esp.dll 不再作为单独文件分发：构建时被转换为字节数组
//  (build_tmp/payload.h) 编译进本 EXE。运行时：
//    1. 自动查找 Minecraft (java.exe/javaw.exe) 进程；
//    2. 把内嵌 DLL 解包到 %TEMP%；
//    3. 通过命名内存映射把【数据目录】告诉 DLL（默认 %APPDATA%\mc_esp）；
//    4. CreateRemoteThread + LoadLibraryW 注入；
//    5. 注入完成后删除临时 DLL。
//
//  用法:
//    mc_esp.exe                自动查找 Minecraft 并注入
//    mc_esp.exe -pid <PID>     注入指定 PID
//    mc_esp.exe -find          仅查找进程，不注入
//    mc_esp.exe -dir <PATH>    自定义 esp.ini / esp_log.txt 目录
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "payload.h"   // g_payloadDll / g_payloadDllSize（build.bat 生成）

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
    if (pid && IsJavaProcessW(pid)) { ctx->pid = pid; return FALSE; }
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
    if (pid && IsJavaProcessW(pid)) { ctx->pid = pid; return FALSE; }
    return TRUE;
}

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
        PrintW(L"    请指定: mc_esp.exe -pid <PID>");
        return 0;
    }
    PrintW(L"[!] 未找到 Minecraft 进程。");
    PrintW(L"    请先启动游戏，再运行 mc_esp.exe；");
    PrintW(L"    或手动指定: mc_esp.exe -pid <PID>");
    return 0;
}

// ------------------------------------------------------------
// 路径工具
// ------------------------------------------------------------
static void AppendSep(wchar_t* p) {
    size_t n = wcslen(p);
    if (n == 0 || p[n - 1] != L'\\') wcscat(p, L"\\");
}

// 数据目录优先级与 DLL 内的 data_directory() 保持一致：
// MC_ESP_DATA_DIR > %APPDATA%\mc_esp > C:\mc_esp（异常兜底）。
static bool GetDataDirW(wchar_t* out, size_t cap) {
    wchar_t env[2048] = {0};
    DWORD n = GetEnvironmentVariableW(L"MC_ESP_DATA_DIR", env, 2048);
    if (n > 0 && n < 2048) {
        if (wcslen(env) + 1 >= cap) return false;
        wcscpy(out, env);
        AppendSep(out);
        CreateDirectoryW(out, nullptr);
        return true;
    }
    n = GetEnvironmentVariableW(L"APPDATA", env, 2048);
    if (n > 0 && n < 2048 && wcslen(env) + 8 < cap) {
        wcscpy(out, env);
        AppendSep(out);
        wcscat(out, L"mc_esp");
    } else {
        wcscpy(out, L"C:\\mc_esp");
    }
    AppendSep(out);
    CreateDirectoryW(out, nullptr);
    return wcslen(out) < cap;
}

static ULONGLONG FileSizeW(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return 0;
    return ((ULONGLONG)data.nFileSizeHigh << 32) | (ULONGLONG)data.nFileSizeLow;
}

// ------------------------------------------------------------
// 清理之前运行遗留的临时 payload（游戏已退出时通常都能删除成功）
// ------------------------------------------------------------
static void CleanupStalePayloads() {
    wchar_t pattern[MAX_PATH] = {};
    DWORD tn = GetTempPathW(MAX_PATH, pattern);
    if (tn == 0 || tn >= MAX_PATH - 16) return;
    wcscat(pattern, L"mc_esp_payload_*.dll");

    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            wchar_t full[MAX_PATH] = {};
            DWORD tn2 = GetTempPathW(MAX_PATH, full);
            if (tn2 && tn2 < MAX_PATH - 32) {
                wcscat(full, fd.cFileName);
                DeleteFileW(full);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ------------------------------------------------------------
// 内嵌 DLL 解包到 %TEMP%
// ------------------------------------------------------------
static bool ExtractPayload(wchar_t* out, size_t cap) {
    wchar_t temp[MAX_PATH] = {};
    DWORD tn = GetTempPathW(MAX_PATH, temp);
    if (tn == 0 || tn >= MAX_PATH) {
        PrintW(L"[!] 无法获取临时目录");
        return false;
    }
    wchar_t name[64];
    swprintf(name, 64, L"mc_esp_payload_%08lX.dll",
             (unsigned long)(GetTickCount() ^ GetCurrentProcessId()));
    wcscpy(out, temp);
    wcscat(out, name);
    if (wcslen(out) >= cap) { PrintW(L"[!] 临时路径过长"); return false; }

    HANDLE h = CreateFileW(out, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        PrintW(L"[!] 创建临时 DLL 失败，错误码 %lu", GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, g_payloadDll, g_payloadDllSize, &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != g_payloadDllSize) {
        PrintW(L"[!] 写入临时 DLL 失败（%lu / %u）", written, g_payloadDllSize);
        DeleteFileW(out);
        return false;
    }
    return true;
}

// ------------------------------------------------------------
// 把数据目录通过命名内存映射告诉目标进程内的 DLL。
// 名称固定为 Local\mc_esp_dir_<pid>，DLL 的 DllMain 会优先读取。
// ------------------------------------------------------------
static bool PublishDataDir(DWORD pid, const wchar_t* dir,
                             HANDLE& map, void*& view) {
    map = nullptr; view = nullptr;
    wchar_t name[96];
    swprintf(name, 96, L"Local\\mc_esp_dir_%lu", pid);

    const DWORD size = 4096;
    map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                             0, size, name);
    if (!map) {
        PrintW(L"[!] 创建目录映射失败，错误码 %lu", GetLastError());
        return false;
    }
    view = MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, size);
    if (!view) {
        PrintW(L"[!] 映射目录视图失败，错误码 %lu", GetLastError());
        CloseHandle(map);
        map = nullptr;
        return false;
    }
    memset(view, 0, size);
    memcpy(view, dir, (wcslen(dir) + 1) * sizeof(wchar_t));
    return true;
}

static void CloseDataDir(HANDLE& map, void*& view) {
    if (view) { UnmapViewOfFile(view); view = nullptr; }
    if (map) { CloseHandle(map); map = nullptr; }
}

// ------------------------------------------------------------
// 注入: CreateRemoteThread + LoadLibraryW
// ------------------------------------------------------------
static bool InjectDllW(DWORD pid, const wchar_t* dllPath, const wchar_t* dataDir) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        PrintW(L"[!] OpenProcess(%lu) 失败，错误码 %lu", pid, GetLastError());
        PrintW(L"    请确认游戏正在运行；如仍失败，尝试以管理员身份运行 mc_esp.exe。");
        return false;
    }

#ifdef _WIN64
    BOOL targetWow64 = FALSE;
    if (IsWow64Process(proc, &targetWow64) && targetWow64) {
        PrintW(L"[!] 目标 Minecraft 是 32 位进程，而 mc_esp.exe 是 64 位。");
        PrintW(L"    请使用 64 位 Java 启动 Minecraft。");
        CloseHandle(proc);
        return false;
    }
#else
    PrintW(L"[!] 当前 mc_esp.exe 是 32 位版本，无法注入 64 位游戏。");
    PrintW(L"    请用 64 位 g++ 重新编译（不要加 -m32）。");
    CloseHandle(proc);
    return false;
#endif

    HANDLE cfgMap = nullptr;
    void* cfgView = nullptr;
    if (!PublishDataDir(pid, dataDir, cfgMap, cfgView)) {
        CloseHandle(proc);
        return false;
    }

    const SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        PrintW(L"[!] VirtualAllocEx 失败，错误码 %lu", GetLastError());
        CloseDataDir(cfgMap, cfgView);
        CloseHandle(proc);
        return false;
    }
    if (!WriteProcessMemory(proc, remote, dllPath, bytes, nullptr)) {
        PrintW(L"[!] WriteProcessMemory 失败，错误码 %lu", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseDataDir(cfgMap, cfgView);
        CloseHandle(proc);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibraryW = GetProcAddress(k32, "LoadLibraryW");
    if (!loadLibraryW) {
        PrintW(L"[!] 找不到 kernel32!LoadLibraryW");
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseDataDir(cfgMap, cfgView);
        CloseHandle(proc);
        return false;
    }

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0,
                                       (LPTHREAD_START_ROUTINE)loadLibraryW,
                                       remote, 0, nullptr);
    if (!thread) {
        PrintW(L"[!] CreateRemoteThread 失败，错误码 %lu", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseDataDir(cfgMap, cfgView);
        CloseHandle(proc);
        return false;
    }

    PrintW(L"[*] 远程线程已创建，等待 LoadLibraryW 返回 ...");
    DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD exitCode = 0;
    if (wait == WAIT_OBJECT_0) GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);

    if (wait != WAIT_OBJECT_0) {
        PrintW(L"[!] 等待远程线程超时，注入可能未完成。");
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseDataDir(cfgMap, cfgView);
        CloseHandle(proc);
        return false;
    }

    // LoadLibraryW 的 64 位返回值经线程退出码截断后可能为 0，不能仅凭它判定失败；
    // 真正验证靠下方 esp_log.txt 是否增长。
    if (exitCode == 0)
        PrintW(L"[!] 远程线程返回 0（64 位返回值截断或加载失败），继续检查日志确认 ...");

    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseDataDir(cfgMap, cfgView);   // DllMain 已读完目录，映射可以关闭
    CloseHandle(proc);
    return true;
}

// ------------------------------------------------------------
// 注入结果验证：DLL 会写数据目录下的 esp_log.txt / esp_log_new.txt
// ------------------------------------------------------------
static void VerifyByLog(const wchar_t* dir) {
    wchar_t log1[MAX_PATH], log2[MAX_PATH];
    wcscpy(log1, dir); wcscat(log1, L"esp_log.txt");
    wcscpy(log2, dir); wcscat(log2, L"esp_log_new.txt");
    const ULONGLONG old1 = FileSizeW(log1);
    const ULONGLONG old2 = FileSizeW(log2);
    Sleep(1200);
    const ULONGLONG new1 = FileSizeW(log1);
    const ULONGLONG new2 = FileSizeW(log2);
    if (new1 > old1 || new2 > old2) {
        PrintW(L"[+] 日志已更新，注入成功（esp.ini / esp_log.txt 位于上述数据目录）。");
    } else {
        PrintW(L"[!] 未观察到日志增长。请检查：");
        PrintW(L"    1) 游戏 Java 是否为 64 位");
        PrintW(L"    2) 是否以管理员身份运行 mc_esp.exe");
    }
}

// ------------------------------------------------------------
// 主流程
// ------------------------------------------------------------
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    DWORD pid = 0;
    bool findOnly = false;
    wchar_t dataDir[MAX_PATH * 2] = {0};

    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"-find") == 0) {
            findOnly = true;
        } else if (_wcsicmp(argv[i], L"-pid") == 0 && i + 1 < argc) {
            pid = (DWORD)wcstoul(argv[++i], nullptr, 10);
        } else if (_wcsicmp(argv[i], L"-dir") == 0 && i + 1 < argc) {
            if (wcslen(argv[i + 1]) >= MAX_PATH * 2 - 1) {
                PrintW(L"[!] 数据目录路径过长");
                return 1;
            }
            wcscpy(dataDir, argv[++i]);
            AppendSep(dataDir);
            CreateDirectoryW(dataDir, nullptr);
        } else {
            PrintW(L"用法:");
            PrintW(L"  mc_esp.exe                      自动查找 Minecraft 并注入");
            PrintW(L"  mc_esp.exe -find                仅查找 Minecraft 进程，不注入");
            PrintW(L"  mc_esp.exe -pid <PID>           注入指定 PID");
            PrintW(L"  mc_esp.exe -dir <数据目录>      自定义 esp.ini / esp_log.txt 保存目录");
            PrintW(L"  默认数据目录: %APPDATA%\\mc_esp\\");
            return 1;
        }
    }

    if (dataDir[0] == L'\0') {
        if (!GetDataDirW(dataDir, MAX_PATH * 2)) {
            PrintW(L"[!] 获取默认数据目录失败");
            return 1;
        }
    }

    if (pid == 0) {
        PrintW(L"[*] 正在自动查找 Minecraft 进程 ...");
        pid = AutoFindMinecraftPid();
        if (pid == 0) return 1;
        PrintW(L"[*] 找到 Minecraft 进程，PID=%lu", pid);
    } else if (!IsJavaProcessW(pid)) {
        PrintW(L"[!] 警告：PID %lu 不是 java/javaw 进程，仍将尝试注入。", pid);
    }

    if (findOnly) {
        PrintW(L"[+] 查找完成，未执行注入。");
        return 0;
    }

    CleanupStalePayloads();

    wchar_t tempDll[MAX_PATH];
    if (!ExtractPayload(tempDll, MAX_PATH)) return 1;

    PrintW(L"[*] 单文件内嵌 DLL 已解包: %ls", tempDll);
    PrintW(L"[*] 数据目录: %ls", dataDir);

    bool ok = InjectDllW(pid, tempDll, dataDir);

    // 注入完成后尝试清理临时 DLL；目标进程仍占用时交给系统延迟删除。
    if (DeleteFileW(tempDll)) {
        PrintW(L"[*] 临时 DLL 已清理");
    } else {
        MoveFileExW(tempDll, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        PrintW(L"[!] 临时 DLL 暂被游戏占用，已安排稍后清理（%ls）", tempDll);
    }

    if (!ok) return 1;
    PrintW(L"[+] 注入流程完成，正在验证 DLL 初始化日志 ...");
    VerifyByLog(dataDir);
    PrintW(L"[*] 游戏内按 INSERT 呼出菜单（ESP / 连点设置均在菜单内）。");
    return 0;
}
