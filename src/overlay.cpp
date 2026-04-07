#include "overlay.h"
#include "stealth.h"
#include <ShellScalingApi.h>
#pragma comment(lib, "Shcore.lib")

// Forward declare ImGui Win32 handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Custom WndProc that forwards input to ImGui
static LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

const MARGINS margins = { -1 ,-1, -1, -1 };
static std::string g_randomClassName;
static std::string g_randomWindowTitle;
WNDCLASSEX wc;

LONG nv_default = WS_POPUP | WS_CLIPSIBLINGS;
LONG nv_default_in_game = nv_default | WS_DISABLED;
LONG nv_edit = nv_default_in_game | WS_VISIBLE;

LONG nv_ex_default = WS_EX_TOOLWINDOW;
LONG nv_ex_edit = nv_ex_default | WS_EX_LAYERED | WS_EX_TRANSPARENT;
LONG nv_ex_edit_menu = nv_ex_default | WS_EX_TRANSPARENT;

static ID3D11Device* g_pd3dDevice = NULL;
static ID3D11DeviceContext* g_pd3dDeviceContext = NULL;
static IDXGISwapChain* g_pSwapChain = NULL;
static ID3D11RenderTargetView* g_mainRenderTargetView = NULL;

ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 0.00f);

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();

// Strings are already UTF-8. Pass through directly.
static const std::string& AnisToUTF8(const std::string& Str)
{
	return Str;
}

static DWORD WINAPI StaticMessageStart(void* Param)
{
	Overlay* ov = (Overlay*)Param;
	ov->CreateOverlay();
	return 0;
}

void Overlay::Start()
{
	DWORD ThreadID;
	CreateThread(NULL, 0, StaticMessageStart, (void*)this, 0, &ThreadID);
}

void Overlay::ClickThrough(bool v)
{
	if (v) {
		nv_edit = nv_default_in_game | WS_VISIBLE;
		if (GetWindowLong(overlayHWND, GWL_EXSTYLE) != nv_ex_edit)
			SetWindowLong(overlayHWND, GWL_EXSTYLE, nv_ex_edit);
	}
	else {
		nv_edit = nv_default | WS_VISIBLE;
		if (GetWindowLong(overlayHWND, GWL_EXSTYLE) != nv_ex_edit_menu)
			SetWindowLong(overlayHWND, GWL_EXSTYLE, nv_ex_edit_menu);
	}
}

