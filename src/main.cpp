#include <windows.h>

#include "app.h"

namespace {

// マニフェストで Per-Monitor V2 を指定しているが、念のためコードでも設定する
// （マニフェストが効いている場合は失敗するだけで害はない）
void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    using SetContextFn = BOOL(WINAPI*)(HANDLE);
    auto setContext = reinterpret_cast<SetContextFn>(
        reinterpret_cast<void (*)(void)>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    if (setContext) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == ((DPI_AWARENESS_CONTEXT)-4)
        if (setContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return;
        if (GetLastError() == ERROR_ACCESS_DENIED) return;  // すでに設定済み
    }
    SetProcessDPIAware();
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    EnableDpiAwareness();
    return RunApp(instance, showCommand);
}
