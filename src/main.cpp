#include "main.h"
#include <cstring>

bool g_DebugConsole = true;

int main(int argc, char* argv[])
{
    // Parse --debug flag
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--debug") == 0)
            g_DebugConsole = true;
    }

    if (g_DebugConsole)
    {
        AllocConsole();
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        printf("[*] Phantom DFB — Debug Console\n");
    }

    MSG Msg;
    Game ov1 = Game();
    ov1.Start();

    while (::GetMessage(&Msg, 0, 0, 0))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ::TranslateMessage(&Msg);
        ::DispatchMessage(&Msg);
    }

    ov1.RestoreAllPatches();
    return 0;
}
