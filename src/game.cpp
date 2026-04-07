#include "Game.h"
#include "overlay.h"
#include "Offset.h"
#include "vmmProc.h"
#include "pattern_scanner.h"
#include "settings.h"
#include "stealth.h"
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

Settings g_Settings;
Overlay  Draw = Overlay();

std::vector<Player> g_Players;
std::mutex          g_DataMutex;

// ---------------------------------------------------------------------------
// Trainer patches — AOB patterns from CE "Find what writes"
//
// Each entry: AOB pattern to locate the instruction, offset from match
// to the bytes we NOP, and the NOP size.
//
// GameAssembly.dll+26D1D8: sub [rbx+20],edi      (health damage)
// GameAssembly.dll+2B22B5: sub [rbx+110],eax     (ammo consumption)
// GameAssembly.dll+294A2F: add [rbx+30],edi       (money spending)
// GameAssembly.dll+276B49: dec [rbx+18]           (medkit use)
// ---------------------------------------------------------------------------

static TrainerPatch g_Patches[PATCH_COUNT] = {
    // God Mode (player only): force iFrame skip in PlayerHealth.TakeDamage(int)
    // This skips the base Health.TakeDamage call for the PLAYER only.
    // Zombies use Health.TakeDamage directly → still take damage (bazooka works!)
    // Patch: 75 17 (jnz) → EB 17 (jmp) = always skip damage
    {
        "God Mode",
        "80 B9 88 00 00 00 00 48 8B D9 75 17 45 33 C0 E8",
        10, 1, {0xEB},   // 75 → EB (jnz → jmp)
        0, {}, false, false, 1  // group 1 = god mode
    },
    // God Mode part 2: PlayerHealth.TakeDamage(int, Vec3, DamageType)
    // Patch: 75 5A (jnz) → EB 5A (jmp)
    {
        "God Mode (vec)",
        "80 B9 88 00 00 00 00 49 8B D8 48 8B F9 75 5A",
        13, 1, {0xEB},   // 75 → EB
        0, {}, false, false, 1  // group 1 = god mode (toggles with above)
    },
    // Infinite Ammo: NOP the ammo decrement
    // Context: mov eax,[rdi+6C]; [sub [rbx+110],eax]; cmp [rbx+110],0; jg ...
    {
        "Infinite Ammo",
        "8B 47 6C 29 83 10 01 00 00 83 BB 10 01 00 00 00 7F",
        3, 6, {0x90,0x90,0x90,0x90,0x90,0x90},
        0, {}, false, false, 0
    },
    // Infinite Money: NOP the money spend instruction
    // Context: xor r8d,r8d; call ...; [add [rbx+30],edi]; lea rcx,[rbx+30]; mov eax,[rcx]
    {
        "Infinite Money",
        "45 33 C0 E8 ?? ?? ?? ?? 01 7B 30 48 8D 4B 30 8B 01",
        8, 3, {0x90,0x90,0x90},
        0, {}, false, false, 0
    },
    // Infinite Medkits: NOP the medkit decrement
    // Context: xor edx,edx; call ...; [dec [rbx+18]]; lea rcx,[rbx+18]; mov rsi,[rbx+40]
    {
        "Infinite Medkits",
        "33 D2 E8 ?? ?? ?? ?? FF 4B 18 48 8D 4B 18 48 8B 73 40",
        7, 3, {0x90,0x90,0x90},
        0, {}, false, false, 0
    },
    // Full Auto: force fireType==0 (auto) branch in Gun.FireInput
    // Context: cmp [rdi+88],edx (fireType vs 0); [jz auto_path]
    // Original: 0F 84 A4 00 00 00 (jz rel32)
    // Patch:    E9 A5 00 00 00 90 (jmp rel32 + nop) = always take auto path
    {
        "Full Auto",
        "39 97 88 00 00 00 0F 84 ?? ?? ?? ?? 0F B6",
        6, 6, {0xE9, 0xA5, 0x00, 0x00, 0x00, 0x90},
        0, {}, false, false, 0
    },
};

// ---------------------------------------------------------------------------
// Color helper
// ---------------------------------------------------------------------------

ImVec4 YColor(DWORD rgb, FLOAT a)
{
    return ImVec4(
        (float)((rgb >> 16) & 0xFF) / 255.f,
        (float)((rgb >> 8)  & 0xFF) / 255.f,
        (float)((rgb >> 0)  & 0xFF) / 255.f,
        a);
}

// ---------------------------------------------------------------------------
// Thread entry points
// ---------------------------------------------------------------------------

static DWORD WINAPI ThreadGameStart(void* Param)
{
    ((Game*)Param)->GameStart();
    return 0;
}

static DWORD WINAPI ThreadEntityLoop(void* Param)
{
    ((Game*)Param)->EntityLoop();
    return 0;
}

static DWORD WINAPI ThreadRenderLoop(void* Param)
{
    ((Game*)Param)->RenderLoop();
    return 0;
}

// ---------------------------------------------------------------------------
// Find the main window of a process by PID
// ---------------------------------------------------------------------------

