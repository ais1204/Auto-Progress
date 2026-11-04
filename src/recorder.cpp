#include "recorder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "timing.h"

namespace recorder {
namespace {

enum class Mode { None, Record, Pick };

// フックで受け取った生のイベント
struct Raw {
    enum Kind { Move, Down, Up, Wheel, KeyDown, KeyUp };
    Kind kind = Move;
    MouseButton button = MouseButton::Left;
    POINT pt{};
    int wheel = 0;
    WORD vk = 0;
    WORD scan = 0;
    bool extended = false;
    double timeMs = 0;
};

constexpr size_t kMaxEvents = 200000;   // 記録できるイベント数の上限
constexpr double kMoveIntervalMs = 15;  // マウス移動を間引く間隔
constexpr int kClickSlop = 4;           // これ以内の移動ならドラッグではなくクリックとみなす

Mode g_mode = Mode::None;
HHOOK g_mouseHook = nullptr;
HHOOK g_keyHook = nullptr;

// 記録用
Options g_opt;
std::vector<Raw> g_raw;
double g_startMs = 0;
double g_lastMoveMs = -1e9;
bool g_hasPendingMove = false;
Raw g_pendingMove;
bool g_ignoreUp[3] = {};
bool g_keyIsDown[256] = {};

// 位置選択用
HWND g_pickNotify = nullptr;
bool g_pickFinished = false;
bool g_pickSwallowLeftUp = false;
bool g_pickSwallowRightUp = false;
POINT g_pickPoint{};

double NowMs() { return timing::ToMs(timing::Now()); }

bool IsOwnProcessWindow(HWND w) {
    if (!w) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

int Distance(POINT a, POINT b) {
    return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

void Push(const Raw& r) {
    if (g_raw.size() >= kMaxEvents) return;
    // このツール自身のウィンドウ上でのマウス移動は記録しない（停止ボタンへ向かう動きなど）
    if (r.kind == Raw::Move) {
        if (IsOwnProcessWindow(WindowFromPoint(r.pt))) return;
        // 直前と同じ位置への移動は不要
        if (!g_raw.empty() && g_raw.back().kind == Raw::Move && g_raw.back().pt.x == r.pt.x &&
            g_raw.back().pt.y == r.pt.y) {
            return;
        }
    }
    g_raw.push_back(r);
}

void FlushPendingMove() {
    if (g_hasPendingMove) {
        g_hasPendingMove = false;
        Push(g_pendingMove);
    }
}

void RecordMouse(WPARAM msg, const MSLLHOOKSTRUCT* ms) {
    Raw r;
    r.pt = ms->pt;
    r.timeMs = NowMs();

    int button = -1;
    bool down = false;
    switch (msg) {
        case WM_MOUSEMOVE:
            if (!g_opt.recordMoves) return;
            r.kind = Raw::Move;
            if (r.timeMs - g_lastMoveMs < kMoveIntervalMs) {
                // 間引き対象。最後の位置だけは後で使えるように保持しておく
                g_pendingMove = r;
                g_hasPendingMove = true;
                return;
            }
            g_hasPendingMove = false;
            g_lastMoveMs = r.timeMs;
            Push(r);
            return;
        case WM_MOUSEWHEEL:
            if (IsOwnProcessWindow(WindowFromPoint(ms->pt))) return;
            FlushPendingMove();
            r.kind = Raw::Wheel;
            r.wheel = static_cast<short>(HIWORD(ms->mouseData));
            Push(r);
            return;
        case WM_LBUTTONDOWN: button = 0; down = true; break;
        case WM_LBUTTONUP: button = 0; break;
        case WM_RBUTTONDOWN: button = 1; down = true; break;
        case WM_RBUTTONUP: button = 1; break;
        case WM_MBUTTONDOWN: button = 2; down = true; break;
        case WM_MBUTTONUP: button = 2; break;
        default: return;
    }

    if (down) {
        // このツールのボタン（記録停止など）を押した操作は記録しない
        if (IsOwnProcessWindow(WindowFromPoint(ms->pt))) {
            g_ignoreUp[button] = true;
            return;
        }
    } else if (g_ignoreUp[button]) {
        g_ignoreUp[button] = false;
        return;
    }

    FlushPendingMove();
    r.kind = down ? Raw::Down : Raw::Up;
    r.button = static_cast<MouseButton>(button);
    Push(r);
}

void RecordKey(WPARAM msg, const KBDLLHOOKSTRUCT* ks) {
    const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const bool up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
    if (!down && !up) return;

    const UINT vk = ks->vkCode & 0xFF;
    for (UINT ignore : g_opt.ignoreVks) {
        if (ignore == vk) return;
    }
    // このツールの入力欄への入力は記録しない
    if (IsOwnProcessWindow(GetForegroundWindow())) return;

    if (down) {
        if (g_keyIsDown[vk]) return;  // 押しっぱなしによるキーリピートは無視
        g_keyIsDown[vk] = true;
    } else {
        g_keyIsDown[vk] = false;
    }

    FlushPendingMove();
    Raw r;
    r.kind = down ? Raw::KeyDown : Raw::KeyUp;
    r.vk = static_cast<WORD>(vk);
    r.scan = static_cast<WORD>(ks->scanCode);
    r.extended = (ks->flags & LLKHF_EXTENDED) != 0;
    r.timeMs = NowMs();
    Push(r);
}

bool HandlePickMouse(WPARAM msg, const MSLLHOOKSTRUCT* ms) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            if (g_pickFinished) return false;
            g_pickPoint = ms->pt;
            g_pickSwallowLeftUp = true;
            return true;
        case WM_LBUTTONUP:
            if (!g_pickSwallowLeftUp) return false;
            g_pickSwallowLeftUp = false;
            if (!g_pickFinished) {
                g_pickFinished = true;
                PostMessageW(g_pickNotify, WM_APP_PICK_DONE, static_cast<WPARAM>(static_cast<INT_PTR>(g_pickPoint.x)),
                             static_cast<LPARAM>(g_pickPoint.y));
            }
            return true;
        case WM_RBUTTONDOWN:
            if (g_pickFinished) return false;
            g_pickSwallowRightUp = true;
            return true;
        case WM_RBUTTONUP:
            if (!g_pickSwallowRightUp) return false;
            g_pickSwallowRightUp = false;
            if (!g_pickFinished) {
                g_pickFinished = true;
                PostMessageW(g_pickNotify, WM_APP_PICK_CANCEL, 0, 0);
            }
            return true;
        default:
            return false;
    }
}

LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        // SendInput で送った（再生中などの）入力は対象外
        if ((ms->flags & LLMHF_INJECTED) == 0) {
            if (g_mode == Mode::Pick) {
                if (HandlePickMouse(wp, ms)) return 1;  // 対象アプリにクリックを渡さない
            } else if (g_mode == Mode::Record) {
                RecordMouse(wp, ms);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* ks = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if ((ks->flags & LLKHF_INJECTED) == 0) {
            if (g_mode == Mode::Pick) {
                if (ks->vkCode == VK_ESCAPE && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
                    if (!g_pickFinished) {
                        g_pickFinished = true;
                        PostMessageW(g_pickNotify, WM_APP_PICK_CANCEL, 0, 0);
                    }
                    return 1;
                }
            } else if (g_mode == Mode::Record) {
                RecordKey(wp, ks);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void Unhook() {
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_keyHook) UnhookWindowsHookEx(g_keyHook);
    g_mouseHook = nullptr;
    g_keyHook = nullptr;
}

bool Hook(bool keyboard) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, inst, 0);
    if (!g_mouseHook) return false;
    if (keyboard) {
        g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, inst, 0);
        if (!g_keyHook) {
            Unhook();
            return false;
        }
    }
    return true;
}

// 生のイベント列を、編集しやすいステップ列に整形する
std::vector<Step> Process(const std::vector<Raw>& raw, double startMs) {
    // 1) 対になっていない押下 / 解放を取り除く
    //    （記録開始・終了のホットキーの修飾キーや、記録開始前から押していたボタンなど）
    const size_t n = raw.size();
    std::vector<char> keep(n, 1);
    std::vector<long long> open(256 + 3, -1);  // 0..255 = 仮想キー, 256.. = マウスボタン
    for (size_t i = 0; i < n; ++i) {
        const Raw& r = raw[i];
        int key;
        bool isDown;
        if (r.kind == Raw::Down || r.kind == Raw::Up) {
            key = 256 + static_cast<int>(r.button);
            isDown = r.kind == Raw::Down;
        } else if (r.kind == Raw::KeyDown || r.kind == Raw::KeyUp) {
            key = r.vk & 0xFF;
            isDown = r.kind == Raw::KeyDown;
        } else {
            continue;
        }
        if (isDown) {
            if (open[key] >= 0) keep[static_cast<size_t>(open[key])] = 0;
            open[key] = static_cast<long long>(i);
        } else if (open[key] >= 0) {
            open[key] = -1;
        } else {
            keep[i] = 0;
        }
    }
    for (long long idx : open) {
        if (idx >= 0) keep[static_cast<size_t>(idx)] = 0;
    }
    std::vector<Raw> ev;
    ev.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (keep[i]) ev.push_back(raw[i]);
    }

