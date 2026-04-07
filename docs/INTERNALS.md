# Phantom DFB — Technical Internals

## Project

**Phantom** by **LitteRabbit** — external C++ overlay for **Dead From Beyond** (v1.31 64-bit, solo).

## Engine

**Unity IL2CPP** — all game logic in `GameAssembly.dll` (~21 MB).
No anti-cheat. Pure ReadProcessMemory, external DX11+ImGui overlay.

## Working Features

### Trainer (6/6 patches — AOB scan)

| Patch | Type | Detail |
|---|---|---|
| God Mode | `75->EB` x2 | PlayerHealth.TakeDamage iFrame skip (player only, bazooka OK) |
| Infinite Ammo | NOP 6B | `sub [rbx+110],eax` ammo decrement |
| Infinite Money | NOP 3B | `add [rbx+30],edi` money spend |
| Infinite Medkits | NOP 3B | `dec [rbx+18]` medkit use |
| Full Auto | `jz->jmp` 6B | Force fireType==0 branch in Gun.FireInput |

- **Restore-on-exit**: `ConsoleCtrlHandler` + `main()` cleanup restore patches
- **Groups**: God Mode patches 2 TakeDamage overloads together via `group=1`

### IL2CPP Pointer Resolution

| Singleton | GA RVA | Static Fields | Instance |
|---|---|---|---|
| GameManager | +0x10CF508 | globalDmgMult +0x8, ptsMult +0x10 | _instance +0x0 |
| GameDirector | +0x10CF4C0 | currentWave +0x4, isRageMode +0x0 | via AIManager |
| AIManager | +0x10C7A80 | instance +0x0 | gameDirector +0x50, NPCs +0x58 |

### PlayerRefrences — GC Heap Scan

Found via structural scan of the IL2CPP GC heap:
- Klass token `0x0200013E`, instSize `0x88` (stable per version)
- Fast path: scan by token+instSize, finds klass in ~2s
- Fallback: 13 consecutive valid pointers + cross-validation via PlayerCurrency back-pointer
- Auto re-scan every 3s after death/respawn

**Pointer chains from PlayerRefrences:**

| Field | Offset | Sub-fields |
|---|---|---|
| pMovement | +0x18 | normalSpeed +0x5C, sprintSpeed +0x50, jumpForce +0xA8, extraJump +0x158 |
| health | +0x20 | health +0x20, maxHealth +0x24, godMode +0x18 |
| weaponManager | +0x28 | equipedWeapon +0x90 -> Gun (_fireRate +0x40, _damage +0x3C, _clipSize +0x48) |
| cameraRotator | +0x30 | mainCamera +0x30 -> Camera (m_CachedPtr +0x10 -> native) |
| currency | +0x38 | money +0x30, refrences +0x38 (back-pointer) |
| grenadeInventory | +0x58 | lethalGrenadeAmount +0x68, lethalGrenadeLimit +0x60 |

### Camera VP (native pointer)

Camera native found via: PlayerRefrences -> RotateCamera -> Camera -> m_CachedPtr
- View matrix: native **+0x5C** (hardcoded)
- Proj matrix: native **+0x9C** (hardcoded)
- VP = P * V computed each frame in `ReadMVP()`

### Player Cheats (Player tab)

| Cheat | Slider | Range |
|---|---|---|
| Wave | int | 1-9999 |
| HP | int | 1-99999 |
| Money | int | 0-9999999 |
| Grenades | int | 0-9999 |
| Walk Speed | float | 1-200 |
| Sprint Speed | float | 1-300 |
| Jump Force | float | 1-200 |
| Extra Jumps | int | 0-999 |
| Fire Rate | float | 0.001-100s |
| Gun Damage | float | 1-99999 |
| Clip Size | int | 1-9999 |

### Zombie List — AIManager.NPCs

`AIManager.instance (+0x0) -> NPCs (+0x58)` = `List<NPC>` (count correct, HP read at +0x78 -> Health +0x20)

---

## ESP — TransformHierarchy

### Discovery

Reversed `Transform::get_position_Injected` in **UnityPlayer.dll** (28 MB, Unity IL2CPP).
Two internal functions found via pattern search (movss [rdx+8] + ret):

- **RVA 0x0012E880** — `get_localPosition`: reads at `data_base + index * 48`
- **RVA 0x0012FA10** — `get_position` (world): reads at `data_base + index * 48 + 0x20`

Both take a `TransformAccess {uint64_t hierarchy_ptr, int32_t index}` read from [rcx].
In the native Transform, this struct is at +0x38 (ptr) and +0x40 (index).