struct FindWindowData {
    DWORD pid;
    HWND  result;
};

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    auto* data = (FindWindowData*)lParam;
    DWORD wndPid = 0;
    GetWindowThreadProcessId(hwnd, &wndPid);
    if (wndPid == data->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == NULL)
    {
        data->result = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND FindWindowByPID(DWORD pid)
{
    FindWindowData data = { pid, NULL };
    EnumWindows(EnumWindowsProc, (LPARAM)&data);
    return data.result;
}

// Range-check helper
static inline bool IsValidUserPtr(uint64_t addr)
{
    return addr >= 0x10000 && addr <= 0x7FFFFFFFFFFFULL;
}

// ---------------------------------------------------------------------------
// PatchCode — write bytes to a code page in the target process
//
// Handles VirtualProtectEx to make the page writable, writes, then restores.
// ---------------------------------------------------------------------------

static bool PatchCode(DWORD pid, uint64_t addr, const void* bytes, size_t size)
{
    HANDLE hProc = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (!hProc) return false;

    DWORD oldProt = 0;
    VirtualProtectEx(hProc, (LPVOID)addr, size, PAGE_EXECUTE_READWRITE, &oldProt);

    SIZE_T written = 0;
    BOOL ok = WriteProcessMemory(hProc, (LPVOID)addr, bytes, size, &written);

    if (oldProt)
        VirtualProtectEx(hProc, (LPVOID)addr, size, oldProt, &oldProt);

    CloseHandle(hProc);
    return ok && written == size;
}

// ---------------------------------------------------------------------------
// InitTrainer — AOB scan GameAssembly.dll for all patch points
// ---------------------------------------------------------------------------

void Game::InitTrainer()
{
    if (!GameAssemblyBase || !GameAssemblySize)
    {
        DBG("[Trainer] GameAssembly.dll not found — trainer disabled\n");
        return;
    }

    DBG("[Trainer] Scanning GameAssembly.dll (base=0x%llX, size=%llu MB)...\n",
        (unsigned long long)GameAssemblyBase,
        (unsigned long long)(GameAssemblySize / (1024 * 1024)));

    int found = 0;
    for (int i = 0; i < PATCH_COUNT; i++)
    {
        TrainerPatch& p = g_Patches[i];
        uint64_t match = PatternScan(GameAssemblyBase, GameAssemblySize, p.aob);
        if (match)
        {
            p.addr = match + p.aobPatchOffset;
            p.found = true;
            found++;

            // Save original bytes
            ReadMemory(p.addr, p.orig, p.patchSize);

            DBG("[Trainer] %s: found at 0x%llX (GameAssembly.dll+0x%llX)\n",
                p.name, (unsigned long long)p.addr,
                (unsigned long long)(p.addr - GameAssemblyBase));
        }
        else
        {
            DBG("[Trainer] %s: AOB NOT FOUND\n", p.name);
        }
    }

    trainerReady = (found > 0);
    DBG("[Trainer] %d/%d patches found\n", found, PATCH_COUNT);
}

// ---------------------------------------------------------------------------
// TogglePatch — enable/disable a NOP patch
// ---------------------------------------------------------------------------

bool Game::TogglePatch(int patchIdx, bool enable)
{
    if (patchIdx < 0 || patchIdx >= PATCH_COUNT)
        return false;

    TrainerPatch& p = g_Patches[patchIdx];
    if (!p.found || !p.addr)
        return false;

    if (enable && !p.active)
    {
        // Write patch bytes (NOP, JMP, or custom)
        if (PatchCode(GamePid, p.addr, p.patchBytes, p.patchSize))
        {
            p.active = true;
            DBG("[Trainer] %s: ENABLED\n", p.name);

            // Enable all patches in same group
            if (p.group > 0)
                for (int i = 0; i < PATCH_COUNT; i++)
                    if (i != patchIdx && g_Patches[i].group == p.group)
                        TogglePatch(i, true);

            return true;
        }
        DBG("[Trainer] %s: write failed\n", p.name);
    }
    else if (!enable && p.active)
    {
        // Restore original bytes
        if (PatchCode(GamePid, p.addr, p.orig, p.patchSize))
        {
            p.active = false;
            DBG("[Trainer] %s: DISABLED\n", p.name);

            // Disable all patches in same group
            if (p.group > 0)
                for (int i = 0; i < PATCH_COUNT; i++)
                    if (i != patchIdx && g_Patches[i].group == p.group)
                        TogglePatch(i, false);

            return true;
        }
        DBG("[Trainer] %s: restore failed\n", p.name);
    }

    return false;
}

// ---------------------------------------------------------------------------
// IL2CPP helpers — resolve class pointers and static fields
// ---------------------------------------------------------------------------

uint64_t Game::GetStaticFields(uint32_t classRVA)
{
    if (!GameAssemblyBase) return 0;
    uint64_t classPtr = Read<uint64_t>(GameAssemblyBase + classRVA);
    if (!IsValidUserPtr(classPtr)) return 0;
    return Read<uint64_t>(classPtr + 0xB8);  // static_fields
}

void Game::InitIL2CPPPointers()
{
    using namespace Offset::IL2CPP;

    il2cpp.gmStaticFields  = GetStaticFields(GameManager_Class);
    il2cpp.gdStaticFields  = GetStaticFields(GameDirector_Class);
    il2cpp.aiStaticFields  = GetStaticFields(AIManager_Class);
    il2cpp.rpmStaticFields = GetStaticFields(RenderPipelineMgr_Class);

    // Resolve GameDirector instance via AIManager.instance.gameDirector
    if (il2cpp.aiStaticFields)
    {
        uint64_t aiInstance = Read<uint64_t>(il2cpp.aiStaticFields + AI_Instance);
        if (IsValidUserPtr(aiInstance))
            il2cpp.gameDirector = Read<uint64_t>(aiInstance + AI_GameDirector);
    }

    DBG("[IL2CPP] Static fields resolved:\n");
    DBG("[IL2CPP]   GameManager:  0x%llX\n", (unsigned long long)il2cpp.gmStaticFields);
    DBG("[IL2CPP]   GameDirector: 0x%llX (static), instance=0x%llX\n",
        (unsigned long long)il2cpp.gdStaticFields, (unsigned long long)il2cpp.gameDirector);
    DBG("[IL2CPP]   AIManager:    0x%llX\n", (unsigned long long)il2cpp.aiStaticFields);
    DBG("[IL2CPP]   RPM:          0x%llX\n", (unsigned long long)il2cpp.rpmStaticFields);

    // --- Phase 4: Try to find Camera via RenderPipelineManager.s_Cameras ---
    if (il2cpp.rpmStaticFields)
    {
        uint64_t sCameras = Read<uint64_t>(il2cpp.rpmStaticFields + RPM_Cameras);
        if (IsValidUserPtr(sCameras))
        {
            int32_t camCount = Read<int32_t>(sCameras + 0x18);  // List._size
            uint64_t camItems = Read<uint64_t>(sCameras + 0x10); // List._items
            DBG("[IL2CPP]   s_Cameras: %d cameras at 0x%llX\n", camCount, (unsigned long long)camItems);

            if (camCount > 0 && IsValidUserPtr(camItems))
            {
                uint64_t cam0 = Read<uint64_t>(camItems + 0x20);  // Camera[0]
                if (IsValidUserPtr(cam0))
                {
                    uint64_t nativeCam = Read<uint64_t>(cam0 + 0x10);  // m_CachedPtr
                    DBG("[IL2CPP]   Camera[0]: managed=0x%llX native=0x%llX\n",
                        (unsigned long long)cam0, (unsigned long long)nativeCam);

                    // Probe native Camera for View matrix (orthonormal rotation in [-1,1])
                    if (IsValidUserPtr(nativeCam))
                    {
                        uint8_t dump[0x800];
                        if (ReadMemory(nativeCam, dump, sizeof(dump)))
                        {
                            DBG("[IL2CPP]   Probing native Camera for matrices...\n");
                            for (uint32_t off = 0; off + 64 <= sizeof(dump); off += 4)
                            {
                                float* m = (float*)(dump + off);
                                // Check for orthonormal rotation (View matrix signature)
                                float c0 = sqrtf(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);
                                float c1 = sqrtf(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]);
                                float c2 = sqrtf(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
                                if (fabsf(c0-1.f)>0.1f || fabsf(c1-1.f)>0.1f || fabsf(c2-1.f)>0.1f) continue;
                                if (fabsf(m[3])>0.01f || fabsf(m[7])>0.01f || fabsf(m[11])>0.01f) continue;
                                if (fabsf(m[15]-1.f)>0.1f) continue;
                                DBG("[IL2CPP]   View matrix candidate at native+0x%X\n", off);
                                DBG("[IL2CPP]     [%.4f %.4f %.4f %.4f]\n", m[0],m[4],m[8],m[12]);
                                DBG("[IL2CPP]     [%.4f %.4f %.4f %.4f]\n", m[1],m[5],m[9],m[13]);
                                DBG("[IL2CPP]     [%.4f %.4f %.4f %.4f]\n", m[2],m[6],m[10],m[14]);
                            }
                        }
                    }
                }
            }
        }
        else
        {
            DBG("[IL2CPP]   s_Cameras: NULL (game uses built-in pipeline)\n");
        }
    }
}

// ---------------------------------------------------------------------------
// FindClassByName — scan GA.dll metadata for an Il2CppClass by name
//
// IL2CPP stores class metadata pointers in a table within GameAssembly.dll.
// Each entry at [GA+RVA] points to an Il2CppClass struct where +0x10 = name.
// We iterate the known RVA range to find the class by name string comparison.
// ---------------------------------------------------------------------------

uint64_t Game::FindClassByName(const char* name)
{
    using namespace Offset::IL2CPP;
    if (!GameAssemblyBase || !GameAssemblySize || !name) return 0;

    size_t nameLen = strlen(name);
    if (nameLen == 0 || nameLen > 63) return 0;

    // Clamp scan range to actual GA.dll size
    uint32_t rvaStart = MetadataRVA_Start;
    uint32_t rvaEnd   = (uint32_t)min((uint64_t)MetadataRVA_End, GameAssemblySize);
    if (rvaStart >= rvaEnd) return 0;

    DBG("[IL2CPP] Searching for class '%s' in GA.dll RVA 0x%X..0x%X\n", name, rvaStart, rvaEnd);

    // Read GA.dll data in chunks to avoid millions of individual RPM calls
    const size_t CHUNK = 0x10000;  // 64 KB chunks
    std::vector<uint8_t> chunk(CHUNK);

    for (uint32_t base = rvaStart; base < rvaEnd; base += CHUNK)
    {
        size_t readSz = min((size_t)(rvaEnd - base), CHUNK);
        if (!ReadMemory(GameAssemblyBase + base, chunk.data(), (DWORD)readSz))
            continue;

        // Scan chunk for valid pointers (8-byte aligned)
        for (size_t off = 0; off + 8 <= readSz; off += 8)
        {
            uint64_t klassPtr = *(uint64_t*)(chunk.data() + off);
            if (!IsValidUserPtr(klassPtr)) continue;

            uint64_t namePtr = Read<uint64_t>(klassPtr + Klass_Name);
            if (!IsValidUserPtr(namePtr)) continue;

            char buf[64] = {};
            if (!ReadMemory(namePtr, buf, 63)) continue;
            buf[63] = '\0';

            if (strcmp(buf, name) == 0)
            {
                uint32_t rva = base + (uint32_t)off;
                DBG("[IL2CPP] Found class '%s' at klass=0x%llX (RVA=0x%X)\n",
                    name, (unsigned long long)klassPtr, rva);
                return klassPtr;
            }
        }
    }

    DBG("[IL2CPP] Class '%s' not found in GA.dll metadata.\n", name);
    return 0;
}

// ---------------------------------------------------------------------------
// ScanForPlayerRefrences — GC heap scan for the unique PlayerRefrences instance
//
// Strategy:
// 1. Find the Il2CppClass* klass pointer for "PlayerRefrences" via FindClassByName
// 2. Scan all readable memory for IL2CPP objects where [addr+0x0] == klassPtr
// 3. Validate: all 13 pointer fields at +0x18..+0x80 must be valid
// 4. Cross-validate: PlayerCurrency.refrences (+0x38→+0x38) points back to obj
// ---------------------------------------------------------------------------

bool Game::ScanForPlayerRefrences()
{
    using namespace Offset::IL2CPP;

    // Check cache: try cached instance first, then cached klass for fast scan
    uint64_t cached = offsetCache.Get("player_refrences");
    if (cached)
    {
        uint64_t klass = Read<uint64_t>(cached + Obj_Klass);
        if (IsValidUserPtr(klass))
        {
            uint64_t namePtr = Read<uint64_t>(klass + Klass_Name);
            if (IsValidUserPtr(namePtr))
            {
                char buf[32] = {};
                ReadMemory(namePtr, buf, 31);
                if (strcmp(buf, "PlayerRefrences") == 0)
                {
                    player.klassPtr = klass;
                    player.instance = cached;
                    DBG("[Player] Restored from cache: instance=0x%llX klass=0x%llX\n",
                        (unsigned long long)cached, (unsigned long long)klass);
                    ResolvePlayerChains();
                    return true;
                }
            }
        }
        DBG("[Player] Cached instance stale — re-scanning.\n");
    }

    // Try cached klass pointer (survives respawn, only changes on game restart)
    uint64_t cachedKlass = offsetCache.Get("player_klass");
    if (!player.klassPtr && cachedKlass)
    {
        uint64_t namePtr = Read<uint64_t>(cachedKlass + Klass_Name);
        if (IsValidUserPtr(namePtr))
        {
            char buf[32] = {};
            ReadMemory(namePtr, buf, 31);
            if (strcmp(buf, "PlayerRefrences") == 0)
            {
                player.klassPtr = cachedKlass;
                DBG("[Player] Klass restored from cache: 0x%llX\n", (unsigned long long)cachedKlass);
            }
        }
    }

    // Phase 1: Fast path — find klass by token+instSize (stable per game version)
    if (!player.klassPtr)
    {
        DBG("[Player] Scanning for klass (token=0x%08X instSize=0x%X)...\n",
            PR_ExpectedToken, PR_ExpectedInstanceSize);

        HANDLE hKlass = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, GamePid);
        if (hKlass)
        {
            MEMORY_BASIC_INFORMATION mbiK;
            uint64_t kAddr = 0x10000;
            const size_t KCHUNK = 256 * 1024;

            while (kAddr < 0x7FFFFFFFFFFFULL && !player.klassPtr)
            {
                if (VirtualQueryEx(hKlass, (LPCVOID)kAddr, &mbiK, sizeof(mbiK)) == 0)
                    break;

                uint64_t kBase = (uint64_t)mbiK.BaseAddress;
                size_t kSize = mbiK.RegionSize;

                bool readable = (mbiK.State == MEM_COMMIT) &&
                                (mbiK.Protect & (PAGE_READONLY | PAGE_READWRITE)) &&
                                !(mbiK.Protect & PAGE_GUARD);

                if (readable && kSize >= 0x120)
                {
                    for (size_t cOff = 0; cOff < kSize; cOff += KCHUNK)
                    {
                        size_t cSz = min(KCHUNK, kSize - cOff);
                        if (cSz < 0x120) break;

                        std::vector<uint8_t> kBuf(cSz);
                        SIZE_T kRead = 0;
                        if (!ReadProcessMemory(hKlass, (LPCVOID)(kBase + cOff), kBuf.data(), cSz, &kRead))
                            continue;
                        if (kRead < 0x120) continue;

                        for (size_t j = 0; j + 0x120 <= kRead; j += 8)
                        {
                            uint32_t tok = *(uint32_t*)(kBuf.data() + j + Klass_Token);
                            if (tok != PR_ExpectedToken) continue;

                            uint32_t iSz = *(uint32_t*)(kBuf.data() + j + Klass_InstanceSize);
                            if (iSz != PR_ExpectedInstanceSize) continue;

                            uint64_t nameP = *(uint64_t*)(kBuf.data() + j + Klass_Name);
                            if (!IsValidUserPtr(nameP)) continue;

                            char nm[32] = {};
                            if (ReadMemory(nameP, nm, 31) && strcmp(nm, "PlayerRefrences") == 0)
                            {
                                player.klassPtr = kBase + cOff + j;
                                DBG("[Player] Klass found at 0x%llX (token match)\n",
                                    (unsigned long long)player.klassPtr);
                                break;
                            }
                        }
                        if (player.klassPtr) break;
                    }
                }

                kAddr = kBase + kSize;
                if (kAddr <= (uint64_t)mbiK.BaseAddress) break;
            }
            CloseHandle(hKlass);
        }
    }

    // Phase 2: Heap scan — structural pattern matching
    // PlayerRefrences has 13 consecutive valid pointers at +0x18..+0x80.
    // Cross-validate: PlayerCurrency (+0x38) has a back-pointer at +0x38 → this object.
    // If we have the klass pointer, also match on +0x00 for speed.
    HANDLE hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, GamePid);
    if (!hProc)
    {
        DBG("[Player] ERROR: OpenProcess failed.\n");
        return false;
    }

    const size_t CHUNK_SZ = 4 * 1024 * 1024;
    MEMORY_BASIC_INFORMATION mbi;
    uint64_t scanAddr = 0x10000;

    struct Candidate {
        uint64_t addr;
        int      validPtrs;
        bool     crossValidated;
    };
    std::vector<Candidate> candidates;

    DBG("[Player] Scanning heap for PlayerRefrences (structural match, klass=%s)...\n",
        player.klassPtr ? "known" : "unknown");

    while (scanAddr < 0x7FFFFFFFFFFFULL && candidates.size() < 20)
    {
        if (VirtualQueryEx(hProc, (LPCVOID)scanAddr, &mbi, sizeof(mbi)) == 0)
            break;

        uint64_t regionBase = (uint64_t)mbi.BaseAddress;
        size_t   regionSize = mbi.RegionSize;

        bool readable = (mbi.State == MEM_COMMIT) &&
                        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                        !(mbi.Protect & PAGE_GUARD);

        // Skip small regions and regions inside GA.dll or exe (managed objects are on GC heap)
        bool isModule = (regionBase >= GameAssemblyBase && regionBase < GameAssemblyBase + GameAssemblySize) ||
                        (regionBase >= GameBase && regionBase < GameBase + 0x1000000);

        if (readable && regionSize >= 0x88 && !isModule)
        {
            for (size_t chunkOff = 0; chunkOff < regionSize; chunkOff += CHUNK_SZ)
            {
                size_t readSz = min(CHUNK_SZ, regionSize - chunkOff);
                if (readSz < 0x88) break;

                std::vector<uint8_t> chunk(readSz);
                SIZE_T bytesRead = 0;
                if (!ReadProcessMemory(hProc, (LPCVOID)(regionBase + chunkOff), chunk.data(), readSz, &bytesRead))
                    continue;
                if (bytesRead < 0x88) continue;

                // Scan at 8-byte alignment for IL2CPP objects
                for (size_t i = 0; i + 0x88 <= bytesRead; i += 8)
                {
                    // Quick reject: +0x00 must be a valid pointer (klass)
                    uint64_t objKlass = *(uint64_t*)(&chunk[i]);
                    if (!IsValidUserPtr(objKlass)) continue;

                    // If we know the klass, fast-filter
                    if (player.klassPtr && objKlass != player.klassPtr) continue;

                    // Read all 13 pointer fields at +0x18 through +0x80
                    uint64_t fields[14] = {}; // fields[0]=+0x18, fields[12]=+0x80
                    int validCount = 0;
                    bool allValid = true;
                    for (int fi = 0; fi < 13; fi++)
                    {
                        uint32_t foff = 0x18 + fi * 8;
                        uint64_t fieldVal;
                        if (i + foff + 8 <= bytesRead)
                            fieldVal = *(uint64_t*)(&chunk[i + foff]);
                        else
                            fieldVal = Read<uint64_t>(regionBase + chunkOff + i + foff);

                        fields[fi] = fieldVal;
                        if (IsValidUserPtr(fieldVal))
                            validCount++;
                        else
                        {
                            allValid = false;
                            break;
                        }
                    }

                    if (!allValid || validCount < 13) continue;

                    uint64_t objAddr = regionBase + chunkOff + i;

                    // All 13 fields must be DISTINCT from each other AND from objAddr
                    bool distinct = true;
                    for (int a = 0; a < 13 && distinct; a++)
                    {
                        if (fields[a] == objAddr) { distinct = false; break; }
                        for (int b = a + 1; b < 13 && distinct; b++)
                        {
                            if (fields[a] == fields[b]) distinct = false;
                        }
                    }
                    if (!distinct) continue;

                    // Validate klass has a readable name at +0x10
                    uint64_t namePtr = Read<uint64_t>(objKlass + Klass_Name);
                    if (!IsValidUserPtr(namePtr)) continue;
                    char klassName[64] = {};
                    if (!ReadMemory(namePtr, klassName, 63)) continue;
                    klassName[63] = '\0';
                    // Name must start with a letter and be ASCII
                    if (!((klassName[0] >= 'A' && klassName[0] <= 'Z') ||
                          (klassName[0] >= 'a' && klassName[0] <= 'z'))) continue;

                    // Cross-validate: PlayerCurrency.refrences (+0x38) should point back
                    // fields[4] = +0x38 = PR_Currency
                    uint64_t currencyPtr = fields[4]; // index 4 = (0x38-0x18)/8

                    bool crossOk = false;
                    if (IsValidUserPtr(currencyPtr) && currencyPtr != objAddr)
                    {
                        uint64_t backPtr = Read<uint64_t>(currencyPtr + PC_Refrences);
                        if (backPtr == objAddr)
                            crossOk = true;
                    }

                    // Validate Health has plausible values
                    // fields[1] = +0x20 = PR_Health
                    uint64_t healthPtr = fields[1];
                    bool healthOk = false;
                    if (IsValidUserPtr(healthPtr))
                    {
                        int32_t hp = Read<int32_t>(healthPtr + HP_Health);
                        int32_t maxHp = Read<int32_t>(healthPtr + HP_MaxHealth);
                        if (hp > 0 && hp <= 10000 && maxHp > 0 && maxHp <= 10000)
                            healthOk = true;
                    }

                    // ALWAYS require at least cross-validation OR health check
                    // (prevents accepting stale instances in menus)
                    if (!crossOk && !healthOk) continue;

                    candidates.push_back({ objAddr, validCount, crossOk });
                    DBG("[Player] Candidate at 0x%llX: %d ptrs, cross=%s, health=%s, klass='%s'\n",
                        (unsigned long long)objAddr, validCount,
                        crossOk ? "YES" : "no", healthOk ? "YES" : "no", klassName);
                }
            }
        }

        scanAddr = regionBase + regionSize;
        if (scanAddr <= (uint64_t)mbi.BaseAddress)
            break;
    }

    CloseHandle(hProc);

    // Pick the best candidate (prefer cross-validated)
    uint64_t bestAddr = 0;
    int bestScore = -1;
    for (auto& c : candidates)
    {
        int score = c.validPtrs + (c.crossValidated ? 100 : 0);
        if (score > bestScore)
        {
            bestScore = score;
            bestAddr = c.addr;
        }
    }

    if (!bestAddr)
    {
        DBG("[Player] No PlayerRefrences found on heap. Is a game loaded?\n");
        return false;
    }

    player.instance = bestAddr;

    // Learn the klass pointer from the found instance (for future fast scans)
    if (!player.klassPtr)
    {
        player.klassPtr = Read<uint64_t>(bestAddr + Obj_Klass);
        DBG("[Player] Learned klass pointer: 0x%llX\n", (unsigned long long)player.klassPtr);
    }

    // Log stable class metadata
    if (player.klassPtr)
    {
        uint32_t token    = Read<uint32_t>(player.klassPtr + Klass_Token);
        uint32_t instSize = Read<uint32_t>(player.klassPtr + Klass_InstanceSize);
        DBG("[Player] Klass: token=0x%08X instSize=0x%X %s\n",
            token, instSize,
            (token == PR_ExpectedToken && instSize == PR_ExpectedInstanceSize) ? "(OK)" : "(MISMATCH — update Offset.h!)");
    }

    offsetCache.Set("player_refrences", bestAddr);
    offsetCache.Set("player_klass", player.klassPtr);
    DBG("[+] PlayerRefrences found at 0x%llX (%d candidates)\n",
        (unsigned long long)bestAddr, (int)candidates.size());

    ResolvePlayerChains();
    return true;
}