    // 2) 押下 + 解放をクリック / キー入力にまとめる
    struct Timed {
        Step step;
        double t;
    };
    std::vector<Timed> out;
    out.reserve(ev.size());
    const double doubleClickMs = static_cast<double>(GetDoubleClickTime());

    for (size_t i = 0; i < ev.size(); ++i) {
        const Raw& r = ev[i];
        Step s;
        s.x = r.pt.x;
        s.y = r.pt.y;
        s.button = r.button;
        switch (r.kind) {
            case Raw::Down: {
                size_t j = i + 1;
                bool smallMove = true;
                while (j < ev.size()) {
                    const Raw& q = ev[j];
                    if (q.kind == Raw::Up && q.button == r.button) break;
                    if (q.kind != Raw::Move || Distance(q.pt, r.pt) > kClickSlop) {
                        smallMove = false;
                        break;
                    }
                    ++j;
                }
                if (smallMove && j < ev.size() && Distance(ev[j].pt, r.pt) <= kClickSlop) {
                    // 同じ位置での素早い 2 回目のクリックはダブルクリックにまとめる
                    if (!out.empty()) {
                        Timed& prev = out.back();
                        if (prev.step.type == StepType::Click && prev.step.clicks == 1 &&
                            prev.step.button == r.button &&
                            Distance(POINT{prev.step.x, prev.step.y}, r.pt) <= kClickSlop &&
                            r.timeMs - prev.t <= doubleClickMs) {
                            prev.step.clicks = 2;
                            i = j;
                            continue;
                        }
                    }
                    s.type = StepType::Click;
                    s.clicks = 1;
                    out.push_back({s, r.timeMs});
                    i = j;
                    continue;
                }
                s.type = StepType::Down;
                break;
            }
            case Raw::Up: s.type = StepType::Up; break;
            case Raw::Move: s.type = StepType::Move; break;
            case Raw::Wheel:
                s.type = StepType::Wheel;
                s.wheel = r.wheel;
                break;
            case Raw::KeyDown: {
                s.vk = r.vk;
                s.scan = r.scan;
                s.extended = r.extended;
                size_t j = i + 1;
                while (j < ev.size() && ev[j].kind == Raw::Move) ++j;
                if (j < ev.size() && ev[j].kind == Raw::KeyUp && ev[j].vk == r.vk) {
                    s.type = StepType::KeyPress;
                    out.push_back({s, r.timeMs});
                    i = j;
                    continue;
                }
                s.type = StepType::KeyDown;
                break;
            }
            case Raw::KeyUp:
                s.type = StepType::KeyUp;
                s.vk = r.vk;
                s.scan = r.scan;
                s.extended = r.extended;
                break;
        }
        out.push_back({s, r.timeMs});
    }

