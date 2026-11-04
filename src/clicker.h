#pragma once

#include <windows.h>

#include <atomic>
#include <vector>

#include "input.h"
#include "macro.h"
#include "worker.h"

enum WorkerId : int { kWorkerClicker = 1, kWorkerPlayer = 2 };

// ---------------------------------------------------------------------------
// 連打（オートクリック）
// ---------------------------------------------------------------------------
struct ClickerConfig {
    int intervalMs = 100;  // クリック間隔
    int jitterMs = 0;      // 間隔のランダム揺らぎ（±）
    MouseButton button = MouseButton::Left;
    int clicks = 1;        // 1 = シングル, 2 = ダブル

    enum class Repeat { Infinite, Count, Duration };
    Repeat repeat = Repeat::Infinite;
    long long repeatCount = 100;
    int durationSec = 60;

    bool fixedPosition = false;  // false = 現在のカーソル位置
    int x = 0;
    int y = 0;
    bool restoreCursor = false;  // 指定位置クリック後にカーソルを戻す

    int startDelayMs = 0;
    bool cornerStop = true;
    HWND ownWindow = nullptr;  // このウィンドウ上ではクリックしない
};

class Clicker : public Worker {
public:
    Clicker() : Worker(kWorkerClicker) {}
    bool Start(HWND notify, const ClickerConfig& cfg);
    long long Count() const { return count_.load(); }
    const ClickerConfig& Config() const { return cfg_; }

private:
    StopReason Run(HANDLE stopEvent);

    ClickerConfig cfg_;
    std::atomic<long long> count_{0};
};

// ---------------------------------------------------------------------------
// マクロ再生
// ---------------------------------------------------------------------------
struct PlayerConfig {
    int loops = 1;          // 0 = 無限
    int speedPercent = 100;  // 100 = 記録どおり, 200 = 2 倍速
    int loopDelayMs = 0;     // ループの間の待機
    int startDelayMs = 0;
    bool cornerStop = true;
};

class Player : public Worker {
public:
    Player() : Worker(kWorkerPlayer) {}
    bool Start(HWND notify, std::vector<Step> steps, const PlayerConfig& cfg);
    int CurrentLoop() const { return loop_.load(); }
    int CurrentStep() const { return step_.load(); }  // 0 始まり、未実行なら -1
    int StepCount() const { return static_cast<int>(steps_.size()); }
    const PlayerConfig& Config() const { return cfg_; }

private:
    StopReason Run(HANDLE stopEvent);

    std::vector<Step> steps_;
    PlayerConfig cfg_;
    std::atomic<int> loop_{0};
    std::atomic<int> step_{-1};
};
