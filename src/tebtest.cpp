// tebtest.cpp — 验证 x64 TEB.StartAddress 偏移
#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <intrin.h>

typedef NTSTATUS(NTAPI* pNtQueryInformationThread)(HANDLE, LONG, PVOID, ULONG, PULONG);

int main() {
    auto NtQueryInformationThread = (pNtQueryInformationThread)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    if (!NtQueryInformationThread) { printf("no NtQueryInformationThread\n"); return 1; }

    ULONG_PTR start = 0;
    NTSTATUS st = NtQueryInformationThread(GetCurrentThread(), 0x9 /*ThreadQuerySetWin32StartAddress*/,
                                           &start, sizeof(start), nullptr);
    printf("NtQuery StartAddress        = 0x%llX (st=0x%lX)\n", (ULONGLONG)start, (ULONG)st);

    BYTE* teb = (BYTE*)__readgsqword(0x30);
    printf("TEB base                    = 0x%llX\n", (ULONGLONG)teb);
    for (DWORD off = 0x1450; off <= 0x14A0; off += 8) {
        ULONG_PTR v = *(ULONG_PTR*)(teb + off);
        const char* mark = (v == start) ? "  <== MATCH" : "";
        printf("  TEB+0x%04X = 0x%llX%s\n", off, (ULONGLONG)v, mark);
    }
    // 打印几个附近的已知字段以便对齐
    printf("TEB+0x60  PEB       = 0x%llX\n", (ULONGLONG)*(PVOID*)(teb + 0x60));
    printf("TEB+0x48  TID       = 0x%llX\n", (ULONGLONG)*(PULONG)(teb + 0x48));
    return 0;
}
