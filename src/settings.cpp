#include "settings.h"

#include <shlobj.h>

#include <algorithm>
#include <climits>
#include <cwchar>
#include <iterator>

namespace settings {
namespace {

bool CanWriteTo(const std::wstring& dir) {
    const std::wstring probe = dir + L"AutoClicker.write-test";
    HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    CloseHandle(f);
    return true;
}

std::wstring ComputeDataDir() {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (len > 0 && len < std::size(path)) {
        std::wstring dir(path, len);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dir.resize(slash + 1);
            if (CanWriteTo(dir)) return dir;
        }
    }
    // Program Files など書き込めない場所に置かれた場合
    wchar_t appData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData))) {
        std::wstring dir = std::wstring(appData) + L"\\AutoClicker\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    return L".\\";
}

int ReadInt(const wchar_t* section, const wchar_t* key, int def, int lo, int hi) {
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(section, key, L"", buf, 32, IniPath().c_str());
    if (buf[0] == L'\0') return def;
    wchar_t* end = nullptr;
    const long v = std::wcstol(buf, &end, 10);
    if (end == buf) return def;
    return static_cast<int>(std::min<long>(hi, std::max<long>(lo, v)));
}

bool ReadBool(const wchar_t* section, const wchar_t* key, bool def) {
    return ReadInt(section, key, def ? 1 : 0, 0, 1) != 0;
}

void WriteInt(const wchar_t* section, const wchar_t* key, long long v) {
    WritePrivateProfileStringW(section, key, std::to_wstring(v).c_str(), IniPath().c_str());
}

const wchar_t* const kHotkeyKeys[kHotkeyCount] = {L"Clicker", L"Play", L"Record", L"AddPoint"};

}  // namespace

const std::wstring& DataDir() {
    static const std::wstring dir = ComputeDataDir();
    return dir;
}

std::wstring IniPath() { return DataDir() + L"AutoClicker.ini"; }
std::wstring LastMacroPath() { return DataDir() + L"last_macro.acm"; }

void Load(Config& c) {
    const int kMax = 1000000000;
    c.intervalValue = ReadInt(L"Clicker", L"Interval", c.intervalValue, 1, kMax);
    c.intervalUnit = ReadInt(L"Clicker", L"IntervalUnit", c.intervalUnit, 0, 2);
    c.jitterMs = ReadInt(L"Clicker", L"Jitter", c.jitterMs, 0, kMax);
    c.button = ReadInt(L"Clicker", L"Button", c.button, 0, 2);
    c.clickType = ReadInt(L"Clicker", L"ClickType", c.clickType, 0, 1);
    c.repeatMode = ReadInt(L"Clicker", L"RepeatMode", c.repeatMode, 0, 2);
    c.repeatCount = ReadInt(L"Clicker", L"RepeatCount", c.repeatCount, 1, kMax);
    c.durationSec = ReadInt(L"Clicker", L"Duration", c.durationSec, 1, kMax);
    c.positionMode = ReadInt(L"Clicker", L"PositionMode", c.positionMode, 0, 1);
    c.posX = ReadInt(L"Clicker", L"X", c.posX, -kMax, kMax);
    c.posY = ReadInt(L"Clicker", L"Y", c.posY, -kMax, kMax);
    c.restoreCursor = ReadBool(L"Clicker", L"RestoreCursor", c.restoreCursor);

    c.recordMoves = ReadBool(L"Macro", L"RecordMoves", c.recordMoves);
    c.recordKeys = ReadBool(L"Macro", L"RecordKeys", c.recordKeys);
    c.recordAppend = ReadBool(L"Macro", L"RecordAppend", c.recordAppend);
    c.loops = ReadInt(L"Macro", L"Loops", c.loops, 0, kMax);
    c.speedPercent = ReadInt(L"Macro", L"Speed", c.speedPercent, 1, 10000);
    c.loopDelayMs = ReadInt(L"Macro", L"LoopDelay", c.loopDelayMs, 0, kMax);
    c.editAction = ReadInt(L"Macro", L"EditAction", c.editAction, 0, 100);
    c.editDelayMs = ReadInt(L"Macro", L"EditDelay", c.editDelayMs, 0, kMax);

    c.topmost = ReadBool(L"General", L"Topmost", c.topmost);
    c.startDelaySec = ReadInt(L"General", L"StartDelay", c.startDelaySec, 0, 3600);
    c.cornerStop = ReadBool(L"General", L"CornerStop", c.cornerStop);
    c.sound = ReadBool(L"General", L"Sound", c.sound);
    c.trayOnMinimize = ReadBool(L"General", L"TrayOnMinimize", c.trayOnMinimize);
    c.autoSaveMacro = ReadBool(L"General", L"AutoSaveMacro", c.autoSaveMacro);
    c.tab = ReadInt(L"General", L"Tab", c.tab, 0, 2);
    c.windowX = ReadInt(L"General", L"WindowX", c.windowX, INT_MIN, INT_MAX);
    c.windowY = ReadInt(L"General", L"WindowY", c.windowY, INT_MIN, INT_MAX);

    for (int i = 0; i < kHotkeyCount; ++i) {
        const std::wstring vkKey = std::wstring(kHotkeyKeys[i]) + L"Key";
        const std::wstring modKey = std::wstring(kHotkeyKeys[i]) + L"Mods";
        c.hotkeys[i].vk = static_cast<UINT>(ReadInt(L"Hotkeys", vkKey.c_str(), static_cast<int>(c.hotkeys[i].vk), 0, 255));
        c.hotkeys[i].mods = static_cast<UINT>(ReadInt(L"Hotkeys", modKey.c_str(), static_cast<int>(c.hotkeys[i].mods), 0, 15));
    }
}