// ---------------------------------------------------------------------------
// ResolvePlayerChains — follow PlayerRefrences → sub-objects
//
// Called once after scan and periodically in EntityLoop to keep fresh.
// ---------------------------------------------------------------------------

void Game::ResolvePlayerChains()
{
    using namespace Offset::IL2CPP;

    if (!player.instance) return;

    // Verify instance is still alive (klass pointer should match)
    uint64_t klass = Read<uint64_t>(player.instance + Obj_Klass);
    if (klass != player.klassPtr)
    {
        DBG("[Player] Instance invalidated (klass mismatch) — clearing.\n");
        player.instance = 0;
        cameraStructBase = 0;  // force camera re-probe on next resolve
        return;
    }

    player.health         = Read<uint64_t>(player.instance + PR_Health);
    player.weaponManager  = Read<uint64_t>(player.instance + PR_WeaponManager);
    player.cameraRotator  = Read<uint64_t>(player.instance + PR_CameraRotator);
    player.playerMovement = Read<uint64_t>(player.instance + PR_PlayerMovement);
    player.currency       = Read<uint64_t>(player.instance + PR_Currency);
    player.grenadeInv     = Read<uint64_t>(player.instance + PR_GrenadeInv);

    // Validate health — if sub-object pointers are dead, instance is stale
    if (!IsValidUserPtr(player.health) || !IsValidUserPtr(player.weaponManager))
    {
        DBG("[Player] Instance sub-pointers invalid — clearing.\n");
        player.instance = 0;
        cameraStructBase = 0;
        return;
    }

    // Chain to Camera.main via RotateCamera
    player.cameraManaged = 0;
    player.cameraNative  = 0;
    if (IsValidUserPtr(player.cameraRotator))
    {
        player.cameraManaged = Read<uint64_t>(player.cameraRotator + RC_MainCamera);
        if (IsValidUserPtr(player.cameraManaged))
            player.cameraNative = Read<uint64_t>(player.cameraManaged + CAM_CachedPtr);
    }

    // Set camera struct base from native pointer (hardcoded offsets for DFB v1.31)
    if (IsValidUserPtr(player.cameraNative))
    {
        if (cameraStructBase != player.cameraNative)
        {
            cameraStructBase = player.cameraNative;
            DBG("[Camera] Native camera set: 0x%llX (View=+0x%X, Proj=+0x%X)\n",
                (unsigned long long)player.cameraNative, cameraViewOffset, cameraProjOffset);
        }
    }
    else
    {
        cameraStructBase = 0;
    }
}

// ---------------------------------------------------------------------------
// RestoreAllPatches — undo all active patches before exit
// ---------------------------------------------------------------------------

void Game::RestoreAllPatches()
{
    for (int i = 0; i < PATCH_COUNT; i++)
    {
        if (g_Patches[i].active)
            TogglePatch(i, false);
    }
    DBG("[Trainer] All patches restored.\n");
}

// ---------------------------------------------------------------------------
// ResetState — full reset on game disconnect or scene change
// ---------------------------------------------------------------------------

// Bone probe state (defined earlier in file, reset here)
extern uint32_t g_ParentIndicesOff;
extern bool     g_ParentIndicesProbed;

void Game::ResetState()
{
    DBG("[State] Full reset\n");

    // Restore patches (best effort — process may be dead)
    RestoreAllPatches();

    // Reset trainer
    for (int i = 0; i < PATCH_COUNT; i++)
    {
        g_Patches[i].addr = 0;
        g_Patches[i].found = false;
        g_Patches[i].active = false;
    }
    trainerReady = false;

    // Reset player pointers
    {
        std::lock_guard<std::mutex> lock(playerMutex);
        player = {};
        cameraStructBase = 0;
    }

    // Reset VP
    vpAddress = 0;
    ViewProj = {};

    // Reset IL2CPP
    il2cpp = {};

    // Reset bone probe
    g_ParentIndicesOff = 0;
    g_ParentIndicesProbed = false;

    // Reset NPC list
    {
        std::lock_guard<std::mutex> lock(g_DataMutex);
        g_Players.clear();
    }

    // Reset process info
    GamePid = 0;
    GameBase = 0;
    GameAssemblyBase = 0;
    GameAssemblySize = 0;

    // Reset state flags
    gameConnected = false;
    inMatch = false;
}

// Global pointer for atexit/signal handlers
static Game* g_GameInstance = nullptr;

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    if (g_GameInstance)
        g_GameInstance->RestoreAllPatches();
    return FALSE;  // let default handler (exit) proceed
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

void Game::Start()
{
    g_GameInstance = this;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    CreateThread(NULL, 0, ThreadGameStart, (void*)this, 0, NULL);
}

// ---------------------------------------------------------------------------
// GameStart — Dead From Beyond adaptation
//
// No anti-cheat, no DLL injection, pure RPM.
// Find process, setup overlay, init trainer, start scanning.
// ---------------------------------------------------------------------------

void Game::GameStart()
{
    if (!MemoryBackendInit())
    {
        printf("[-] FATAL: MemoryBackendInit failed. Press Enter to exit.\n");
        getchar();
        ExitProcess(1);
    }

    // Start overlay immediately on desktop so user sees status
    Draw.GameWindow = GetDesktopWindow();
    Draw.Start();

    // Start RenderLoop early — shows "Waiting for Dead From Beyond..."
    CreateThread(NULL, 0, ThreadRenderLoop, (void*)this, 0, NULL);

    // === Phase 1: Wait for game process (blocking) ===
    std::string gameName = GetGameProcessName();
    DBG("[*] Waiting for %s...\n", gameName.c_str());

    while (true)
    {
        std::vector<DWORD> pidList = GetProcessPidList();
        for (size_t i = 0; i < pidList.size(); i++)
        {
            LPSTR szName = ProcessGetInformationString(pidList[i]);
            if (!szName) continue;
            if (strstr(szName, gameName.c_str()) != NULL)
            {
                GamePid = pidList[i];
                SetProcessPid(GamePid);
                GameBase = GetModuleFromName(gameName);
            }
            free(szName);
            if (GamePid != 0) break;
        }
        if (GamePid != 0 && GameBase != 0) break;
        GamePid = 0;
        GameBase = 0;
        Sleep(2000);
    }

    DBG("[+] Game PID: %d, Base: 0x%llX\n", GamePid, (unsigned long long)GameBase);

    // === Phase 2: Find game window ===
    for (int tries = 0; tries < 30; tries++)
    {
        HWND wnd = FindWindowByPID(GamePid);
        if (wnd) { Draw.GameWindow = wnd; break; }
        Sleep(500);
    }

    // === Phase 3: Wait for GameAssembly.dll ===
    for (int tries = 0; tries < 60; tries++)
    {
        GameAssemblyBase = GetModuleFromName("GameAssembly.dll");
        GameAssemblySize = GetModuleSize("GameAssembly.dll");
        if (GameAssemblyBase) break;
        Sleep(500);
    }

    // === Phase 4: Full init ===
    if (GameAssemblyBase)
    {
        DBG("[+] GameAssembly.dll: 0x%llX (%llu MB)\n",
            (unsigned long long)GameAssemblyBase,
            (unsigned long long)(GameAssemblySize / (1024*1024)));

        uint64_t modSize = GameAssemblySize;
        bool cacheLoaded = offsetCache.Load();
        if (cacheLoaded && offsetCache.IsStale(modSize))
            offsetCache.InvalidateAll();
        offsetCache.SetModuleSize(modSize);

        InitTrainer();
        InitIL2CPPPointers();
        // VP scan skipped at startup — useless in menus, camera path preferred.
        // RenderLoop VP recovery handles re-scan when needed in-match.
        ScanForPlayerRefrences();
        offsetCache.Save();
    }
    else
    {
        DBG("[!] GameAssembly.dll not loaded after 30s\n");
    }

    gameConnected = true;

    // === Phase 5: Start EntityLoop AFTER full init ===
    CreateThread(NULL, 0, ThreadEntityLoop, (void*)this, 0, NULL);
}

