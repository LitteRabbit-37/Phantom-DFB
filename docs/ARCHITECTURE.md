# Phantom DFB — Architecture

## File Map

| File | Role |
|------|------|
| `game.cpp` | Main logic: GameStart, EntityLoop, RenderLoop, W2S, VP scan, trainer |
| `Game.h` | Structures (Vector3, Matrix4x4, Player) + Game class |
| `Offset.h` | All offsets + ResolveChain() |
| `settings.h` | AimbotSettings, EspSettings, menuKey |
| `stealth.h` | DBG macro, process name, RandomName |
| `overlay.cpp/h` | DX11 window, ImGui init, draw primitives, WndProc |
| `pattern_scanner.cpp/h` | AOB scan for GameAssembly.dll |
| `vmmProc.h` | Memory backend API |
| `vmmProc_rpm.cpp` | ReadProcessMemory backend (active) |
| `auto_scan.h/cpp` | Offset cache persistence (phantom_cache.ini) |
| `main.cpp` | Entry point, console, message pump |

## Thread Model

```
main thread        -> message pump (GetMessage loop)
  |
  +-- GameStart()  -> init RPM backend, find Dead From Beyond.exe,
        |             AOB scan GameAssembly.dll, VP scan, entity scan
        |
        +-- EntityLoop thread  [80ms tick]
        |     Read entity list from IL2CPP List<T>
        |     Follow pointer chains for positions
        |     Update g_Players vector (mutex-protected)
        |
        +-- RenderLoop thread  [~1ms tick]
              ReadMVP() -> fetch ViewProjection matrix
              For each player: WorldToScreen -> DrawPlayer
              Aimbot: SendInput mouse moves
              ImGui menu (ESP, Aimbot, Trainer tabs)
```

## Data Flow

```
Memory (Dead From Beyond)     Phantom
   |                            |
   +-- GameAssembly.dll ------> Trainer patches (NOP via AOB)
   +-- Heap (VP matrix) -----> VP Matrix (16 floats, auto-scanned)
   +-- IL2CPP List<T> -------> Entity list (CE hint address)
   +-- Entity chain ----------> Zombie positions (XYZ)
   |                            |
   |                       WorldToScreen()
   |                            |
   |                       Screen coordinates
   |                            |
   |                       DX11 overlay (ImGui)
```

## Memory Backend

| Backend | File | Requires |
|---------|------|----------|
| RPM | vmmProc_rpm.cpp | Nothing (no anti-cheat) |

Implements the `vmmProc.h` interface:
`ReadMemory`, `GetModuleFromName`, `GetModuleSize`, etc.

## Build

```bat
build_rpm.bat  ->  build\Phantom_DFB.exe
```

Compiler: MSVC cl.exe (`/O2 /EHsc /std:c++17 /utf-8`)

## Overlay

- Window name randomized (`dwm_XXXXXXXX`)
- Transparent, click-through, topmost
- DPI-aware to prevent cursor offset
- END key toggles menu
