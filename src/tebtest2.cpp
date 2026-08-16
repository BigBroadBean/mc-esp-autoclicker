// tebtest2.cpp — 全 TEB 扫描 StartAddress 存储位置
#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <intrin.h>

typedef NTSTATUS(NTAPI* pNtQueryInformationThread)(HANDLE, LONG, PVOID, ULONG, PULONG);

int main() {
    auto NtQueryInformationThread = (pNtQueryInformationThread)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    ULONG_PTR start = 0;
    NTSTATUS st = NtQueryInformationThread(GetCurrentThread(), 0x9, &start, sizeof(start), nullptr);
    printf("NtQuery StartAddress = 0x%llX (st=0x%lX)\n", (ULONGLONG)start, (ULONG)st);

    BYTE* teb = (BYTE*)__readgsqword(0x30);
    int hits = 0;
    // TEB 通常 4KB（最多几 KB），扫描前 0x2000 字节找该值（按 8 字节对齐）
    for (DWORD off = 0; off < 0x2000; off += 8) {
        if (*(ULONG_PTR*)(teb + off) == start) {
            printf("  FOUND at TEB+0x%04X\n", off);
            if (++hits > 10) break;
        }
    }
    if (!hits) printf("  not found in first 0x2000 bytes of TEB\n");

    // 另开一个线程验证：线程内 NtQuery 值 + 该值在本线程 TEB 的位置
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        auto nq = (pNtQueryInformationThread)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                                            "NtQueryInformationThread");
        ULONG_PTR s = 0;
        nq(GetCurrentThread(), 0x9, &s, sizeof(s), nullptr);
        BYTE* t = (BYTE*)__readgsqword(0x30);
        printf("  [thread] NtQuery = 0x%llX\n", (ULONGLONG)s);
        int h = 0;
        for (DWORD o = 0; o < 0x2000; o += 8)
            if (*(ULONG_PTR*)(t + o) == s) { printf("    [thread] FOUND at TEB+0x%04X\n", o); if (++h > 10) break; }
        if (!h) printf("    [thread] not found\n");
        return 0;
    }, nullptr, 0, nullptr);
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    return 0;
}