// ---------------------------------------------------------------------------
// ScanForVPMatrix — automatic VP matrix finder (no CE needed)
//
// Unity IL2CPP: the VP matrix (View * Projection) is computed each frame
// and stored in CPU-accessible memory for rendering.
//
// Key insight: VP matrix has PROJECTION SCALING that distinguishes it from
// a plain View matrix. View matrices have rotation elements all <= 1.0.
// VP matrices have elements > 1.0 from FOV/aspect scaling.
//
// Phase A: Scan all readable memory at 16-byte alignment
//   - Fast reject (NaN, extremes, all same)
//   - Structure check (enough non-zero, has negatives, good range)
//   - Perspective column (camera forward encoded in column 3)
//   - Projection scaling (some elements > 1.0 in the 3x3 block)
//
// Phase B: Dynamic test (3 samples over 2 seconds — MOVE THE CAMERA!)
//   - Real VP changes when camera rotates
//   - Must change in at least 2 of 3 samples
//
// Phase C: Score and select best candidate
// ---------------------------------------------------------------------------

bool Game::ScanForVPMatrix()
{
    HANDLE hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, GamePid);
    if (!hProc)
        return false;

    const size_t ALIGN     = 4;     // 4-byte alignment (Unity field offset can be any multiple of 4)
    const size_t CHUNK_SZ  = 4 * 1024 * 1024;
    const int    MAX_CANDIDATES = 500;

    struct VPCandidate {
        uint64_t addr;
        float    data[16];
        float    score;         // higher = more VP-like
        int      dynamicHits;   // how many freshness samples showed change
        bool     isTransposed;
    };
    std::vector<VPCandidate> candidates;

    MEMORY_BASIC_INFORMATION mbi;
    uint64_t addr = 0x10000;

    DBG("[VP] === Automatic VP Matrix Scan ===\n");
    DBG("[VP] Scanning all readable memory (16-byte align)...\n");
    DBG("[VP] TIP: Move the camera during scan for best results!\n");

    while (addr < 0x7FFFFFFFFFFFULL && (int)candidates.size() < MAX_CANDIDATES)
    {
        if (VirtualQueryEx(hProc, (LPCVOID)addr, &mbi, sizeof(mbi)) == 0)
            break;

        uint64_t regionBase = (uint64_t)mbi.BaseAddress;
        size_t   regionSize = mbi.RegionSize;
        uint64_t regionEnd  = regionBase + regionSize;

        bool readable = (mbi.State == MEM_COMMIT) &&
                        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE)) &&
                        !(mbi.Protect & PAGE_GUARD);

        if (readable && regionSize >= 64)
        {
            for (size_t off = 0; off + 64 <= regionSize && (int)candidates.size() < MAX_CANDIDATES; off += CHUNK_SZ)
            {
                size_t readSz = min(CHUNK_SZ, regionSize - off);
                if (readSz < 64) break;

                std::vector<uint8_t> chunk(readSz);
                SIZE_T bytesRead = 0;
                if (!ReadProcessMemory(hProc, (LPCVOID)(regionBase + off), chunk.data(), readSz, &bytesRead))
                    continue;
                if (bytesRead < 64) continue;

                for (size_t i = 0; i + 64 <= bytesRead && (int)candidates.size() < MAX_CANDIDATES; i += ALIGN)
                {
                    const float* vp = (const float*)&chunk[i];

                    // --- Quick reject ---
                    if (vp[0] != vp[0] || fabsf(vp[0]) < 0.001f)
                        continue;
                    if (vp[0] == vp[1] && vp[1] == vp[2])
                        continue;

                    // --- Full validation ---
                    int nonzero = 0, negatives = 0, bigValues = 0;
                    float minVal = vp[0], maxVal = vp[0];
                    bool valid = true;

                    for (int j = 0; j < 16; j++)
                    {
                        float v = vp[j];
                        if (v != v) { valid = false; break; }              // NaN
                        if (fabsf(v) > 50000.f) { valid = false; break; }  // extreme
                        if (fabsf(v) > 0.001f) nonzero++;
                        if (v < -0.01f) negatives++;
                        if (v < minVal) minVal = v;
                        if (v > maxVal) maxVal = v;
                    }
                    if (!valid || nonzero < 10 || negatives < 2)
                        continue;
                    if ((maxVal - minVal) < 1.0f)
                        continue;

                    // --- Perspective column check (both conventions) ---
                    // Row-major (pos*M): perspective in column 3 → vp[3], vp[7], vp[11]
                    float perspA = vp[3]*vp[3] + vp[7]*vp[7] + vp[11]*vp[11];
                    // Column-major (M*pos): perspective in row 3 → vp[12], vp[13], vp[14]
                    float perspB = vp[12]*vp[12] + vp[13]*vp[13] + vp[14]*vp[14];

                    bool perspOK_A = (perspA >= 0.7f && perspA <= 1.4f);
                    bool perspOK_B = (perspB >= 0.7f && perspB <= 1.4f);
                    if (!perspOK_A && !perspOK_B)
                        continue;

                    // --- Projection scaling check ---
                    // VP has FOV/aspect scaling → some 3x3 elements have |value| > 1.05
                    // Pure View matrix has all rotation elements <= 1.0
                    int scaledCount = 0;
                    int rotIndices[] = {0,1,2, 4,5,6, 8,9,10};
                    for (int ri : rotIndices)
                        if (fabsf(vp[ri]) > 1.05f) scaledCount++;

                    if (scaledCount < 1)
                        continue;   // Looks like a pure View matrix, not VP

                    // --- Zero row check ---
                    // VP should NOT have an entire row of zeros
                    bool hasZeroRow = false;
                    for (int r = 0; r < 4; r++)
                    {
                        if (fabsf(vp[r*4]) < 0.001f && fabsf(vp[r*4+1]) < 0.001f &&
                            fabsf(vp[r*4+2]) < 0.001f && fabsf(vp[r*4+3]) < 0.001f)
                            hasZeroRow = true;
                    }
                    if (hasZeroRow) continue;

                    // --- Row diversity check ---
                    // A real VP matrix has VARIED values per row (rotation + projection).
                    // Reject matrices where any row has all ~identical values
                    // (kills constant-fill buffers, stack garbage, etc.)
                    bool rowsDiverse = true;
                    for (int r = 0; r < 4; r++)
                    {
                        float rv[4] = { vp[r*4], vp[r*4+1], vp[r*4+2], vp[r*4+3] };
                        float rowMin = rv[0], rowMax = rv[0];
                        for (int c = 1; c < 4; c++) {
                            if (rv[c] < rowMin) rowMin = rv[c];
                            if (rv[c] > rowMax) rowMax = rv[c];
                        }
                        if ((rowMax - rowMin) < 0.02f) { rowsDiverse = false; break; }
                    }
                    if (!rowsDiverse) continue;

                    // --- Column diversity check ---
                    // Same for columns — reject if any column is constant
                    bool colsDiverse = true;
                    for (int c = 0; c < 4; c++)
                    {
                        float cv[4] = { vp[c], vp[4+c], vp[8+c], vp[12+c] };
                        float colMin = cv[0], colMax = cv[0];
                        for (int r = 1; r < 4; r++) {
                            if (cv[r] < colMin) colMin = cv[r];
                            if (cv[r] > colMax) colMax = cv[r];
                        }
                        if ((colMax - colMin) < 0.02f) { colsDiverse = false; break; }
                    }
                    if (!colsDiverse) continue;

                    // --- Orthogonality + magnitude check ---
                    // VP = View * Projection. The first 3 rows (row-major) or
                    // columns (column-major) represent camera axes scaled by projection.
                    // They must be:
                    //   1. Nearly orthogonal (dot < 0.15)
                    //   2. Magnitude 0.1-5.0 (projection scaling is bounded)
                    //
                    // Test BOTH conventions: pass if either rows or columns are orthogonal.

                    auto checkOrthoMag = [&](bool useColumns) -> bool {
                        float v[3][3]; // 3 vectors, each 3D
                        for (int i = 0; i < 3; i++)
                            for (int j = 0; j < 3; j++)
                                v[i][j] = useColumns ? vp[j*4+i] : vp[i*4+j];

                        // Check magnitudes (0.1-5.0 for VP projection scaling)
                        for (int i = 0; i < 3; i++) {
                            float mag = sqrtf(v[i][0]*v[i][0] + v[i][1]*v[i][1] + v[i][2]*v[i][2]);
                            if (mag < 0.1f || mag > 5.0f) return false;
                        }

                        // Check pairwise orthogonality (|dot| < 0.15 for all pairs)
                        for (int a = 0; a < 3; a++) {
                            for (int b = a+1; b < 3; b++) {
                                float lenA = sqrtf(v[a][0]*v[a][0] + v[a][1]*v[a][1] + v[a][2]*v[a][2]);
                                float lenB = sqrtf(v[b][0]*v[b][0] + v[b][1]*v[b][1] + v[b][2]*v[b][2]);
                                if (lenA < 0.001f || lenB < 0.001f) return false;
                                float dot = (v[a][0]*v[b][0] + v[a][1]*v[b][1] + v[a][2]*v[b][2]) / (lenA*lenB);
                                if (fabsf(dot) > 0.15f) return false;
                            }
                        }
                        return true;
                    };

                    bool rowMajorOK = checkOrthoMag(false);  // check rows
                    bool colMajorOK = checkOrthoMag(true);   // check columns
                    if (!rowMajorOK && !colMajorOK) continue;

                    // --- Score the candidate ---
                    float score = 0.f;

                    // Perspective column closeness to magnitude 1.0
                    if (perspOK_A) score += 50.f * (1.f - fabsf(perspA - 1.0f));
                    if (perspOK_B) score += 50.f * (1.f - fabsf(perspB - 1.0f));

                    // Projection scaling (more scaled elements = more VP-like)
                    score += scaledCount * 10.f;

                    // Prefer matrices with good element diversity
                    score += (float)nonzero * 2.f;
                    score += (float)negatives * 3.f;

                    VPCandidate cand;
                    cand.addr = regionBase + off + i;
                    memcpy(cand.data, vp, 64);
                    cand.score = score;
                    cand.dynamicHits = 0;
                    cand.isTransposed = colMajorOK && !rowMajorOK;
                    candidates.push_back(cand);
                }
            }
        }

        addr = regionEnd;
        if (addr <= regionBase) break;
    }

    if (candidates.empty())
    {
        DBG("[VP] Phase A: No VP candidates found.\n");
        CloseHandle(hProc);
        return false;
    }

    DBG("[VP] Phase A: %d VP candidates found. Testing dynamics...\n",
        (int)candidates.size());

    // --- Phase B: Dynamic test (3 reads over 2 seconds) ---
    // The real VP matrix updates every frame when the camera moves.
    // We take 3 snapshots and count how many times each candidate changed.
    for (int sample = 0; sample < 3; sample++)
    {
        Sleep(700);

        for (auto& cand : candidates)
        {
            if (cand.score < 0) continue;  // already invalidated

            float reread[16] = {};
            SIZE_T br = 0;
            if (!ReadProcessMemory(hProc, (LPCVOID)cand.addr, reread, 64, &br) || br != 64)
            {
                cand.score = -1;  // invalidate: unreadable
                continue;
            }

            // Re-validate: check for NaN and constant rows after re-read
            bool rereadOK = true;
            for (int j = 0; j < 16; j++)
            {
                if (reread[j] != reread[j]) { rereadOK = false; break; }  // NaN
                if (fabsf(reread[j]) > 50000.f) { rereadOK = false; break; }
            }
            if (rereadOK)
            {
                // Re-check row diversity on the fresh data
                for (int r = 0; r < 4 && rereadOK; r++)
                {
                    float rMin = reread[r*4], rMax = reread[r*4];
                    for (int c = 1; c < 4; c++) {
                        if (reread[r*4+c] < rMin) rMin = reread[r*4+c];
                        if (reread[r*4+c] > rMax) rMax = reread[r*4+c];
                    }
                    if ((rMax - rMin) < 0.02f) rereadOK = false;
                }
            }
            if (!rereadOK)
            {
                cand.score = -1;  // invalidate
                continue;
            }

            bool changed = false;
            for (int j = 0; j < 16; j++)
            {
                if (fabsf(reread[j] - cand.data[j]) > 0.0001f)
                {
                    changed = true;
                    break;
                }
            }

            if (changed)
            {
                cand.dynamicHits++;
                memcpy(cand.data, reread, 64);
            }
        }
    }

    // Remove invalidated candidates
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
                       [](const VPCandidate& c) { return c.score < 0; }),
        candidates.end());

    // Count dynamics
    int dynamicCount = 0;
    for (auto& c : candidates)
        if (c.dynamicHits >= 2) dynamicCount++;

    DBG("[VP] Phase B: %d dynamic (changed 2+ times), %d static\n",
        dynamicCount, (int)candidates.size() - dynamicCount);

    // --- Phase C: Score and select ---
    // Dynamic bonus: +200 for 3 hits, +100 for 2 hits
    for (auto& c : candidates)
    {
        if (c.dynamicHits >= 3) c.score += 200.f;
        else if (c.dynamicHits >= 2) c.score += 100.f;
        else if (c.dynamicHits >= 1) c.score += 30.f;
    }

    // Sort by score descending
    std::sort(candidates.begin(), candidates.end(),
              [](const VPCandidate& a, const VPCandidate& b) {
                  return a.score > b.score;
              });

    // Log top 5
    int logN = min(5, (int)candidates.size());
    DBG("[VP] Top %d candidates:\n", logN);
    for (int i = 0; i < logN; i++)
    {
        auto& c = candidates[i];
        DBG("[VP]   #%d: 0x%llX  score=%.0f  dynamic=%d  %s\n",
            i + 1, (unsigned long long)c.addr, c.score, c.dynamicHits,
            c.isTransposed ? "col-major" : "row-major");
    }

    // Pick the best
    if (candidates[0].score < 50.f)
    {
        DBG("[VP] Best candidate score too low (%.0f). No VP found.\n", candidates[0].score);
        CloseHandle(hProc);
        return false;
    }

    vpAddress = candidates[0].addr;
    vpIsTransposed = candidates[0].isTransposed;

    // Re-read the winner for clean dump
    float final[16] = {};
    ReadProcessMemory(hProc, (LPCVOID)vpAddress, final, 64, nullptr);

    DBG("[VP] === Winner: 0x%llX (score=%.0f, dynamic=%d, %s) ===\n",
        (unsigned long long)vpAddress, candidates[0].score,
        candidates[0].dynamicHits,
        vpIsTransposed ? "COLUMN-MAJOR (M*pos)" : "ROW-MAJOR (pos*M)");

    DBG("[VP]   [%10.4f %10.4f %10.4f %10.4f]\n", final[0],  final[1],  final[2],  final[3]);
    DBG("[VP]   [%10.4f %10.4f %10.4f %10.4f]\n", final[4],  final[5],  final[6],  final[7]);
    DBG("[VP]   [%10.4f %10.4f %10.4f %10.4f]\n", final[8],  final[9],  final[10], final[11]);
    DBG("[VP]   [%10.4f %10.4f %10.4f %10.4f]\n", final[12], final[13], final[14], final[15]);

    CloseHandle(hProc);
    return true;
}

