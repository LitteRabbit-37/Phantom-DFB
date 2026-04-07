#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>
#include "vmmProc.h"

// Offset.h — Dead From Beyond (v1.31 64bit, Unity IL2CPP)
//
// === ENGINE ===
// Unity IL2CPP — all game logic in GameAssembly.dll
// Main exe: "Dead From Beyond.exe" loads GameAssembly.dll
//
// === DISCOVERED (CE) ===
//
// Health (int32):
//   entity+0x20 — current HP (damage subtracted via `sub [rbx+20],edi`)
//   entity+0x68 — display/synced HP (written via `mov [rdi+68],eax`)
//   Damage function at GameAssembly.dll+26D1D8
//
// Money (int32):
//   resource_obj+0x30 — spending via `add [rbx+30],edi` (edi negative)
//   Money function at GameAssembly.dll+294A2F
//
// Medkits / Aids (int32):
//   item_obj+0x18 — consumed via `dec [rbx+18]`
//   Decrement at GameAssembly.dll+276B49
//
// Weapon ammo (int32):
//   weapon+0x110 — consumed via `sub [rbx+110],eax`
//   Per-weapon-instance (new address when weapon is dropped/bought)
//   Ammo function at GameAssembly.dll+2B22B5
//
namespace Offset
{
    // ---------------------------------------------------------------------------
    // Pointer chain resolver
    // ---------------------------------------------------------------------------

    inline uint64_t ResolveChain(uint64_t base, const std::vector<uint32_t>& offsets)
    {
        if (base < 0x10000 || base > 0x7FFFFFFFFFFFULL)
            return 0;

        uint64_t addr = base;
        for (size_t i = 0; i < offsets.size(); i++)
        {
            addr = Read<uint64_t>(addr + offsets[i]);
            if (addr == 0 || addr < 0x10000 || addr > 0x7FFFFFFFFFFFULL)
                return 0;
        }
        return addr;
    }

    // ---------------------------------------------------------------------------
    // Structural offsets (from CE "Find what writes")
    // ---------------------------------------------------------------------------

    // ---------------------------------------------------------------------------
    // TransformHierarchy — position via Unity native hierarchy
    //
    // Reversed from UnityPlayer.dll:
    //   GetTransformAccess (+0x57D3C0): reads TransformAccess from Component+0x38
    //   Inner functions (+0x12E880, +0x12FA10): read TRS from hierarchy data
    //
    // TransformAccess = {uint64_t hierarchy_ptr, int32_t index} at Component+0x38
    // TRS entry (48 bytes): {Vec3 localPos(+0x00), Quat localRot(+0x10), Vec3 localScale(+0x20)}
    // For ROOT transforms (idx=0), localPos = worldPos.
    // ---------------------------------------------------------------------------

    namespace Transform
    {
        inline constexpr uint32_t NativeComp_GO     = 0x30;  // native Component → GameObject

        // TransformAccess is at +0x38 in any native Component (confirmed by reversing
        // GetTransformAccess at UnityPlayer.dll+0x57D3C0: movups xmm0, [rsi+0x38])
        // Struct: {uint64_t hierarchy_ptr, int32_t index, int32_t padding}
        inline constexpr uint32_t TA_HierarchyPtr   = 0x38;  // Component → TransformAccess.hierarchy
        inline constexpr uint32_t TA_Index          = 0x40;  // Component → TransformAccess.index (lo32)

        // Hierarchy data: local TRS array at hierarchy+0x18, stride 48 bytes per entry.
        // Entry layout: {Vec3 localPos(+0x00), Quat localRot(+0x10), Vec3 localScale(+0x20)}
        // For ROOT transforms (idx=0), localPos = worldPos.
        // NPC zombies are always root of their own hierarchy (idx=0).
        inline constexpr uint32_t Hierarchy_DataPtr = 0x18;
        inline constexpr uint32_t Entry_Stride      = 48;
        inline constexpr uint32_t Entry_LocalPos    = 0x00;  // = worldPos for root transforms
    }

    // ---------------------------------------------------------------------------
    // NPC / RagdollController — bone access (from dump.cs)
    // ---------------------------------------------------------------------------

    namespace NPC
    {
        inline constexpr uint32_t Health            = 0x78;  // Health*
        inline constexpr uint32_t Animator          = 0x80;  // Animator*
        inline constexpr uint32_t RagdollController = 0x98;  // RagdollController*
        inline constexpr uint32_t EyeSight          = 0xA0;  // Transform* (eye position)
    }