void Save(const Config& c) {
    WriteInt(L"Clicker", L"Interval", c.intervalValue);
    WriteInt(L"Clicker", L"IntervalUnit", c.intervalUnit);
    WriteInt(L"Clicker", L"Jitter", c.jitterMs);
    WriteInt(L"Clicker", L"Button", c.button);
    WriteInt(L"Clicker", L"ClickType", c.clickType);
    WriteInt(L"Clicker", L"RepeatMode", c.repeatMode);
    WriteInt(L"Clicker", L"RepeatCount", c.repeatCount);
    WriteInt(L"Clicker", L"Duration", c.durationSec);
    WriteInt(L"Clicker", L"PositionMode", c.positionMode);
    WriteInt(L"Clicker", L"X", c.posX);
    WriteInt(L"Clicker", L"Y", c.posY);
    WriteInt(L"Clicker", L"RestoreCursor", c.restoreCursor);

    WriteInt(L"Macro", L"RecordMoves", c.recordMoves);
    WriteInt(L"Macro", L"RecordKeys", c.recordKeys);
    WriteInt(L"Macro", L"RecordAppend", c.recordAppend);
    WriteInt(L"Macro", L"Loops", c.loops);
    WriteInt(L"Macro", L"Speed", c.speedPercent);
    WriteInt(L"Macro", L"LoopDelay", c.loopDelayMs);
    WriteInt(L"Macro", L"EditAction", c.editAction);
    WriteInt(L"Macro", L"EditDelay", c.editDelayMs);

    WriteInt(L"General", L"Topmost", c.topmost);
    WriteInt(L"General", L"StartDelay", c.startDelaySec);
    WriteInt(L"General", L"CornerStop", c.cornerStop);
    WriteInt(L"General", L"Sound", c.sound);
    WriteInt(L"General", L"TrayOnMinimize", c.trayOnMinimize);
    WriteInt(L"General", L"AutoSaveMacro", c.autoSaveMacro);
    WriteInt(L"General", L"Tab", c.tab);
    WriteInt(L"General", L"WindowX", c.windowX);
    WriteInt(L"General", L"WindowY", c.windowY);

    for (int i = 0; i < kHotkeyCount; ++i) {
        const std::wstring vkKey = std::wstring(kHotkeyKeys[i]) + L"Key";
        const std::wstring modKey = std::wstring(kHotkeyKeys[i]) + L"Mods";
        WriteInt(L"Hotkeys", vkKey.c_str(), c.hotkeys[i].vk);
        WriteInt(L"Hotkeys", modKey.c_str(), c.hotkeys[i].mods);
    }
}

}  // namespace settings