// ---------------------------------------------------------------------------
// ReadMVP — read the ViewProjection matrix
// ---------------------------------------------------------------------------

bool Game::ReadMVP()
{
    // Read View + Projection from native Camera and compute VP = P * V
    if (cameraStructBase)
    {
        float V[16], P[16];
        if (ReadMemory(cameraStructBase + cameraViewOffset, V, 64) &&
            ReadMemory(cameraStructBase + cameraProjOffset, P, 64))
        {
            // Column-major multiply: VP = P * V
            // VP[col*4+row] = sum_k P[k*4+row] * V[col*4+k]
            float vp[16];
            for (int col = 0; col < 4; col++)
                for (int row = 0; row < 4; row++)
                {
                    float sum = 0;
                    for (int k = 0; k < 4; k++)
                        sum += P[k * 4 + row] * V[col * 4 + k];
                    vp[col * 4 + row] = sum;
                }

            memcpy(&ViewProj, vp, 64);
            vpIsTransposed = false;  // memcpy stores VP^T; D3D row-vector code then does clip = VP * pos correctly
            return true;
        }
    }

    // Fallback: read pre-computed VP from vpAddress (legacy scanner)
    if (vpAddress)
    {
        Matrix4x4 newVP;
        if (ReadMemory(vpAddress, &newVP, sizeof(Matrix4x4)))
        {
            if (newVP.m[0][0] != 0.f && newVP.m[0][0] == newVP.m[0][0] &&
                !(newVP.m[0][0] == 1.f && newVP.m[1][1] == 1.f && newVP.m[2][2] == 1.f))
            {
                ViewProj = newVP;
                return true;
            }
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// WorldToScreen — ViewProjection matrix multiply (XYZ, confirmed working)
// ---------------------------------------------------------------------------

bool Game::WorldToScreen(const Vector3& world, Vector2& screen, float* outDist)
{
    float wx = world.x;
    float wy = world.y;
    float wz = world.z;

    float x, y, w;
    if (vpIsTransposed)
    {
        // Column-vector convention: clip = M * pos
        x = ViewProj.m[0][0]*wx + ViewProj.m[0][1]*wy + ViewProj.m[0][2]*wz + ViewProj.m[0][3];
        y = ViewProj.m[1][0]*wx + ViewProj.m[1][1]*wy + ViewProj.m[1][2]*wz + ViewProj.m[1][3];
        w = ViewProj.m[3][0]*wx + ViewProj.m[3][1]*wy + ViewProj.m[3][2]*wz + ViewProj.m[3][3];
    }
    else
    {
        // Row-vector convention: clip = pos * M (D3D standard)
        x = wx * ViewProj.m[0][0] + wy * ViewProj.m[1][0] + wz * ViewProj.m[2][0] + ViewProj.m[3][0];
        y = wx * ViewProj.m[0][1] + wy * ViewProj.m[1][1] + wz * ViewProj.m[2][1] + ViewProj.m[3][1];
        w = wx * ViewProj.m[0][3] + wy * ViewProj.m[1][3] + wz * ViewProj.m[2][3] + ViewProj.m[3][3];

        // Detect affine matrix (View*FOV without perspective divide):
        // Column 3 = (0,0,0,1) means W is always 1.
        // In that case, use Z (depth) as the perspective divisor instead.
        if (fabsf(w - 1.0f) < 0.01f)
        {
            float z = wx * ViewProj.m[0][2] + wy * ViewProj.m[1][2] + wz * ViewProj.m[2][2] + ViewProj.m[3][2];
            w = z;  // perspective divide by depth
        }
    }

    if (w < 0.001f)
        return false;

    float ndcX = x / w;
    float ndcY = y / w;

    float sw = (float)ScreenW;
    float sh = (float)ScreenH;
    if (sw <= 0) sw = (float)Draw.GameCenterW;
    if (sh <= 0) sh = (float)Draw.GameCenterH;

    screen.x = (ndcX + 1.f) * 0.5f * sw;
    screen.y = (1.f - ndcY) * 0.5f * sh;

    if (outDist)
        *outDist = w;  // approximate depth distance

    return (screen.x >= 0 && screen.x <= sw && screen.y >= 0 && screen.y <= sh);
}

// ---------------------------------------------------------------------------
// DrawPlayer — render ESP for one entity
// ---------------------------------------------------------------------------

bool Game::DrawPlayer(const Player& p)
{
    Vector2 posScreen;
    float dist = 0.f;

    if (!WorldToScreen(p.Position, posScreen, &dist))
        return false;

    if (dist > g_Settings.esp.maxDistance)
        return false;

    int color = (dist < 50.f) ? g_Settings.esp.colorClose : g_Settings.esp.colorEnemy;

    float dotSize = std::clamp(6.f - dist / 100.f, 2.f, 6.f);
    Draw.DrawCircleFilled((int)posScreen.x, (int)posScreen.y, dotSize, color, 8, 220);

    if (g_Settings.esp.showDistance)
    {
        Draw.DrawNewText((int)posScreen.x + 8, (int)posScreen.y - 6, COLOR_WHITE, 12.f,
                         "%.0f", dist);
    }

    return true;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// ReadWorldPosition — get world position from a native Transform via hierarchy
//
// Reversed from UnityPlayer.dll Transform::get_position_Injected:
//   hierarchy_ptr = *(nativeTransform + 0x38)
//   index         = *(nativeTransform + 0x40)
//   data_base     = *(hierarchy_ptr + 0x18)
//   worldPos      = *(data_base + index * 48 + 0x20)
// ---------------------------------------------------------------------------

// ReadWorldPosition — read world position from a native Component that has TransformAccess
//
// TransformAccess is at +0x38 in any native built-in Component (Camera, Transform, etc.)
// Formula from UnityPlayer.dll reverse:
//   hierarchy_ptr = *(comp + 0x38)
//   index         = *(comp + 0x40) & 0xFFFFFFFF
//   data_base     = *(hierarchy_ptr + 0x18)
//   world_pos     = *(data_base + index * 48 + 0x20)

static bool ReadWorldPosition(uint64_t nativeComp, Vector3& out)
{
    using namespace Offset::Transform;

    uint64_t hierarchyPtr = Read<uint64_t>(nativeComp + TA_HierarchyPtr);
    if (!IsValidUserPtr(hierarchyPtr))
        return false;

    int32_t index = Read<int32_t>(nativeComp + TA_Index);
    if (index < 0 || index > 100000)
        return false;

    uint64_t dataBase = Read<uint64_t>(hierarchyPtr + Hierarchy_DataPtr);
    if (!IsValidUserPtr(dataBase))
        return false;

    uint64_t posAddr = dataBase + (uint64_t)index * Entry_Stride + Entry_LocalPos;

    float xyz[3];
    if (!ReadMemory(posAddr, xyz, 12))
        return false;

    if (xyz[0] != xyz[0] || xyz[1] != xyz[1] || xyz[2] != xyz[2])
        return false;

    out.x = xyz[0];
    out.y = xyz[1];
    out.z = xyz[2];
    return true;
}

// ---------------------------------------------------------------------------
// GetNativeTransform — managed MonoBehaviour → native Transform
//
// Path: managed_obj → m_CachedPtr (+0x10) → native Component
//       → native GameObject (+0x30) → native Transform (+0x30)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// GetNativeTransform — find the native Transform component for a managed object
//
// For MonoBehaviour scripts (NPC, etc.), m_CachedPtr does NOT have TransformAccess.
// We must navigate: managed → nativeComp → nativeGO → component list → Transform.
//
// The GO component list is at *(nativeGO + 0x30). Each entry is 16 bytes:
//   {int32 type_lo, int32 type_hi, ptr component}
// The native Transform is the FIRST component (type_lo = 0x75).
// TransformAccess at Transform+0x38 has a valid hierarchy pointer.
// ---------------------------------------------------------------------------

static uint64_t GetNativeTransform(uint64_t managedObj)
{
    using namespace Offset::Transform;

    uint64_t nativeComp = Read<uint64_t>(managedObj + 0x10);  // m_CachedPtr
    if (!IsValidUserPtr(nativeComp))
        return 0;

    // Check if nativeComp itself has valid TransformAccess at +0x38
    // (works for built-in components like Camera, and for native Transform itself)
    uint64_t directHPtr = Read<uint64_t>(nativeComp + TA_HierarchyPtr);
    if (IsValidUserPtr(directHPtr))
    {
        int32_t directIdx = Read<int32_t>(nativeComp + TA_Index);
        if (directIdx >= 0 && directIdx < 100000)
        {
            uint64_t directData = Read<uint64_t>(directHPtr + Hierarchy_DataPtr);
            if (IsValidUserPtr(directData))
                return nativeComp;  // This component HAS TransformAccess (Camera, Transform, etc.)
        }
    }

    // MonoBehaviour path: nativeComp+0x30 → nativeGO → +0x30 → component list
    uint64_t nativeGO = Read<uint64_t>(nativeComp + NativeComp_GO);
    if (!IsValidUserPtr(nativeGO))
        return 0;

    uint64_t compListBase = Read<uint64_t>(nativeGO + 0x30);
    if (!IsValidUserPtr(compListBase))
        return 0;

    // Scan first 12 component entries (16 bytes each: {8B typeData, 8B ptr})
    for (int e = 0; e < 12; e++)
    {
        uint64_t compPtr = Read<uint64_t>(compListBase + e * 16 + 0x08);
        if (!IsValidUserPtr(compPtr))
            continue;

        // Check if this component has valid TransformAccess
        uint64_t hPtr = Read<uint64_t>(compPtr + TA_HierarchyPtr);
        if (!IsValidUserPtr(hPtr))
            continue;

        int32_t idx = Read<int32_t>(compPtr + TA_Index);
        if (idx < 0 || idx > 100000)
            continue;

        uint64_t dBase = Read<uint64_t>(hPtr + Hierarchy_DataPtr);
        if (IsValidUserPtr(dBase))
            return compPtr;  // Found native Transform with valid hierarchy
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Bone world position — parent chain walk with quaternion math
//
// Unity's hierarchy stores LOCAL TRS at hierarchy+0x18 (stride 48):
//   entry = {localPos(+0x00), localRot(+0x10), localScale(+0x20)}
// There is NO separate world position array (confirmed by disassembly).
//
// To get world position for a child transform (idx>0), we walk the parent
// chain up to root, compositing local transforms via quaternion rotation.
//
// Parent indices are in a int32[] array at some hierarchy offset (probed).
// ---------------------------------------------------------------------------

// Cached hierarchy offset for parent index array (0 = not found)
uint32_t g_ParentIndicesOff = 0;
bool     g_ParentIndicesProbed = false;

static bool ProbeParentIndices(uint64_t hierarchyPtr, int32_t childIndex)
{
    if (childIndex <= 0) return false;
    if (g_ParentIndicesProbed) return (g_ParentIndicesOff != 0);
    g_ParentIndicesProbed = true;

    for (uint32_t off = 0x00; off <= 0x80; off += 0x08)
    {
        if (off == 0x18) continue;  // local TRS data

        uint64_t arrPtr = Read<uint64_t>(hierarchyPtr + off);
        if (!IsValidUserPtr(arrPtr)) continue;

        // Walk up from childIndex — each parent should have a LOWER index
        int32_t cur = childIndex;
        bool valid = true;
        int steps = 0;
        while (cur > 0 && steps < 30)
        {
            int32_t parent = Read<int32_t>(arrPtr + (uint64_t)cur * 4);
            if (parent < 0 || parent >= cur) { valid = false; break; }
            cur = parent;
            steps++;
        }
        if (!valid || cur != 0) continue;  // didn't reach root

        g_ParentIndicesOff = off;
        DBG("[Bones] Parent indices at hierarchy+0x%X (walked %d steps from idx %d to root)\n",
            off, steps, childIndex);
        return true;
    }

    DBG("[Bones] Parent indices NOT found (tried +0x00..+0x80)\n");
    return false;
}

// Quaternion-rotate a vector: v' = q * v * q^-1
static inline void QuatRotateVec(const float q[4], const float v[3], float out[3])
{
    float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    float tx = 2.f * (qy * v[2] - qz * v[1]);
    float ty = 2.f * (qz * v[0] - qx * v[2]);
    float tz = 2.f * (qx * v[1] - qy * v[0]);
    out[0] = v[0] + qw * tx + (qy * tz - qz * ty);
    out[1] = v[1] + qw * ty + (qz * tx - qx * tz);
    out[2] = v[2] + qw * tz + (qx * ty - qy * tx);
}

// Quaternion multiply: out = a * b
static inline void QuatMul(const float a[4], const float b[4], float out[4])
{
    out[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    out[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    out[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    out[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}

// Compute world position by walking the parent chain
static bool ComputeBoneWorldPos(uint64_t hierarchyPtr, int32_t boneIndex, Vector3& out)
{
    if (g_ParentIndicesOff == 0 || boneIndex <= 0) return false;

    uint64_t parentArr = Read<uint64_t>(hierarchyPtr + g_ParentIndicesOff);
    uint64_t dataBase  = Read<uint64_t>(hierarchyPtr + Offset::Transform::Hierarchy_DataPtr);
    if (!IsValidUserPtr(parentArr) || !IsValidUserPtr(dataBase)) return false;

    // Collect chain from bone to root (max 30 levels)
    struct LocalTRS { float pos[3]; float rot[4]; };
    LocalTRS chain[30];
    int chainLen = 0;
    int32_t cur = boneIndex;

    while (cur > 0 && chainLen < 30)
    {
        uint64_t entry = dataBase + (uint64_t)cur * 48;
        if (!ReadMemory(entry, chain[chainLen].pos, 12)) return false;
        if (!ReadMemory(entry + 0x10, chain[chainLen].rot, 16)) return false;
        chainLen++;
        cur = Read<int32_t>(parentArr + (uint64_t)cur * 4);
        if (cur < 0) return false;
    }

    // Read root TRS (idx 0): localPos = worldPos, localRot = worldRot
    float wp[3], wr[4];
    if (!ReadMemory(dataBase, wp, 12)) return false;
    if (!ReadMemory(dataBase + 0x10, wr, 16)) return false;

    // Apply chain from root down to bone
    for (int i = chainLen - 1; i >= 0; i--)
    {
        float rotated[3];
        QuatRotateVec(wr, chain[i].pos, rotated);
        wp[0] += rotated[0];
        wp[1] += rotated[1];
        wp[2] += rotated[2];

        float newRot[4];
        QuatMul(wr, chain[i].rot, newRot);
        wr[0] = newRot[0]; wr[1] = newRot[1];
        wr[2] = newRot[2]; wr[3] = newRot[3];
    }

    out.x = wp[0]; out.y = wp[1]; out.z = wp[2];
    return true;
}

// ---------------------------------------------------------------------------
// GetHeadPosition — find head bone world position for a zombie NPC
//
// Chain: NPC → ragdollController (+0x98) → transforms (+0x20) → Transform[]
// For each bone: managed → native → hierarchy → parent chain walk → worldPos
// Head = bone with highest world Y.
// ---------------------------------------------------------------------------

// Try reading world position of a managed Transform via parent chain walk
static bool ReadManagedTransformWorldPos(uint64_t managedTransform, Vector3& out, bool probe)
{
    using namespace Offset::Transform;

    uint64_t nativeComp = Read<uint64_t>(managedTransform + 0x10);  // m_CachedPtr
    if (!IsValidUserPtr(nativeComp)) return false;

    uint64_t hPtr = Read<uint64_t>(nativeComp + TA_HierarchyPtr);
    int32_t  idx  = Read<int32_t>(nativeComp + TA_Index);
    if (!IsValidUserPtr(hPtr) || idx < 0) return false;

    if (idx == 0)
        return ReadWorldPosition(nativeComp, out);

    if (!g_ParentIndicesProbed && probe)
        ProbeParentIndices(hPtr, idx);

    return ComputeBoneWorldPos(hPtr, idx, out);
}

static bool GetHeadPosition(uint64_t npc, const Vector3& rootPos, Vector3& headOut, bool probeBones)
{
    using namespace Offset;

    // === Strategy 1: NPC.eyeSight Transform (+0xA0) ===
    // This is placed at the zombie's eyes — best head approximation.
    uint64_t eyeSight = Read<uint64_t>(npc + NPC::EyeSight);
    if (IsValidUserPtr(eyeSight))
    {
        Vector3 eyePos;
        if (ReadManagedTransformWorldPos(eyeSight, eyePos, probeBones))
        {
            float dist = rootPos.Distance(eyePos);
            if (dist > 0.5f && dist < 3.0f && eyePos.y > rootPos.y + 0.5f)
            {
                headOut = eyePos;
                return true;
            }
        }
    }

    // === Strategy 2: highest centered ragdoll bone + 0.3m offset ===
    // Ragdoll transforms don't include head (common Unity setup).
    // The neck/upper-chest is the highest bone — add ~0.3m for head.
    uint64_t ragdollCtrl = Read<uint64_t>(npc + NPC::RagdollController);
    if (!IsValidUserPtr(ragdollCtrl)) return false;

    uint64_t transformsArr = Read<uint64_t>(ragdollCtrl + RagdollCtrl::Transforms);
    if (!IsValidUserPtr(transformsArr)) return false;

    uint64_t arrLen = Read<uint64_t>(transformsArr + IL2CppArray::MaxLength);
    if (arrLen == 0 || arrLen > 100) return false;

    int boneCount = (int)min(arrLen, (uint64_t)30);

    // Read all bone world positions
    struct BoneInfo { Vector3 pos; bool valid; };
    BoneInfo bones[30] = {};

    for (int i = 0; i < boneCount; i++)
    {
        uint64_t boneManaged = Read<uint64_t>(transformsArr + IL2CppArray::FirstElem + i * IL2CppArray::ElemStride);
        if (!IsValidUserPtr(boneManaged)) continue;

        Vector3 bonePos;
        if (!ReadManagedTransformWorldPos(boneManaged, bonePos, probeBones)) continue;

        float dist = rootPos.Distance(bonePos);
        if (dist > 4.0f) continue;

        bones[i].pos = bonePos;
        bones[i].valid = true;
    }

    // Debug: log bone positions once per session/reconnect
    static bool loggedOnce = false;
    if (g_ParentIndicesOff == 0) loggedOnce = false;  // reset on bone probe reset
    if (!loggedOnce)
    {
        loggedOnce = true;
        DBG("[Bones] Bone dump for NPC at (%.1f, %.1f, %.1f):\n", rootPos.x, rootPos.y, rootPos.z);
        for (int i = 0; i < boneCount; i++)
            if (bones[i].valid)
                DBG("[Bones]   [%2d] dY=%.2f hDist=%.2f\n", i,
                    bones[i].pos.y - rootPos.y,
                    sqrtf((bones[i].pos.x-rootPos.x)*(bones[i].pos.x-rootPos.x) +
                          (bones[i].pos.z-rootPos.z)*(bones[i].pos.z-rootPos.z)));
    }

    // Find highest centered bone (neck) and add 0.3m for head
    float maxBoneY = -99999.f;
    for (int i = 0; i < boneCount; i++)
        if (bones[i].valid && bones[i].pos.y > maxBoneY)
            maxBoneY = bones[i].pos.y;

    if (maxBoneY < rootPos.y) return false;

    float bestHDist = 99999.f;
    Vector3 neckPos = {};
    bool found = false;

    for (int i = 0; i < boneCount; i++)
    {
        if (!bones[i].valid) continue;
        if (bones[i].pos.y < maxBoneY - 0.3f) continue;

        float hdx = bones[i].pos.x - rootPos.x;
        float hdz = bones[i].pos.z - rootPos.z;
        float hDist = sqrtf(hdx * hdx + hdz * hdz);

        if (hDist < bestHDist)
        {
            bestHDist = hDist;
            neckPos = bones[i].pos;
            found = true;
        }
    }

    if (found)
    {
        // Neck → head: add ~0.3m upward
        headOut = neckPos + Vector3(0, 0.3f, 0);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// EntityLoop — background thread: enumerate NPCs via AIManager + hierarchy
// ---------------------------------------------------------------------------

void Game::EntityLoop()
{
    DBG("[+] EntityLoop started\n");

    while (true)
    {
        Sleep(80);

        // === Check if game process is alive ===
        if (GamePid != 0)
        {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GamePid);
            if (!hProc)
            {
                DBG("[State] Game process lost (PID %d) — shutting down\n", GamePid);
                ResetState();
                DBG("[*] Game closed. Exiting Phantom.\n");
                ExitProcess(0);
            }
            else
            {
                CloseHandle(hProc);
            }
        }

        // === Late init: GameAssembly.dll might not be loaded at startup ===
        if (!GameAssemblyBase)
        {
            GameAssemblyBase = GetModuleFromName("GameAssembly.dll");
            GameAssemblySize = GetModuleSize("GameAssembly.dll");
            if (GameAssemblyBase)
            {
                DBG("[+] GameAssembly.dll loaded (late): 0x%llX\n", (unsigned long long)GameAssemblyBase);
                uint64_t modSize = GameAssemblySize;
                offsetCache.Load();
                offsetCache.SetModuleSize(modSize);
                InitTrainer();
                InitIL2CPPPointers();
                ScanForPlayerRefrences();
                offsetCache.Save();
            }
            else
                continue;
        }

        // === Retry IL2CPP statics if not resolved ===
        if (!il2cpp.aiStaticFields)
        {
            static DWORD lastIL2CPPRetry = 0;
            DWORD now = GetTickCount();
            if (now - lastIL2CPPRetry > 5000)
            {
                lastIL2CPPRetry = now;
                InitIL2CPPPointers();
            }
        }

        // === Refresh player chains (pointers may change on scene reload / death) ===
        {
            std::lock_guard<std::mutex> lock(playerMutex);
            ResolvePlayerChains();
        }

        // Re-scan if player instance was invalidated (death, scene change)
        if (!player.instance)
        {
            static DWORD lastRescanTick = 0;
            DWORD now = GetTickCount();
            if (now - lastRescanTick > 3000)
            {
                lastRescanTick = now;
                std::lock_guard<std::mutex> lock(playerMutex);
                if (ScanForPlayerRefrences())
                    DBG("[Player] Re-scan OK: instance=0x%llX\n", (unsigned long long)player.instance);
            }
        }

        // === Detect in-match state ===
        // wave > 0 AND AIManager.instance alive (m_CachedPtr valid)
        {
            bool wasInMatch = inMatch;
            inMatch = false;

            if (il2cpp.gdStaticFields)
            {
                int32_t wave = Read<int32_t>(il2cpp.gdStaticFields + Offset::IL2CPP::GD_CurrentWave);
                if (wave > 0 && il2cpp.aiStaticFields)
                {
                    uint64_t aiInst = Read<uint64_t>(il2cpp.aiStaticFields + Offset::IL2CPP::AI_Instance);
                    if (IsValidUserPtr(aiInst))
                    {
                        // Check if AIManager is still alive (native ptr valid)
                        uint64_t aiNative = Read<uint64_t>(aiInst + 0x10);  // m_CachedPtr
                        if (IsValidUserPtr(aiNative))
                            inMatch = true;
                    }
                }
            }

            if (wasInMatch && !inMatch)
            {
                DBG("[State] Left match (menu/loading)\n");
                // Clear stale match data so everything re-acquires on next match
                std::lock_guard<std::mutex> lock(playerMutex);
                player.instance = 0;
                cameraStructBase = 0;
            }
            else if (!wasInMatch && inMatch)
            {
                DBG("[State] Entered match\n");
            }
        }

        // === Read NPCs from AIManager.NPCs (only in match) ===
        std::vector<Player> temp;

        if (inMatch && il2cpp.aiStaticFields)
        {
            using namespace Offset::IL2CPP;

            uint64_t aiInst = Read<uint64_t>(il2cpp.aiStaticFields + AI_Instance);
            if (!IsValidUserPtr(aiInst))
                goto publish;

            uint64_t npcList = Read<uint64_t>(aiInst + AI_NPCs);
            if (!IsValidUserPtr(npcList))
                goto publish;

            int32_t npcCount = Read<int32_t>(npcList + 0x18);  // List<T>._size
            uint64_t npcArr = Read<uint64_t>(npcList + 0x10);   // List<T>._items
            if (npcCount <= 0 || npcCount > 200 || !IsValidUserPtr(npcArr))
                goto publish;

            for (int i = 0; i < npcCount; i++)
            {
                uint64_t npc = Read<uint64_t>(npcArr + 0x20 + i * 8);
                if (!IsValidUserPtr(npc)) continue;

                // Get native Transform via hierarchy
                uint64_t nativeTr = GetNativeTransform(npc);
                if (!nativeTr) continue;

                Vector3 worldPos;
                if (!ReadWorldPosition(nativeTr, worldPos))
                    continue;

                // Sanity: skip zero / extreme positions
                float mag = sqrtf(worldPos.x * worldPos.x + worldPos.y * worldPos.y + worldPos.z * worldPos.z);
                if (mag < 0.01f || mag > 50000.f)
                    continue;

                // Read health: NPC → +0x78 → Health component
                float hp = 100.f, maxHp = 100.f;
                uint64_t healthPtr = Read<uint64_t>(npc + 0x78);
                if (IsValidUserPtr(healthPtr))
                {
                    int32_t h = Read<int32_t>(healthPtr + HP_Health);
                    int32_t mh = Read<int32_t>(healthPtr + HP_MaxHealth);
                    if (h > 0 && h < 100000 && mh > 0 && mh < 100000)
                    {
                        hp = (float)h;
                        maxHp = (float)mh;
                    }
                }

                // Skip dead NPCs
                if (hp <= 0.f) continue;

                // Compute aim target position
                float fallbackY = (g_Settings.aimbot.targetBone == 0)
                    ? g_Settings.aimbot.headOffset
                    : g_Settings.aimbot.torsoOffset;
                Vector3 headPos = worldPos + Vector3(0, fallbackY, 0);
                bool hasBone = false;

                // For head targeting, try actual bone reading
                if (g_Settings.aimbot.targetBone == 0)
                {
                    if (GetHeadPosition(npc, worldPos, headPos, true))
                        hasBone = true;
                }

                Player p;
                p.Ptr          = npc;
                p.Team         = 0;
                p.Health       = hp;
                p.MaxHealth    = maxHp;
                p.Position     = worldPos;
                p.HeadPos      = headPos;
                p.hasBoneHead  = hasBone;
                temp.push_back(p);
            }
        }

publish:
        {
            std::lock_guard<std::mutex> lock(g_DataMutex);
            g_Players = std::move(temp);
        }
    }
}

// ---------------------------------------------------------------------------
// RenderLoop — main overlay rendering + aimbot
// ---------------------------------------------------------------------------

void Game::RenderLoop()
{
    DBG("[+] RenderLoop started\n");

    while (true)
    {
        Sleep(1);

        if (!Draw.overlayHWND || !Draw.running)
            continue;

        // --- Quit hotkey (DELETE) ---
        static bool delWasDown = false;
        bool delIsDown = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        if (delIsDown && !delWasDown)
        {
            DBG("[*] Quit hotkey pressed — shutting down\n");
            RestoreAllPatches();
            ExitProcess(0);
        }
        delWasDown = delIsDown;

        // --- Menu toggle ---
        static bool keyWasDown = false;
        bool keyIsDown = (GetAsyncKeyState(g_Settings.menuKey) & 0x8000) != 0;
        if (keyIsDown && !keyWasDown)
        {
            g_Settings.menuOpen = !g_Settings.menuOpen;
            Draw.ClickThrough(!g_Settings.menuOpen);
        }
        keyWasDown = keyIsDown;

        // --- Begin frame ---
        Draw.BeginDraw();

        // --- Resolution tracking ---
        int newW = (int)Draw.GameCenterW;
        int newH = (int)Draw.GameCenterH;
        if (newW > 0 && newH > 0 && (newW != ScreenW || newH != ScreenH))
        {
            if (ScreenW > 0)  // skip first init
                DBG("[State] Resolution changed: %dx%d -> %dx%d\n", ScreenW, ScreenH, newW, newH);
            ScreenW = newW;
            ScreenH = newH;
            // Clamp FOV to new screen diagonal
            float maxFov = sqrtf((float)(ScreenW * ScreenW + ScreenH * ScreenH));
            if (g_Settings.aimbot.fov > maxFov)
                g_Settings.aimbot.fov = maxFov;
        }

        // --- VP matrix with recovery ---
        static int vpFailCount = 0;
        if (gameConnected)
        {
            if (ReadMVP())
            {
                vpFailCount = 0;
            }
            else
            {
                vpFailCount++;
                if (vpFailCount == 60)  // ~1 second of fails
                {
                    DBG("[VP] ReadMVP failing — re-probing camera\n");
                    std::lock_guard<std::mutex> lock(playerMutex);
                    cameraStructBase = 0;
                    ResolvePlayerChains();
                }
                if (vpFailCount == 300) // ~5 seconds of fails
                {
                    DBG("[VP] Rescanning VP matrix...\n");
                    vpAddress = 0;
                    ScanForVPMatrix();
                    vpFailCount = 0;
                }
            }
        }

        // --- Draw Menu ---
        if (g_Settings.menuOpen)
        {
            ImGui::SetNextWindowSize(ImVec2(350, 400), ImGuiCond_Once);
            ImGui::Begin("Phantom DFB", nullptr, ImGuiWindowFlags_NoCollapse);

            if (ImGui::BeginTabBar("##tabs"))
            {
                if (ImGui::BeginTabItem("ESP"))
                {
                    ImGui::Checkbox("Show Enemies",  &g_Settings.esp.showPlayers);
                    ImGui::Checkbox("Show Distance",  &g_Settings.esp.showDistance);
                    ImGui::SliderFloat("Max Distance", &g_Settings.esp.maxDistance, 50.f, 1000.f, "%.0f m");
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Aimbot"))
                {
                    ImGui::Checkbox("Enable",          &g_Settings.aimbot.enabled);

                    // FOV: max = screen diagonal so it can cover entire screen
                    float maxFov = sqrtf((float)(ScreenW * ScreenW + ScreenH * ScreenH));
                    if (maxFov < 400.f) maxFov = 400.f;
                    ImGui::SliderFloat("FOV",          &g_Settings.aimbot.fov, 10.f, maxFov, "%.0f px");

                    ImGui::SliderFloat("Max Distance", &g_Settings.aimbot.maxDistance, 50.f, 800.f, "%.0f m");
                    ImGui::SliderFloat("Smooth",       &g_Settings.aimbot.smooth, 1.f, 20.f, "%.1f");
                    ImGui::SliderFloat("Sensitivity",  &g_Settings.aimbot.sensitivity, 0.1f, 5.f, "%.2f");
                    ImGui::Checkbox("Show FOV Circle", &g_Settings.aimbot.showFovCircle);

                    ImGui::Separator();
                    ImGui::Checkbox("No Recoil", &g_Settings.aimbot.noRecoil);
                    ImGui::Checkbox("No Spread", &g_Settings.aimbot.noSpread);

                    ImGui::Separator();
                    ImGui::Text("Target Bone:");
                    ImGui::RadioButton("Head", &g_Settings.aimbot.targetBone, 0);
                    ImGui::SameLine();
                    ImGui::RadioButton("Torso", &g_Settings.aimbot.targetBone, 1);
                    ImGui::SliderFloat("Head Offset", &g_Settings.aimbot.headOffset, 1.0f, 2.5f, "%.1f m");
                    ImGui::SliderFloat("Torso Offset", &g_Settings.aimbot.torsoOffset, 0.5f, 1.5f, "%.1f m");

                    ImGui::Separator();
                    // Bone status
                    if (g_ParentIndicesOff != 0)
                        ImGui::TextColored(ImVec4(0.3f,1,0.3f,1), "Bones: parent chain OK (hierarchy+0x%X)", g_ParentIndicesOff);
                    else if (g_ParentIndicesProbed)
                        ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "Bones: no parent array (using offset)");
                    else
                        ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "Bones: waiting for probe...");

                    ImGui::EndTabItem();
                }

                // ============ TRAINER TAB ============
                if (ImGui::BeginTabItem("Trainer"))
                {
                    if (!trainerReady)
                    {
                        ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "Trainer unavailable");
                        ImGui::TextWrapped("GameAssembly.dll not found or AOB scan failed.");
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(0.3f,1,0.3f,1), "GameAssembly.dll loaded");
                        ImGui::Separator();

                        for (int i = 0; i < PATCH_COUNT; i++)
                        {
                            TrainerPatch& p = g_Patches[i];

                            // Hide secondary group members (they toggle with the primary)
                            if (p.group > 0 && i > 0 && g_Patches[i-1].group == p.group)
                                continue;

                            if (!p.found)
                            {
                                ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "%s (not found)", p.name);
                                continue;
                            }

                            bool toggled = p.active;
                            if (ImGui::Checkbox(p.name, &toggled))
                            {
                                TogglePatch(i, toggled);
                            }
                            if (ImGui::IsItemHovered())
                            {
                                ImGui::SetTooltip("GameAssembly.dll+0x%llX (%d bytes)",
                                    (unsigned long long)(p.addr - GameAssemblyBase),
                                    p.patchSize);
                            }
                        }
                    }

                    // --- IL2CPP Memory Cheats ---
                    if (il2cpp.gmStaticFields || il2cpp.gdStaticFields || il2cpp.gameDirector)
                    {
                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "Game Modifiers");

                        using namespace Offset::IL2CPP;

                        // Global Damage Multiplier
                        if (il2cpp.gmStaticFields)
                        {
                            float dmgMult = Read<float>(il2cpp.gmStaticFields + GM_GlobalDamageMultiplier);
                            if (ImGui::SliderFloat("Damage Mult", &dmgMult, 0.1f, 50.0f, "%.1fx"))
                                WriteMemory(il2cpp.gmStaticFields + GM_GlobalDamageMultiplier, &dmgMult, 4);

                            float ptsMult = Read<float>(il2cpp.gmStaticFields + GM_PointsMultiplier);
                            if (ImGui::SliderFloat("Points Mult", &ptsMult, 1.0f, 100.0f, "%.1fx"))
                                WriteMemory(il2cpp.gmStaticFields + GM_PointsMultiplier, &ptsMult, 4);
                        }

                        // Wave info & control
                        if (il2cpp.gdStaticFields)
                        {
                            int wave = Read<int32_t>(il2cpp.gdStaticFields + GD_CurrentWave);
                            ImGui::Text("Current Wave: %d", wave);
                        }

                        // GameDirector instance cheats
                        if (il2cpp.gameDirector)
                        {
                            int zombieHp = Read<int32_t>(il2cpp.gameDirector + GD_ZombieHp);
                            if (ImGui::SliderInt("Zombie HP", &zombieHp, 1, 99999))
                                WriteMemory(il2cpp.gameDirector + GD_ZombieHp, &zombieHp, 4);

                            int zombieDmg = Read<int32_t>(il2cpp.gameDirector + GD_ZombieDamage);
                            if (ImGui::SliderInt("Zombie Damage", &zombieDmg, 0, 200))
                                WriteMemory(il2cpp.gameDirector + GD_ZombieDamage, &zombieDmg, 4);

                            float zombieSpd = Read<float>(il2cpp.gameDirector + GD_ZombieSpeed);
                            if (ImGui::SliderFloat("Zombie Speed", &zombieSpd, 0.0f, 20.0f, "%.1f"))
                                WriteMemory(il2cpp.gameDirector + GD_ZombieSpeed, &zombieSpd, 4);
                        }
                    }

                    ImGui::EndTabItem();
                }

                // ---- Player Cheats tab ----
                if (ImGui::BeginTabItem("Player"))
                {
                    if (!player.instance)
                    {
                        ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "PlayerRefrences not found");
                        ImGui::TextWrapped("Start a solo game first, then restart Phantom.");
                        if (ImGui::Button("Retry Scan"))
                            ScanForPlayerRefrences();
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(0.3f,1,0.3f,1), "Player found");
                        ImGui::Separator();

                        // --- Wave control (via GameDirector) ---
                        if (il2cpp.gameDirector)
                        {
                            int wave = Read<int32_t>(il2cpp.gameDirector + Offset::IL2CPP::GD_Wave);
                            if (ImGui::SliderInt("Wave", &wave, 1, 9999))
                                Write<int32_t>(il2cpp.gameDirector + Offset::IL2CPP::GD_Wave, wave);
                        }

                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "Health & Economy");

                        // --- HP ---
                        if (IsValidUserPtr(player.health))
                        {
                            int hp    = Read<int32_t>(player.health + Offset::IL2CPP::HP_Health);
                            int maxHp = Read<int32_t>(player.health + Offset::IL2CPP::HP_MaxHealth);
                            if (maxHp < 1) maxHp = 200;
                            if (ImGui::SliderInt("HP", &hp, 1, 99999))
                                Write<int32_t>(player.health + Offset::IL2CPP::HP_Health, hp);
                        }

                        // --- Money ---
                        if (IsValidUserPtr(player.currency))
                        {
                            int money = Read<int32_t>(player.currency + Offset::IL2CPP::PC_Money);
                            if (ImGui::SliderInt("Money", &money, 0, 9999999))
                                Write<int32_t>(player.currency + Offset::IL2CPP::PC_Money, money);
                        }

                        // --- Grenades ---
                        if (IsValidUserPtr(player.grenadeInv))
                        {
                            int nades = Read<int32_t>(player.grenadeInv + Offset::IL2CPP::GI_LethalAmount);
                            if (ImGui::SliderInt("Grenades", &nades, 0, 9999))
                                Write<int32_t>(player.grenadeInv + Offset::IL2CPP::GI_LethalAmount, nades);
                        }

                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "Movement");

                        // --- Speed ---
                        if (IsValidUserPtr(player.playerMovement))
                        {
                            float speed = Read<float>(player.playerMovement + Offset::IL2CPP::MOV_NormalSpeed);
                            if (ImGui::SliderFloat("Walk Speed", &speed, 1.0f, 200.0f, "%.1f"))
                                Write<float>(player.playerMovement + Offset::IL2CPP::MOV_NormalSpeed, speed);

                            float sprint = Read<float>(player.playerMovement + Offset::IL2CPP::MOV_SprintSpeed);
                            if (ImGui::SliderFloat("Sprint Speed", &sprint, 1.0f, 300.0f, "%.1f"))
                                Write<float>(player.playerMovement + Offset::IL2CPP::MOV_SprintSpeed, sprint);

                            float jump = Read<float>(player.playerMovement + Offset::IL2CPP::MOV_JumpForce);
                            if (ImGui::SliderFloat("Jump Force", &jump, 1.0f, 200.0f, "%.1f"))
                                Write<float>(player.playerMovement + Offset::IL2CPP::MOV_JumpForce, jump);

                            int extraJumps = Read<int32_t>(player.playerMovement + Offset::IL2CPP::PM_ExtraJump);
                            if (ImGui::SliderInt("Extra Jumps", &extraJumps, 0, 999))
                                Write<int32_t>(player.playerMovement + Offset::IL2CPP::PM_ExtraJump, extraJumps);
                        }

                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "Weapon");

                        // --- Fire rate & damage (from equipped weapon) ---
                        if (IsValidUserPtr(player.weaponManager))
                        {
                            uint64_t gun = Read<uint64_t>(player.weaponManager + Offset::IL2CPP::WM_EquipedWeapon);
                            if (IsValidUserPtr(gun))
                            {
                                float fireRate = Read<float>(gun + Offset::IL2CPP::GUN_FireRate);
                                if (ImGui::SliderFloat("Fire Rate", &fireRate, 0.001f, 100.0f, "%.3f s"))
                                    Write<float>(gun + Offset::IL2CPP::GUN_FireRate, fireRate);

                                float dmg = Read<float>(gun + Offset::IL2CPP::GUN_Damage);
                                if (ImGui::SliderFloat("Gun Damage", &dmg, 1.0f, 99999.0f, "%.0f"))
                                    Write<float>(gun + Offset::IL2CPP::GUN_Damage, dmg);

                                int clipSize = Read<int32_t>(gun + Offset::IL2CPP::GUN_ClipSize);
                                if (ImGui::SliderInt("Clip Size", &clipSize, 1, 9999))
                                    Write<int32_t>(gun + Offset::IL2CPP::GUN_ClipSize, clipSize);
                            }
                            else
                            {
                                ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "No weapon equipped");
                            }
                        }

                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "Camera: %s",
                            IsValidUserPtr(player.cameraNative) ? "found" : "not found");
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::End();
        }

        // --- FOV circle ---
        if (g_Settings.aimbot.showFovCircle && g_Settings.aimbot.enabled)
        {
            Draw.DrawCircle(Draw.GameCenterX, Draw.GameCenterY,
                            g_Settings.aimbot.fov, COLOR_WHITE, 64, 1.0f);
        }

        // --- VP status indicator ---
        if (gameConnected)
        {
            if (vpAddress || cameraStructBase)
                Draw.DrawNewText(10, 40, 0x00FF00, 12.f, "VP: OK");
            else
                Draw.DrawNewText(10, 40, 0x0000FF, 12.f, "VP: scanning...");
        }

        // --- No Recoil / No Spread (runs every frame, independent of aimbot) ---
        if (gameConnected && (g_Settings.aimbot.noRecoil || g_Settings.aimbot.noSpread) &&
            IsValidUserPtr(player.weaponManager))
        {
            uint64_t gun = Read<uint64_t>(player.weaponManager + Offset::IL2CPP::WM_EquipedWeapon);
            if (IsValidUserPtr(gun))
            {
                if (g_Settings.aimbot.noRecoil)
                {
                    uint64_t recoil = Read<uint64_t>(gun + Offset::Gun::Recoil);
                    if (IsValidUserPtr(recoil))
                    {
                        float zero[3] = {0, 0, 0};
                        WriteMemory(recoil + Offset::Recoil::CurrentRot, zero, 12);
                        WriteMemory(recoil + Offset::Recoil::TargetRot, zero, 12);
                    }
                }
                if (g_Settings.aimbot.noSpread)
                {
                    float zero = 0.f;
                    WriteMemory(gun + Offset::Gun::Inaccuracy, &zero, 4);
                }
            }
        }

        // --- ESP + Aimbot ---
        if (gameConnected && (g_Settings.esp.showPlayers || g_Settings.aimbot.enabled))
        {
            std::lock_guard<std::mutex> lock(g_DataMutex);

            // Aimbot: pick closest enemy by 3D distance, then check FOV
            float   bestWorldDist = g_Settings.aimbot.maxDistance;
            Vector2 bestAimTarget = {};
            bool    hasAimTarget = false;

            for (auto& p : g_Players)
            {
                if (g_Settings.esp.showPlayers)
                {
                    DrawPlayer(p);

                    // Draw head marker (small cross at head position)
                    Vector2 headScreen;
                    if (WorldToScreen(p.HeadPos, headScreen))
                    {
                        int hc = p.hasBoneHead ? 0x00FF00 : 0xFFFF00;  // green=bone, yellow=offset
                        Draw.DrawLine((int)headScreen.x - 4, (int)headScreen.y,
                                      (int)headScreen.x + 4, (int)headScreen.y, hc, 1.5f);
                        Draw.DrawLine((int)headScreen.x, (int)headScreen.y - 4,
                                      (int)headScreen.x, (int)headScreen.y + 4, hc, 1.5f);
                    }
                }

                if (g_Settings.aimbot.enabled &&
                    (GetAsyncKeyState(g_Settings.aimbot.hotkey) & 0x8000))
                {
                    Vector2 aimScreen;
                    if (WorldToScreen(p.HeadPos, aimScreen))
                    {
                        // FOV check in screen space
                        float cx = (float)Draw.GameCenterX;
                        float cy = (float)Draw.GameCenterY;
                        float dx = aimScreen.x - cx;
                        float dy = aimScreen.y - cy;
                        float screenDist = sqrtf(dx * dx + dy * dy);
                        if (screenDist > g_Settings.aimbot.fov) continue;

                        // Pick closest enemy by 3D world distance
                        float worldDist = 0.f;
                        WorldToScreen(p.HeadPos, aimScreen, &worldDist);
                        if (worldDist < bestWorldDist)
                        {
                            bestWorldDist = worldDist;
                            bestAimTarget = aimScreen;
                            hasAimTarget = true;
                        }
                    }
                }
            }

            if (hasAimTarget)
            {
                float cx = (float)Draw.GameCenterX;
                float cy = (float)Draw.GameCenterY;
                float dx = bestAimTarget.x - cx;
                float dy = bestAimTarget.y - cy;
                float smooth = g_Settings.aimbot.smooth;
                float sens   = g_Settings.aimbot.sensitivity;
                if (sens < 0.01f) sens = 0.01f;
                float moveX = dx / (smooth * sens);
                float moveY = dy / (smooth * sens);
                moveX = std::clamp(moveX, -40.f, 40.f);
                moveY = std::clamp(moveY, -40.f, 40.f);

                if (fabsf(moveX) > 0.3f || fabsf(moveY) > 0.3f)
                {
                    INPUT input = {};
                    input.type = INPUT_MOUSE;
                    input.mi.dx = (LONG)moveX;
                    input.mi.dy = (LONG)moveY;
                    input.mi.dwFlags = MOUSEEVENTF_MOVE;
                    SendInput(1, &input, sizeof(INPUT));
                }
            }
        }

        // --- Status text ---
        if (!gameConnected)
            Draw.DrawNewText(10, 10, COLOR_RED, 14.f, "Waiting for Dead From Beyond...");
        else if (!inMatch)
            Draw.DrawNewText(10, 10, 0xFFAA00, 14.f, "Menu / Lobby");
        else
            Draw.DrawNewText(10, 10, COLOR_GREEN, 14.f, "In Game [%d]", (int)g_Players.size());

        Draw.EndDraw();
    }
}
