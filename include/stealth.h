#pragma once
#include <string>
#include <random>
#include <cstdio>

// ============================================================================
// stealth.h — Utilities for Phantom (Dead From Beyond)
//
// No anti-cheat in DFB — no XOR encoding or evasion needed.
// Just debug output + random window name for overlay.
// ============================================================================

// --- Debug output (runtime flag, enabled via --debug) ---
extern bool g_DebugConsole;
#define DBG(fmt, ...) do { if (g_DebugConsole) printf(fmt, ##__VA_ARGS__); } while(0)

// --- Random window/class name generator ---
inline std::string RandomName(const char* prefix = "dwm_", int randLen = 8)
{
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(chars) - 2);
    std::string s(prefix);
    for (int i = 0; i < randLen; i++)
        s += chars[dis(gen)];
    return s;
}

// --- Game process name ---
inline std::string GetGameProcessName() { return "Dead From Beyond.exe"; }
