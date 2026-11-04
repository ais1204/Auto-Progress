#include "clicker.h"

#include <algorithm>
#include <climits>

#include "timing.h"

namespace {

// pt の下にあるのがこのプロセスのウィンドウか
bool IsOwnWindowAt(POINT pt) {
    HWND w = WindowFromPoint(pt);
    if (!w) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

}  // namespace

// ---------------------------------------------------------------------------
// Clicker
// ---------------------------------------------------------------------------
bool Clicker::Start(HWND notify, const ClickerConfig& cfg) {
    Stop();
    cfg_ = cfg;
    cfg_.intervalMs = std::max(1, cfg_.intervalMs);
    cfg_.jitterMs = std::max(0, cfg_.jitterMs);
    cfg_.clicks = cfg_.clicks >= 2 ? 2 : 1;
    count_.store(0);
    return Launch(notify, [this](HANDLE stop) { return Run(stop); });
}

StopReason Clicker::Run(HANDLE stop) {
    timing::HighResolutionScope highRes;
    timing::Rng rng;

    if (cfg_.startDelayMs > 0) {
        SetWaiting(true);
        if (!timing::WaitUntil(stop, timing::Now() + timing::FromMs(cfg_.startDelayMs))) return StopReason::User;
        SetWaiting(false);
    }

    const int64_t begin = timing::Now();
    const int64_t end = begin + timing::FromMs(static_cast<double>(cfg_.durationSec) * 1000.0);
    int64_t next = begin;
    POINT lastSet{INT_MIN, INT_MIN};

    for (;;) {
        if (cfg_.repeat == ClickerConfig::Repeat::Duration && timing::Now() >= end) return StopReason::Completed;
        if (cfg_.cornerStop && input::CursorInCorner(cfg_.fixedPosition ? &lastSet : nullptr)) {
            return StopReason::Failsafe;
        }

        if (cfg_.fixedPosition) {
            const POINT target{cfg_.x, cfg_.y};
            POINT original{};
            const bool restore = cfg_.restoreCursor && GetCursorPos(&original);
            input::Click(cfg_.button, cfg_.clicks, &target, restore ? &original : nullptr);
            lastSet = restore ? original : target;
            count_.fetch_add(1);
        } else {
            POINT p{};
            // 自分のウィンドウ（停止ボタンなど）の上では誤操作防止のためクリックしない
            if (GetCursorPos(&p) && !IsOwnWindowAt(p)) {
                input::Click(cfg_.button, cfg_.clicks, nullptr);
                count_.fetch_add(1);
            }
        }

        if (cfg_.repeat == ClickerConfig::Repeat::Count && count_.load() >= cfg_.repeatCount) {
            return StopReason::Completed;
        }

        int interval = cfg_.intervalMs;
        if (cfg_.jitterMs > 0) interval += rng.Range(-cfg_.jitterMs, cfg_.jitterMs);
        if (interval < 1) interval = 1;
        next += timing::FromMs(interval);
        const int64_t now = timing::Now();
        if (next < now) next = now;  // 処理が遅れても、まとめ撃ちはしない
        if (cfg_.repeat == ClickerConfig::Repeat::Duration && next > end) next = end;
        if (!timing::WaitUntil(stop, next)) return StopReason::User;
    }
}

// ---------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------
bool Player::Start(HWND notify, std::vector<Step> steps, const PlayerConfig& cfg) {
    Stop();
    if (steps.empty()) return false;
    steps_ = std::move(steps);
    cfg_ = cfg;
    cfg_.loops = std::max(0, cfg_.loops);
    cfg_.speedPercent = std::min(10000, std::max(1, cfg_.speedPercent));
    cfg_.loopDelayMs = std::max(0, cfg_.loopDelayMs);
    loop_.store(0);
    step_.store(-1);
    return Launch(notify, [this](HANDLE stop) { return Run(stop); });
}

StopReason Player::Run(HANDLE stop) {
    timing::HighResolutionScope highRes;

    if (cfg_.startDelayMs > 0) {
        SetWaiting(true);
        if (!timing::WaitUntil(stop, timing::Now() + timing::FromMs(cfg_.startDelayMs))) return StopReason::User;
        SetWaiting(false);
    }

    const double speed = cfg_.speedPercent / 100.0;
    long long totalDelay = 0;
    for (const Step& s : steps_) totalDelay += s.delayMs;
    int loopDelay = cfg_.loopDelayMs;
    // 待機時間がすべて 0 のマクロを無限ループすると入力があふれるので最低限の間隔を空ける
    if (cfg_.loops == 0 && totalDelay == 0 && loopDelay < 10) loopDelay = 10;

    bool buttonDown[3] = {false, false, false};
    bool keyDown[256] = {};
    Step keyInfo[256];
    POINT lastSet{INT_MIN, INT_MIN};

    auto play = [&]() -> StopReason {
        int64_t t = timing::Now();
        for (int loop = 0; cfg_.loops == 0 || loop < cfg_.loops; ++loop) {
            loop_.store(loop + 1);
            if (loop > 0 && loopDelay > 0) {
                t += timing::FromMs(loopDelay);
                if (!timing::WaitUntil(stop, t)) return StopReason::User;
            }
            for (size_t i = 0; i < steps_.size(); ++i) {
                const Step& s = steps_[i];
                step_.store(static_cast<int>(i));
                t += timing::FromMs(s.delayMs / speed);
                const int64_t now = timing::Now();
                if (t < now - timing::FromMs(50)) t = now;  // 大きく遅れたら基準を取り直す
                if (!timing::WaitUntil(stop, t)) return StopReason::User;
                if (cfg_.cornerStop && input::CursorInCorner(&lastSet)) return StopReason::Failsafe;

                const POINT p{s.x, s.y};
                const int b = static_cast<int>(s.button);
                switch (s.type) {
                    case StepType::Click:
                        input::Click(s.button, s.clicks, &p);
                        lastSet = p;
                        break;
                    case StepType::Down:
                        input::Button(s.button, true, &p);
                        if (b >= 0 && b < 3) buttonDown[b] = true;
                        lastSet = p;
                        break;
                    case StepType::Up:
                        input::Button(s.button, false, &p);
                        if (b >= 0 && b < 3) buttonDown[b] = false;
                        lastSet = p;
                        break;
                    case StepType::Move:
                        input::MoveTo(p.x, p.y);
                        lastSet = p;
                        break;
                    case StepType::Wheel:
                        input::Wheel(s.wheel, &p);
                        lastSet = p;
                        break;
                    case StepType::KeyPress:
                        input::Key(s.vk, s.scan, s.extended, true);
                        input::Key(s.vk, s.scan, s.extended, false);
                        break;
                    case StepType::KeyDown:
                        input::Key(s.vk, s.scan, s.extended, true);
                        keyDown[s.vk & 0xFF] = true;
                        keyInfo[s.vk & 0xFF] = s;
                        break;
                    case StepType::KeyUp:
                        input::Key(s.vk, s.scan, s.extended, false);
                        keyDown[s.vk & 0xFF] = false;
                        break;
                    case StepType::Wait:
                        break;
                }
            }
        }
        return StopReason::Completed;
    };

    const StopReason reason = play();

    // 押しっぱなしのボタン・キーを必ず離す
    for (int b = 0; b < 3; ++b) {
        if (buttonDown[b]) input::Button(static_cast<MouseButton>(b), false, nullptr);
    }
    for (int k = 0; k < 256; ++k) {
        if (keyDown[k]) input::Key(keyInfo[k].vk, keyInfo[k].scan, keyInfo[k].extended, false);
    }
    return reason;
}
