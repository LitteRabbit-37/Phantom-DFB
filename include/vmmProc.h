#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <windows.h>

// --- ReadProcessMemory backend (no driver needed, no anti-cheat in DFB) ---

// --- Initialization & process management ---
BOOL  MemoryBackendInit();
void  SetProcessPid(DWORD dwPid);
DWORD GetProcessPid(std::string strPorName);
std::string GetPidName(DWORD dwPid);
std::vector<DWORD> GetProcessPidList();
LPSTR ProcessGetInformationString(DWORD dwPID);

// --- Module resolution ---
uint64_t GetModuleFromName(std::string strName);
uint64_t GetModuleSize(std::string strName);

// --- Memory access ---
BOOL ReadMemory(uint64_t uBaseAddr, LPVOID lpBuffer, DWORD nSize);
BOOL WriteMemory(uint64_t uBaseAddr, LPVOID lpBuffer, DWORD nSize);
std::vector<BYTE> ReadBYTE(uint64_t ptr, SIZE_T size);

// --- Templated helpers (header-only) ---
template<typename T>
T Read(uint64_t ptr)
{
    T buff{};
    ReadMemory(ptr, &buff, sizeof(T));
    return buff;
}

template<typename T>
BOOL Write(uint64_t ptr, T value)
{
    WriteMemory(ptr, &value, sizeof(T));
    return true;
}