    // 3) 時刻を「前のステップからの待機時間」に変換
    std::vector<Step> steps;
    steps.reserve(out.size());
    double prev = startMs;
    for (Timed& t : out) {
        const double d = t.t - prev;
        t.step.delayMs = d > 0 ? static_cast<int>(std::lround(d)) : 0;
        prev = t.t;
        steps.push_back(t.step);
    }
    return steps;
}

}  // namespace

bool Start(const Options& options) {
    if (g_mode != Mode::None) return false;
    g_opt = options;
    g_raw.clear();
    g_raw.reserve(1024);
    g_lastMoveMs = -1e9;
    g_hasPendingMove = false;
    std::fill(std::begin(g_ignoreUp), std::end(g_ignoreUp), false);
    std::fill(std::begin(g_keyIsDown), std::end(g_keyIsDown), false);
    if (!Hook(options.recordKeys)) return false;
    g_startMs = NowMs();
    g_mode = Mode::Record;
    return true;
}

std::vector<Step> Stop() {
    if (g_mode != Mode::Record) return {};
    Unhook();
    g_mode = Mode::None;
    FlushPendingMove();
    std::vector<Step> steps = Process(g_raw, g_startMs);
    g_raw.clear();
    g_raw.shrink_to_fit();
    return steps;
}

bool IsRecording() { return g_mode == Mode::Record; }

size_t EventCount() { return g_raw.size(); }

bool BeginPick(HWND notify) {
    if (g_mode != Mode::None) return false;
    g_pickNotify = notify;
    g_pickFinished = false;
    g_pickSwallowLeftUp = false;
    g_pickSwallowRightUp = false;
    if (!Hook(true)) return false;
    g_mode = Mode::Pick;
    return true;
}

void EndPick() {
    if (g_mode != Mode::Pick) return;
    Unhook();
    g_mode = Mode::None;
}

bool IsPicking() { return g_mode == Mode::Pick; }

}  // namespace recorder
