#pragma once
#include <Windows.h>

// settings.h — Runtime-configurable options for Phantom (Dead From Beyond).
// g_Settings is defined in game.cpp and accessible from anywhere that includes this file.

struct AimbotSettings
{
    bool  enabled       = false;
    int   hotkey        = VK_RBUTTON;   // Key that activates aim (default: RMB)
    float fov           = 100.f;        // FOV radius in screen pixels
    float maxDistance    = 300.f;        // Max engagement distance (metres)
    float smooth        = 5.f;          // Steps to target (1=instant, 20=very slow)
    float sensitivity   = 1.0f;         // Mouse calibration factor (pixel → mickey)
                                        //   < 1.0 = overshoots → increase this
                                        //   > 1.0 = undershoots → decrease this
                                        //   Calibrate once, then leave it
    bool  showFovCircle = false;        // Draw FOV circle on overlay
    bool  noRecoil      = false;        // Zero recoil each frame
    bool  noSpread      = false;        // Zero inaccuracy each frame
    int   targetBone    = 0;            // 0=Head, 1=Torso
    float headOffset    = 1.7f;         // Fallback Y offset for head (no bones)
    float torsoOffset   = 0.9f;         // Fallback Y offset for torso (no bones)
};

struct EspSettings
{
    bool  showPlayers   = true;
    bool  showDistance   = true;
    float maxDistance    = 500.f;        // metres
    int   colorEnemy    = 32896;        // COLOR_TEAL  (0x008080)
    int   colorClose    = 16711680;     // COLOR_RED   (0xFF0000) — under 50 m
};

struct Settings
{
    AimbotSettings aimbot;
    EspSettings    esp;

    bool menuOpen = false;
    int  menuKey  = VK_END;              // Toggle menu open/close
};

// Defined in game.cpp
extern Settings g_Settings;