    namespace RagdollCtrl
    {
        inline constexpr uint32_t Rbs             = 0x18;  // List<Rigidbody>*
        inline constexpr uint32_t Transforms      = 0x20;  // Transform[] (IL2CPP array)
        inline constexpr uint32_t Root            = 0x28;  // Transform* (root bone)
        inline constexpr uint32_t Ragdoll         = 0x30;  // BakeRagdoll*
        inline constexpr uint32_t RagdollSpawned  = 0x38;  // bool
    }

    // Recoil system — Gun._recoil → Recoil component (from dump.cs)
    namespace Recoil
    {
        inline constexpr uint32_t CurrentRot  = 0x20;  // Vector3 — current applied rotation
        inline constexpr uint32_t TargetRot   = 0x2C;  // Vector3 — target recoil rotation
        inline constexpr uint32_t Snapiness   = 0x38;  // float
        inline constexpr uint32_t ReturnSpeed = 0x3C;  // float
    }

    namespace Gun
    {
        inline constexpr uint32_t Recoil      = 0x100;  // Recoil* component
        inline constexpr uint32_t Inaccuracy  = 0xE8;   // float — current spread
    }

    // IL2CPP array layout: [klass+0x00][monitor+0x08][bounds+0x10][max_length+0x18][elem0+0x20]
    namespace IL2CppArray
    {
        inline constexpr uint32_t MaxLength  = 0x18;  // uintptr_t
        inline constexpr uint32_t FirstElem  = 0x20;  // T[0] starts here
        inline constexpr uint32_t ElemStride = 0x08;  // pointer size for ref types
    }

    // ---------------------------------------------------------------------------
    // IL2CPP class RVAs (from Il2CppDumper analysis)
    // [GA+RVA] → Il2CppClass* → [+0xB8] → static_fields → [+offset] → value
    // ---------------------------------------------------------------------------

    namespace IL2CPP
    {
        // Class pointer RVAs in GameAssembly.dll
        inline constexpr uint32_t GameManager_Class       = 0x10CF508;
        inline constexpr uint32_t GameDirector_Class      = 0x10CF4C0;
        inline constexpr uint32_t AIManager_Class         = 0x10C7A80;
        inline constexpr uint32_t RenderPipelineMgr_Class = 0x10D9D18;

        // RVA scan range for FindClassByName
        // Known classes at 0x10C7-0x10D9 but table spans much wider.
        // Scan the entire .data section of GA.dll (last ~5 MB).
        inline constexpr uint32_t MetadataRVA_Start = 0x1000000;
        inline constexpr uint32_t MetadataRVA_End   = 0x1600000;  // adjusted at runtime to min(this, GA size)

        // Il2CppClass struct offsets (from il2cpp.h: Il2CppClass_1 + _2)
        inline constexpr uint32_t Klass_Name         = 0x10;   // const char*
        inline constexpr uint32_t Klass_Parent       = 0x58;   // Il2CppClass*
        inline constexpr uint32_t Klass_StaticFields  = 0xB8;  // void*
        inline constexpr uint32_t Klass_InstanceSize  = 0xF8;  // uint32_t
        inline constexpr uint32_t Klass_Token         = 0x118; // uint32_t (0x02000000 | TypeDefIndex)
        inline constexpr uint32_t Klass_FieldCount    = 0x11C; // uint16_t

        // PlayerRefrences stable identifiers (confirmed at runtime, DFB v1.31)
        inline constexpr uint32_t PR_ExpectedToken        = 0x0200013E;  // runtime metadata token
        inline constexpr uint32_t PR_ExpectedInstanceSize  = 0x88;       // 0x18 base + 13*8 ptrs

        // Il2CppObject header
        inline constexpr uint32_t Obj_Klass   = 0x0;  // Il2CppClass*
        inline constexpr uint32_t Obj_Monitor = 0x8;  // void*

        // Static field offsets (in static_fields block)
        // GameManager statics
        inline constexpr uint32_t GM_Instance              = 0x0;   // GameManager*
        inline constexpr uint32_t GM_GlobalDamageMultiplier = 0x8;   // float
        inline constexpr uint32_t GM_AITickMultiplier       = 0xC;   // float
        inline constexpr uint32_t GM_PointsMultiplier       = 0x10;  // float

        // GameDirector statics
        inline constexpr uint32_t GD_IsRageModeActive = 0x0;  // bool
        inline constexpr uint32_t GD_CurrentWave      = 0x4;  // int32

        // AIManager statics
        inline constexpr uint32_t AI_Instance = 0x0;   // AIManager*

        // RenderPipelineManager statics
        inline constexpr uint32_t RPM_Cameras = 0x8;   // List<Camera>*

        // AIManager instance fields
        inline constexpr uint32_t AI_GameDirector = 0x50;  // GameDirector*
        inline constexpr uint32_t AI_NPCs         = 0x58;  // List<NPC>*

