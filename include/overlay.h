#pragma once
#include <Windows.h>
#include <Dwmapi.h>
#include <d3d11.h>
#include <string>
#include <chrono>
#include <thread>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")

// Color constants in 0xRRGGBB format
#define COLOR_BLUE     255       // 0x0000FF
#define COLOR_CYAN     65535     // 0x00FFFF
#define COLOR_BLACK    0         // 0x000000
#define COLOR_OLIVE    8421376   // 0x808000
#define COLOR_SKY_BLUE 33023     // 0x0080FF
#define COLOR_ORANGE   16746496  // 0xFF8000
#define COLOR_RED      16711680  // 0xFF0000
#define COLOR_GREEN    65280     // 0x00FF00
#define COLOR_WHITE    16777215  // 0xFFFFFF
#define COLOR_GRAY     8409343   // 0x806F7F
#define COLOR_TEAL     32896     // 0x008080
#define COLOR_AMBER    16753920  // 0xFFA500
#define COLOR_AZURE    49151     // 0x00BFFF
#define COLOR_PINK     16738740  // 0xFF69B4

class Overlay
{
public:
	void Start();
	DWORD CreateOverlay();
	void BeginDraw();
	void EndDraw();
	void ClickThrough(bool v);
	void DrawNewText(int X, int Y, int Color, float Size, const char* Str, ...);
	void DrawCircleFilled(int X, int Y, float Radius, int Color, int Segments, int tmz);
	void DrawCircle(int X, int Y, float Radius, int Color, int Segments, float thickness);
	void DrawRect(int X, int Y, int W, int H, int Color, float thickness, float rounding);
	void DrawFilledRect(int X, int Y, int W, int H, int Color, int tmz, float rounding);
	void DrawLine(int X1, int Y1, int X2, int Y2, int Color, float thickness);

public:
	bool running;
	HWND overlayHWND;
	HWND GameWindow;
	WINDOWINFO pwi;
	ULONG WindowX;
	ULONG WindowY;
	ULONG GameCenterW;
	ULONG GameCenterH;
	ULONG GameCenterX;
	ULONG GameCenterY;
};
