#pragma once
#include <Windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cmath>
#include "auto_scan.h"

#define M_PI 3.1415926535

// ---------------------------------------------------------------------------
// Shared data structures
// ---------------------------------------------------------------------------

struct Matrix4x4
{
    float m[4][4];
};

class Vector3
{
public:
    float x, y, z;

    Vector3() : x(0.f), y(0.f), z(0.f) {}
    Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

    inline float Dot(Vector3 v) const
    {
        return x * v.x + y * v.y + z * v.z;
    }

    inline float Distance(Vector3 v) const
    {
        float dx = v.x - x, dy = v.y - y, dz = v.z - z;
        return sqrtf(dx * dx + dy * dy + dz * dz);
    }

    inline float Length() const
    {
        return sqrtf(x * x + y * y + z * z);
    }

    inline Vector3 operator+(const Vector3& v) const { return Vector3(x + v.x, y + v.y, z + v.z); }
    inline Vector3 operator-(const Vector3& v) const { return Vector3(x - v.x, y - v.y, z - v.z); }
    inline Vector3 operator*(float v) const { return Vector3(x * v, y * v, z * v); }
    inline Vector3 operator/(float v) const { return Vector3(x / v, y / v, z / v); }
    inline Vector3& operator+=(const Vector3& v) { x += v.x; y += v.y; z += v.z; return *this; }
    inline Vector3& operator-=(const Vector3& v) { x -= v.x; y -= v.y; z -= v.z; return *this; }
};

struct Vector2
{
    float x;
    float y;
};

struct Player
{
    uint64_t Ptr;
    int      Team;
    float    Health;
    float    MaxHealth;
    Vector3  Position;   // root (feet) position
    Vector3  HeadPos;    // aimbot target (bone or root+offset)
    bool     hasBoneHead = false;  // true if HeadPos came from actual bone data
};

// ---------------------------------------------------------------------------
// Trainer — NOP patch system for GameAssembly.dll
//
// Unity IL2CPP: all game logic is in GameAssembly.dll.
// We AOB-scan for specific instructions and NOP them to toggle cheats.
// ---------------------------------------------------------------------------

struct TrainerPatch
{
    const char* name;           // Display name
    const char* aob;            // AOB pattern to find the instruction
    int         aobPatchOffset; // Byte offset from AOB match to the instruction to patch
    int         patchSize;      // Number of bytes to patch
    uint8_t     patchBytes[8];  // Bytes to write when enabled (0x90=NOP, 0xEB=JMP, etc.)
    uint64_t    addr;           // Resolved address (0 = not found)
    uint8_t     orig[8];        // Original bytes (saved before first patch)
    bool        found;          // True if AOB scan found the pattern
    bool        active;         // True if currently patched
    int         group;          // Patches with same group toggle together (0 = standalone)
};

// Patch indices (for g_Patches array in game.cpp)
enum PatchID {
    PATCH_GODMODE = 0,       // PlayerHealth.TakeDamage(int) — skip via iFrame jump
    PATCH_GODMODE2,          // PlayerHealth.TakeDamage(int,Vec3,DmgType) — same
    PATCH_INF_AMMO,
    PATCH_INF_MONEY,
    PATCH_INF_MEDKITS,
    PATCH_FULL_AUTO,         // Force all weapons to automatic fire mode
    PATCH_COUNT
};

// ---------------------------------------------------------------------------
// Game class — Dead From Beyond (v1.31 64bit, Unity IL2CPP)
// ---------------------------------------------------------------------------

class Game
{
public:
    void Start();
    void GameStart();

    // Background thread: enumerate entities
    void EntityLoop();

    // Render thread: ESP + aimbot + trainer menu
    void RenderLoop();

    // VP matrix
    bool ReadMVP();
    bool ScanForVPMatrix();

    // WorldToScreen
    bool WorldToScreen(const Vector3& world, Vector2& screen, float* outDist = nullptr);

    // Drawing
    bool DrawPlayer(const Player& p);

    // Trainer
    void InitTrainer();
    bool TogglePatch(int patchIdx, bool enable);
    void RestoreAllPatches();

    // State management
    void ResetState();               // full reset on game disconnect / scene change

    // IL2CPP helpers
    uint64_t GetStaticFields(uint32_t classRVA);  // [GA+RVA] → [+0xB8] → static_fields
    void InitIL2CPPPointers();                     // resolve all IL2CPP class pointers

    // GC heap scan — find PlayerRefrences instance
    uint64_t FindClassByName(const char* name);    // scan GA metadata for klass pointer
    bool     ScanForPlayerRefrences();              // heap scan for the unique instance
    void     ResolvePlayerChains();                 // follow PR → health, weapon, camera, etc.

public:
    DWORD    GamePid = 0;
    uint64_t GameBase = 0;
    uint64_t GameAssemblyBase = 0;
    uint64_t GameAssemblySize = 0;

    // Camera state
    Matrix4x4 ViewProj = {};
    uint64_t  vpAddress = 0;
    bool      vpIsTransposed = false;

    // Camera native pointer → View/Projection matrices (hardcoded for DFB v1.31)
    uint64_t  cameraStructBase = 0;
    uint32_t  cameraViewOffset = 0x5C;   // CAM_ViewMatrix
    uint32_t  cameraProjOffset = 0x9C;   // CAM_ProjMatrix

    // IL2CPP resolved pointers (resolved once at startup)
    struct {
        uint64_t gmStaticFields = 0;    // GameManager static fields
        uint64_t gdStaticFields = 0;    // GameDirector static fields
        uint64_t aiStaticFields = 0;    // AIManager static fields
        uint64_t rpmStaticFields = 0;   // RenderPipelineManager static fields
        uint64_t gameDirector = 0;      // GameDirector instance (via AIManager)
    } il2cpp;

    // Player pointer chain (from GC heap scan)
    struct {
        uint64_t klassPtr = 0;          // Il2CppClass* for PlayerRefrences
        uint64_t instance = 0;          // the one PlayerRefrences object
        uint64_t health = 0;            // PlayerHealth*
        uint64_t weaponManager = 0;     // WeaponManager*
        uint64_t cameraRotator = 0;     // RotateCamera*
        uint64_t playerMovement = 0;    // PlayerMovement*
        uint64_t currency = 0;          // PlayerCurrency*
        uint64_t grenadeInv = 0;        // GrenadeInventory*
        uint64_t cameraManaged = 0;     // Camera* (managed IL2CPP object)
        uint64_t cameraNative = 0;      // native Camera* (m_CachedPtr)
    } player;

    // Game resolution
    int ScreenW = 0;
    int ScreenH = 0;

    // Offset cache
    OffsetCache offsetCache;

    // Trainer state
    bool trainerReady = false;

    // Game state flags
    bool gameConnected = false;      // process found and modules resolved
    bool inMatch       = false;      // player alive in a match (not menu/lobby)

    // Thread safety for player struct + camera
    std::mutex playerMutex;          // protects player.*, cameraStructBase
};