        // GameDirector instance fields
        inline constexpr uint32_t GD_Wave           = 0x98;  // int32
        inline constexpr uint32_t GD_ZombieHp       = 0x7C;  // int32
        inline constexpr uint32_t GD_ZombieDamage   = 0x80;  // int32
        inline constexpr uint32_t GD_ZombieSpeed    = 0x84;  // float
        inline constexpr uint32_t GD_MaxZombieCount = 0x58;  // int32

        // ----- PlayerRefrences instance fields (dump.cs TypeDefIndex: 2453) -----
        inline constexpr uint32_t PR_PlayerMovement = 0x18;  // PlayerMovement*
        inline constexpr uint32_t PR_Health         = 0x20;  // PlayerHealth*
        inline constexpr uint32_t PR_WeaponManager  = 0x28;  // WeaponManager*
        inline constexpr uint32_t PR_CameraRotator  = 0x30;  // RotateCamera*
        inline constexpr uint32_t PR_Currency       = 0x38;  // PlayerCurrency*
        inline constexpr uint32_t PR_Interactions   = 0x40;  // PlayerInteractions*
        inline constexpr uint32_t PR_HudFeedback    = 0x48;  // PlayerInteractionFeedback*
        inline constexpr uint32_t PR_HealthInv      = 0x50;  // HealSelf*
        inline constexpr uint32_t PR_GrenadeInv     = 0x58;  // GrenadeInventory*
        inline constexpr uint32_t PR_PromptHud      = 0x60;  // UpgradePromptHUD*
        inline constexpr uint32_t PR_Inventory      = 0x68;  // ItemInventory*
        inline constexpr uint32_t PR_PlayerStats    = 0x70;  // PlayerStats*
        inline constexpr uint32_t PR_PerkAnimation  = 0x78;  // PerkAnimation*
        inline constexpr uint32_t PR_PerkInventory  = 0x80;  // PerkInventory*

        // ----- RotateCamera instance fields -----
        inline constexpr uint32_t RC_MainCamera     = 0x30;  // Camera* (managed)

        // ----- Movement base class fields -----
        inline constexpr uint32_t MOV_Rigidbody     = 0x28;  // Rigidbody*
        inline constexpr uint32_t MOV_SprintSpeed   = 0x50;  // float
        inline constexpr uint32_t MOV_NormalSpeed   = 0x5C;  // float
        inline constexpr uint32_t MOV_JumpForce     = 0xA8;  // float

        // ----- PlayerMovement fields (extends Movement) -----
        inline constexpr uint32_t PM_ExtraJump      = 0x158; // int32
        inline constexpr uint32_t PM_JumpsLeft      = 0x15C; // int32

        // ----- Gun fields -----
        inline constexpr uint32_t GUN_Damage        = 0x3C;  // float
        inline constexpr uint32_t GUN_FireRate       = 0x40;  // float
        inline constexpr uint32_t GUN_ClipSize       = 0x48;  // int32
        inline constexpr uint32_t GUN_InfiniteAmmo   = 0x108; // bool
        inline constexpr uint32_t GUN_LoadedAmmo     = 0x110; // int32
        inline constexpr uint32_t GUN_PlayerRefs     = 0x1A8; // PlayerRefrences*

        // ----- WeaponManager fields -----
        inline constexpr uint32_t WM_EquipedWeapon  = 0x90;  // Gun*
        inline constexpr uint32_t WM_Weapons        = 0xB0;  // List<Gun>*

        // ----- PlayerCurrency fields -----
        inline constexpr uint32_t PC_Money          = 0x30;  // int32
        inline constexpr uint32_t PC_Refrences      = 0x38;  // PlayerRefrences* (back-pointer)

        // ----- Health fields -----
        inline constexpr uint32_t HP_GodMode        = 0x18;  // bool
        inline constexpr uint32_t HP_Health         = 0x20;  // int32
        inline constexpr uint32_t HP_MaxHealth      = 0x24;  // int32

        // ----- GrenadeInventory fields -----
        inline constexpr uint32_t GI_LethalLimit    = 0x60;  // int32
        inline constexpr uint32_t GI_LethalAmount   = 0x68;  // int32

        // ----- Camera managed object → native -----
        inline constexpr uint32_t CAM_CachedPtr     = 0x10;  // IntPtr (native Camera*)

        // Native Camera matrix offsets (discovered via probe, stable per game version)
        inline constexpr uint32_t CAM_ViewMatrix     = 0x5C;  // float[16] worldToCameraMatrix
        inline constexpr uint32_t CAM_ProjMatrix     = 0x9C;  // float[16] projectionMatrix
    }
}
