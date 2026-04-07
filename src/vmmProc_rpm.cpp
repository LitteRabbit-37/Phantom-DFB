// vmmProc_rpm.cpp — ReadProcessMemory backend (no driver needed)
//
// Uses standard Win32 ReadProcessMemory / WriteProcessMemory.
// Works when EAC is disabled (solo/offline mode).
// No kernel driver, no BSOD risk, no admin required.
//
// Default backend for DFB (no anti-cheat, no driver needed).

#include "vmmProc.h"
#include "stealth.h"
#include <TlHelp32.h>
#include <cstdio>

static HANDLE g_hProcess = NULL;
static DWORD  GameID     = 0;

// ============================================================================
// Public API — matches vmmProc.h interface
// ============================================================================

BOOL MemoryBackendInit()
{
    DBG("[+] RPM backend initialized (no driver needed)\n");
    return TRUE;
}

void SetProcessPid(DWORD dwPid)
{
    GameID = dwPid;
    if (g_hProcess) CloseHandle(g_hProcess);
    g_hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                             PROCESS_QUERY_INFORMATION, FALSE, dwPid);
    if (g_hProcess)
        DBG("[+] Opened process %u with RPM access\n", dwPid);
    else
        DBG("[-] OpenProcess failed for PID %u (error %lu)\n", dwPid, GetLastError());
}

DWORD GetProcessPid(std::string strPorName)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = {}; pe.dwSize = sizeof(pe);
    if (Process32First(hSnap, &pe)) {
        do {
            if (strPorName == pe.szExeFile) {
                CloseHandle(hSnap);
                return pe.th32ProcessID;
            }
        } while (Process32Next(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return 0;
}

std::string GetPidName(DWORD dwPid)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return "";
    PROCESSENTRY32 pe = {}; pe.dwSize = sizeof(pe);
    if (Process32First(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == dwPid) {
                CloseHandle(hSnap);
                return pe.szExeFile;
            }
        } while (Process32Next(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return "";
}

std::vector<DWORD> GetProcessPidList()
{
    std::vector<DWORD> pids;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32 pe = {}; pe.dwSize = sizeof(pe);
    if (Process32First(hSnap, &pe)) {
        do { pids.push_back(pe.th32ProcessID); }
        while (Process32Next(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return pids;
}

LPSTR ProcessGetInformationString(DWORD dwPID)
{
    std::string name = GetPidName(dwPID);
    if (name.empty()) return nullptr;
    char* buf = (char*)malloc(name.size() + 1);
    if (buf) strcpy_s(buf, name.size() + 1, name.c_str());
    return buf;
}

uint64_t GetModuleFromName(std::string strName)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GameID);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32 me = {}; me.dwSize = sizeof(me);
    if (Module32First(hSnap, &me)) {
        do {
            if (strName == me.szModule) {
                CloseHandle(hSnap);
                return (uint64_t)me.modBaseAddr;
            }
        } while (Module32Next(hSnap, &me));
    }
    CloseHandle(hSnap);
    return 0;
}

uint64_t GetModuleSize(std::string strName)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GameID);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32 me = {}; me.dwSize = sizeof(me);
    if (Module32First(hSnap, &me)) {
        do {
            if (strName == me.szModule) {
                CloseHandle(hSnap);
                return me.modBaseSize;
            }
        } while (Module32Next(hSnap, &me));
    }
    CloseHandle(hSnap);
    return 0;
}

// ============================================================================
// Memory read/write — direct ReadProcessMemory / WriteProcessMemory
// ============================================================================

BOOL ReadMemory(uint64_t uBaseAddr, LPVOID lpBuffer, DWORD nSize)
{
    if (!g_hProcess || nSize == 0) {
        if (nSize) memset(lpBuffer, 0, nSize);
        return FALSE;
    }
    SIZE_T bytesRead = 0;
    BOOL ok = ReadProcessMemory(g_hProcess, (LPCVOID)uBaseAddr, lpBuffer, nSize, &bytesRead);
    if (!ok) memset(lpBuffer, 0, nSize);
    return ok;
}

BOOL WriteMemory(uint64_t uBaseAddr, LPVOID lpBuffer, DWORD nSize)
{
    if (!g_hProcess || nSize == 0) return FALSE;
    SIZE_T bytesWritten = 0;
    return WriteProcessMemory(g_hProcess, (LPVOID)uBaseAddr, lpBuffer, nSize, &bytesWritten);
}

std::vector<BYTE> ReadBYTE(uint64_t ptr, SIZE_T size)
{
    std::vector<BYTE> bytes(size);
    ReadMemory(ptr, bytes.data(), (DWORD)size);
    return bytes;
}

