# Phantom-DFB

External overlay for **Dead From Beyond** (v1.31 64-bit, solo mode).

Pure `ReadProcessMemory` — no DLL injection, no driver.

## Features

### ESP

- Zombie positions via Unity TransformHierarchy
- Distance display
- Color-coded proximity

### Aimbot

- Head targeting via bone reading (parent chain walk with quaternion math)
- Torso targeting (offset fallback)
- Configurable FOV, smoothing, sensitivity
- No Recoil / No Spread

### Trainer (AOB patches)

- God Mode
- Infinite Ammo
- Infinite Money
- Infinite Medkits
- Full Auto

### Player Cheats (IL2CPP memory)

- Wave, HP, Money, Grenades
- Walk/Sprint speed, Jump force, Extra jumps
- Fire rate, Gun damage, Clip size
- Damage/Points multipliers
- Zombie HP/Damage/Speed

## Build

**Requirements:** CMake 3.16+, MSVC (Visual Studio 2022 Build Tools)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output: `build/Release/Phantom_DFB.exe`

## Usage

1. Launch `Phantom_DFB.exe` (before or after the game)
2. Start or join a solo game
3. Press **END** to toggle the overlay menu
4. Press **DELETE** to quit Phantom

Run with `--debug` for a debug console:

```bash
Phantom_DFB.exe --debug
```

## How It Works

Phantom reads game memory externally via Win32 `ReadProcessMemory`. It resolves IL2CPP class metadata and GC heap objects to find player data, zombie positions, and camera matrices. The DX11 overlay renders ESP markers and the ImGui configuration menu.

See [docs/INTERNALS.md](docs/INTERNALS.md) for the full technical reference.

## License

[AGPLv3](LICENSE) — Free to use, modify, and distribute. Any derivative work must also be open source.