DWORD Overlay::CreateOverlay()
{
	// Fix DPI scaling — prevents cursor offset on Windows 11 (125%/150%)
	SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

	pwi.cbSize = 60;
	GetWindowInfo(GameWindow, &pwi);
	WindowX = pwi.rcClient.left;
	WindowY = pwi.rcClient.top;
	GameCenterW = pwi.rcClient.right - WindowX;
	GameCenterH = pwi.rcClient.bottom - WindowY;

	g_randomClassName  = RandomName("wnd_", 10);
	g_randomWindowTitle = RandomName("ms_", 10);

	wc.cbSize = sizeof(WNDCLASSEX);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = OverlayWndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = NULL;
	wc.hIcon = NULL;
	wc.hCursor = NULL;
	wc.hbrBackground = (HBRUSH)(RGB(0, 0, 0));
	wc.lpszMenuName = NULL;
	wc.lpszClassName = g_randomClassName.c_str();
	wc.hIconSm = NULL;

	RegisterClassEx(&wc);

	overlayHWND = CreateWindowEx(WS_EX_LAYERED | WS_EX_TRANSPARENT, g_randomClassName.c_str(), g_randomWindowTitle.c_str(), WS_POPUP | WS_VISIBLE, WindowX, WindowY, GameCenterW, GameCenterH, NULL, NULL, NULL, NULL);

	if (overlayHWND == 0)
		return 0;

	SetLayeredWindowAttributes(overlayHWND, RGB(0, 0, 0), 255, LWA_ALPHA);

	DwmExtendFrameIntoClientArea(overlayHWND, &margins);

	// Initialize Direct3D
	if (!CreateDeviceD3D(overlayHWND))
	{
		CleanupDeviceD3D();
		return 0;
	}

	// Show the window
	::ShowWindow(overlayHWND, SW_SHOWDEFAULT);
	::UpdateWindow(overlayHWND);

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	ImGui::GetStyle().WindowMinSize = ImVec2(1, 1);

	// Setup Platform/Renderer bindings
	ImGui_ImplWin32_Init(overlayHWND);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

	running = true;

	// Main loop
	MSG msg;
	ZeroMemory(&msg, sizeof(msg));
	ClickThrough(true);

	while (running)
	{
		// Force overlay to stay on top (HWND_TOPMOST)
		::SetWindowPos(overlayHWND, HWND_TOPMOST, 0, 0, 0, 0,
		               SWP_ASYNCWINDOWPOS | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);


		if (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
			continue;
		}
		GetWindowInfo(GameWindow, &pwi);
		WindowX = pwi.rcClient.left;
		WindowY = pwi.rcClient.top;
		GameCenterW = pwi.rcClient.right - WindowX;
		GameCenterH = pwi.rcClient.bottom - WindowY;
		GameCenterX = GameCenterW / 2;
		GameCenterY = GameCenterH / 2;
		MoveWindow(overlayHWND, WindowX, WindowY, GameCenterW, GameCenterH, false);

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ClickThrough(true);
	CleanupDeviceD3D();
	::DestroyWindow(overlayHWND);
	return 0;
}

void Overlay::BeginDraw()
{
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	// draw FPS
	char dist[64];
	sprintf_s(dist, "(%.1f FPS)\n", ImGui::GetIO().Framerate);
	DrawNewText(15, 15, COLOR_GREEN, 18, dist);
}

void Overlay::EndDraw()
{
	ImGui::EndFrame();
	ImGui::Render();
	const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
	g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
	g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	g_pSwapChain->Present(1, 0);
}

void CreateRenderTarget()
{
	ID3D11Texture2D* pBackBuffer;
	g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	if (pBackBuffer)
	{
		g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
		pBackBuffer->Release();
	}
}

bool CreateDeviceD3D(HWND hWnd)
{
	DXGI_SWAP_CHAIN_DESC sd;
	ZeroMemory(&sd, sizeof(sd));
	sd.BufferCount = 2;
	sd.BufferDesc.Width = 0;
	sd.BufferDesc.Height = 0;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 0;     // let DXGI use monitor default
	sd.BufferDesc.RefreshRate.Denominator = 0;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hWnd;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	UINT createDeviceFlags = 0;
	D3D_FEATURE_LEVEL featureLevel;
	const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
	if (D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
		return false;

	CreateRenderTarget();
	return true;
}

void CleanupRenderTarget()
{
	if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}

void CleanupDeviceD3D()
{
	CleanupRenderTarget();
	if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
	if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
	if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
}

void Overlay::DrawNewText(int X, int Y, int Color, float Size, const char* txt, ...)
{
	char str[128];

	va_list va_alist;
	va_start(va_alist, txt);
	_vsnprintf_s(str, sizeof(str), _TRUNCATE, txt, va_alist);
	va_end(va_alist);

	std::string UTF8 = AnisToUTF8(std::string(str));

	ImGui::GetForegroundDrawList()->AddTextEx(ImVec2(X, Y), Size, ImGui::ImAlphaBlendColors(Color, 255), UTF8.c_str());
}


void Overlay::DrawCircleFilled(int X, int Y, float Radius, int Color, int Segments, int tmz)
{
	ImGui::GetForegroundDrawList()->AddCircleFilled(ImVec2(X, Y), Radius, ImGui::ImAlphaBlend(Color, 255, tmz), Segments);
}

void Overlay::DrawCircle(int X, int Y, float Radius, int Color, int Segments, float thickness)
{
	ImGui::GetForegroundDrawList()->AddCircle(ImVec2(X, Y), Radius, ImGui::ImAlphaBlendColors(Color, 255), Segments, thickness);
}

void Overlay::DrawRect(int X, int Y, int W, int H, int Color, float thickness, float rounding)
{
	ImGui::GetForegroundDrawList()->AddRect(ImVec2(X, Y), ImVec2(X + W, Y + H), ImGui::ImAlphaBlendColors(Color, 255), rounding, 15, thickness);
}

void Overlay::DrawFilledRect(int X, int Y, int W, int H, int Color, int tmz, float rounding)
{
	ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(X, Y), ImVec2(X + W, Y + H), ImGui::ImAlphaBlend(Color, 255, tmz), rounding);
}

void Overlay::DrawLine(int X1, int Y1, int X2, int Y2, int Color, float thickness)
{
	ImGui::GetForegroundDrawList()->AddLine(ImVec2(X1, Y1), ImVec2(X2, Y2), ImGui::ImAlphaBlendColors(Color, 255), thickness);
}