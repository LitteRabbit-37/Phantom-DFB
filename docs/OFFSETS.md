# Dead From Beyond — Offset Reference

Game: `Dead From Beyond.exe` v1.31 64bit (Unity IL2CPP)
Engine DLLs: `GameAssembly.dll` (~21 MB), `UnityPlayer.dll`

## Confirmed Working (from CE + ASM analysis)

### Trainer Patches (GameAssembly.dll RVAs)

| RVA | Instruction | Bytes | Effect |
|---|---|---|---|
| +0x26D1D8 | `sub [rbx+20],edi` | 29 7B 20 | Health damage — NOP = God Mode |
| +0x2B22B5 | `sub [rbx+110],eax` | 29 83 10 01 00 00 | Ammo decrement — NOP = Infinite Ammo |
| +0x294A2F | `add [rbx+30],edi` | 01 7B 30 | Money spend — NOP = Infinite Money |
| +0x276B49 | `dec [rbx+18]` | FF 4B 18 | Medkit use — NOP = Infinite Medkits |

### Entity Object Offsets

| Offset | Type | Description | Found via |
|---|---|---|---|
| +0x20 | int32 | Current health (damage subtracted here) | `sub [rbx+20],edi` at +26D1D8 |
| +0x68 | int32 | Health sync/display copy | `mov [rdi+68],eax` at +2A04E0 |

### Resource/Item Object Offsets

| Object Type | Offset | Type | Description |
|---|---|---|---|
| Resource | +0x30 | int32 | Money (negative edi = spending) |
| Item | +0x18 | int32 | Consumable count (medkits) |
| Weapon | +0x110 | int32 | Current magazine ammo (per-instance) |

### Unity IL2CPP Container Layout (List<T>)

```
List<T> object:
  +0x10: _items (pointer to T[] backing array)
  +0x18: _size (int32, current count)
  +0x1C: _version (int32)

T[] backing array:
  +0x18: max_length (int32)
  +0x20: T[0] (first element, 8 bytes for reference types)
  +0x28: T[1]
  ...
  Stride: 8 bytes per element
```

### Camera System (UnityPlayer.dll)

| Item | Detail |
|---|---|
| Camera matrix computation | `UnityPlayer.dll+2F797A`: `movups [rsp+30],xmm3` |
| Camera data source | Heap region ~0x209200Bxxxx (changes per launch) |
| Frame history buffer | VP matrices stored sequentially, stride ~0x8C (140 bytes) |
| Projection type | **Reversed-Z** (found by probe at +384 from View matrix) |

### CE Scan Results Summary

**-1/+1 sky/ground scan** found pitch-dependent values at ~-0.001745 (sin of near-horizontal):
- Stack copies at 0xC899xxxxxxx (temporary, from UnityPlayer computation)
- Heap copies at 0x209200Bxxxx (persistent, camera object data)
- Pairs at +0x8C stride = two matrix fields in same camera struct
- Array at +0x200 stride in 0x20A2B36Bxxxx (NOT the VP — 8 copies = render batch)

**Zombie count scan** (value = 3 or 22 depending on wave):
- `0x20A2DD61DC8` = GameAssembly.dll-managed List<T> (best candidate)
- `0x20A4F6E6FB0` / `0x20A4F6F0D50` = UnityPlayer.dll containers
- `0x2092009BCA0` / `0x209211E5618` / `0x209213E6218` = Unity engine trackers

## W2S Formula (current, needs validation)

```cpp
// Row-major (pos * M) with affine matrix detection:
x = wx * VP[0][0] + wy * VP[1][0] + wz * VP[2][0] + VP[3][0];
y = wx * VP[0][1] + wy * VP[1][1] + wz * VP[2][1] + VP[3][1];
w = wx * VP[0][3] + wy * VP[1][3] + wz * VP[2][3] + VP[3][3];

// If w ≈ 1 (affine View*FOV matrix without perspective):
if (|w - 1| < 0.01) {
    z = wx * VP[0][2] + wy * VP[1][2] + wz * VP[2][2] + VP[3][2];
    w = z;  // divide by depth instead
}

screenX = (x/w + 1) * screenWidth / 2
screenY = (1 - y/w) * screenHeight / 2
```

Coordinate order: **XYZ** confirmed (Unity standard). Y is up.
