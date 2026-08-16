// tebtest3.cpp — 验证 RtlUserThreadStart 伪装线程起点的机制
#include <windows.h>
#include <winternl.h>
#include <cstdio>

typedef NTSTATUS(NTAPI* pNtQueryInformationThread)(HANDLE, LONG, PVOID, ULONG, PULONG);

static DWORD WINAPI myThread(LPVOID p) {
    printf("  myThread ran! (param=%p)\n", p);
    return 42;
}

int main() {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    auto RtlUserThreadStart = (LPTHREAD_START_ROUTINE)GetProcAddress(ntdll, "RtlUserThreadStart");
    auto NtQueryInformationThread = (pNtQueryInformationThread)GetProcAddress(ntdll, "NtQueryInformationThread");
    printf("RtlUserThreadStart = %p\n", (void*)RtlUserThreadStart);

    // 关键：起点 = RtlUserThreadStart（合法模块地址），参数 = 真实函数指针
    HANDLE h = CreateThread(nullptr, 0, RtlUserThreadStart, (LPVOID)&myThread, 0, nullptr);
    if (!h) { printf("CreateThread failed err=%lu\n", GetLastError()); return 1; }

    DWORD tid = GetThreadId(h);
    ULONG_PTR start = 0;
    NtQueryInformationThread(h, 0x9, &start, sizeof(start), nullptr);
    printf("  created thread NtQuery StartAddress = 0x%llX\n", (ULONGLONG)start);
    printf("  == RtlUserThreadStart ? %s\n", ((ULONG_PTR)start == (ULONG_PTR)RtlUserThreadStart) ? "YES" : "NO");

    DWORD rc = 0;
    WaitForSingleObject(h, 5000);
    GetExitCodeThread(h, &rc);
    CloseHandle(h);
    printf("  thread exit code = %lu (expect 42)\n", rc);
    return 0;
}