### World Position Formula (NPCs)

NPC zombies are root (idx=0) of their own hierarchy, so localPos = worldPos.

```
// TransformAccess at native Component +0x38 (confirmed: UnityPlayer.dll+0x57D3C0)
hierarchy_ptr = *(nativeTransform + 0x38)
index         = *(nativeTransform + 0x40)  // int32, always 0 for NPC roots
data_base     = *(hierarchy_ptr + 0x18)    // TRS data array
localPos      = *(data_base + index * 48 + 0x00)  // Vec3 = worldPos for roots
```

TRS entry (48 bytes): `{Vec3 localPos(+0x00), Quat localRot(+0x10), Vec3 localScale(+0x20)}`

### Chain: NPC -> World Position

```
NPC (managed) -> m_CachedPtr (+0x10) -> native MonoBehaviour
-> native GameObject (+0x30) -> component list (+0x30)
-> scan entries (16B each: {typeData, componentPtr})
-> find Transform (first with valid TransformAccess at +0x38)
-> hierarchy_ptr (+0x38), index (+0x40)
-> data_base (hierarchy_ptr + 0x18)
-> localPos (data_base + index * 48 + 0x00) = worldPos
```

---

## Aimbot — Head Targeting

### Implementation

Aimbot targets zombie heads. Two modes:

1. **Bone reading** (automatic if available): reads bones via RagdollController.transforms[]
   and picks the bone with highest world Y (= head).
2. **Root + offset Y** (fallback): rootPos + (0, headOffset, 0). Default headOffset=1.7m.

### Bone Reading — Parent Chain Walk

**Key discovery**: there is NO separate world position array in the hierarchy.
Confirmed by disassembly: `get_localPosition` (RVA 0x12E880) and the function at
0x12FA10 both read from hierarchy+0x18. The +0x20 offset in the entry is the
**localScale**, not worldPos. The real `get_position` computes worldPos by
compositing the parent chain at runtime.

**Solution**: parent chain walk with quaternion math.

1. **Probe parent indices**: scan hierarchy offsets +0x00..+0x80 for the int32 parent
   indices array. Validate by walking from childIdx to root (idx 0), checking each
   parent has a lower index. Cached in `g_ParentIndicesOff`.

2. **Walk chain**: for each bone, walk parent->root, collect localTRS at each level,
   then compose:
   ```
   worldPos = rootPos
   worldRot = rootRot
   for each child (root->bone):
       worldPos += quaternionRotate(worldRot, childLocalPos)
       worldRot = worldRot * childLocalRot
   ```

3. **Head = bone with highest world Y** among ragdollController.transforms[]

### Chain: Bone -> World Position

```
NPC (+0x98) -> RagdollController
  -> transforms (+0x20) -> Transform[] (IL2CPP array, elements at +0x20+i*8)
  -> Transform[i] -> m_CachedPtr (+0x10) -> native Transform
  -> hierarchy_ptr (+0x38), index (+0x40)
  -> parentArr = *(hierarchy_ptr + g_ParentIndicesOff)   [int32 array]
  -> dataBase = *(hierarchy_ptr + 0x18)                  [local TRS]
  -> walk chain: bone->parent->...->root, compose quaternion rotations
```

### No Recoil / No Spread

Each frame in RenderLoop:
- **No Recoil**: zero `Recoil.currentRot` (+0x20) and `Recoil.targetRot` (+0x2C)
  via Gun._recoil (+0x100)
- **No Spread**: zero `Gun.inaccuracy` (+0xE8)

### Targeting

**Priority**: closest enemy by 3D world distance (not screen distance).
FOV is checked in screen-space (only enemies inside the FOV circle are eligible),
then among those, the closest in 3D is targeted.

### ESP Head Marker

Cross (+) on each zombie's head:
- **Green** = real bone position (parent chain walk)
- **Yellow** = fallback Y offset

---

## Architecture

```
src/main.cpp             Entry point + --debug flag + restore-on-exit
src/game.cpp             Game class: IL2CPP resolution, player scan, camera, trainer, render
include/Game.h           Game class + data structures
include/Offset.h         Offsets + IL2CPP class RVAs + field offsets
src/auto_scan.cpp        Offset cache persistence (phantom_cache.ini)
src/vmmProc_rpm.cpp      ReadProcessMemory/WriteProcessMemory backend
include/settings.h       Runtime settings (ESP, aimbot, trainer)
src/overlay.cpp          DX11 + ImGui overlay
src/pattern_scanner.cpp  AOB pattern scan for GameAssembly.dll
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```
