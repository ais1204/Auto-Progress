#pragma once

#include <windows.h>

#include <string>

struct Hotkey {
    UINT vk = 0;    // 0 = 無効
    UINT mods = 0;  // MOD_ALT / MOD_CONTROL / MOD_SHIFT / MOD_WIN
    bool operator==(const Hotkey& o) const { return vk == o.vk && mods == o.mods; }
};

enum HotkeyId : int { kHotkeyClicker = 0, kHotkeyPlay, kHotkeyRecord, kHotkeyAddPoint, kHotkeyCount };

struct Config {
    // 連打
    int intervalValue = 100;
    int intervalUnit = 0;  // 0 = ミリ秒, 1 = 秒, 2 = 分
    int jitterMs = 0;
    int button = 0;     // 0 = 左, 1 = 右, 2 = 中
    int clickType = 0;  // 0 = シングル, 1 = ダブル
    int repeatMode = 0;  // 0 = 無限, 1 = 回数, 2 = 時間
    int repeatCount = 100;
    int durationSec = 60;
    int positionMode = 0;  // 0 = 現在位置, 1 = 指定位置
    int posX = 0;
    int posY = 0;
    bool restoreCursor = false;

    // マクロ
    bool recordMoves = false;
    bool recordKeys = false;
    bool recordAppend = false;
    int loops = 1;
    int speedPercent = 100;
    int loopDelayMs = 0;
    int editAction = 0;
    int editDelayMs = 500;

    // 共通
    bool topmost = false;
    bool compact = false;  // コンパクト表示
    int startDelaySec = 0;
    bool cornerStop = true;
    bool sound = false;
    bool trayOnMinimize = false;
    bool autoSaveMacro = true;
    int tab = 0;
    int windowX = CW_USEDEFAULT;
    int windowY = CW_USEDEFAULT;

    Hotkey hotkeys[kHotkeyCount] = {
        {VK_F6, 0},  // 連打 開始/停止
        {VK_F7, 0},  // マクロ 再生/停止
        {VK_F8, 0},  // 記録 開始/停止
        {VK_F9, 0},  // カーソル位置をステップに追加
    };
};

namespace settings {

// 設定ファイルなどを置くフォルダー（末尾に \ 付き）。
// exe と同じフォルダーに書き込めればそこ、だめなら %APPDATA%\AutoClicker\。
const std::wstring& DataDir();
std::wstring IniPath();
std::wstring LastMacroPath();

void Load(Config& cfg);
void Save(const Config& cfg);

}  // namespace settings
