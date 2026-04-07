@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist build mkdir build
if not exist build\obj mkdir build\obj
echo [*] Compiling Phantom DFB...
cl.exe /O2 /EHsc /std:c++17 /utf-8 /W3 /nologo ^
    /I"include" /I"vendor" /I"vendor\imgui" ^
    src\main.cpp src\game.cpp src\auto_scan.cpp src\overlay.cpp ^
    src\vmmProc_rpm.cpp src\pattern_scanner.cpp ^
    vendor\imgui\imgui.cpp vendor\imgui\imgui_draw.cpp ^
    vendor\imgui\imgui_impl_dx11.cpp vendor\imgui\imgui_impl_win32.cpp ^
    vendor\imgui\imgui_tables.cpp vendor\imgui\imgui_widgets.cpp ^
    d3d11.lib dwmapi.lib advapi32.lib ^
    /Fe:"build\Phantom_DFB.exe" /Fo:"build\obj\\"
if %ERRORLEVEL% == 0 (
    echo [+] Build successful: build\Phantom_DFB.exe
    del /q "build\obj\*" >nul 2>&1
    rmdir "build\obj" >nul 2>&1
) else (
    echo [-] Build failed with error %ERRORLEVEL%
)
