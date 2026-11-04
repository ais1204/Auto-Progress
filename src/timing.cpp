#include "timing.h"

#include <mmsystem.h>

namespace timing {

int64_t Frequency() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<int64_t>(f.QuadPart);
    }();
    return freq;
}

int64_t Now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(c.QuadPart);
}

int64_t FromMs(double ms) {
    return static_cast<int64_t>(ms * static_cast<double>(Frequency()) / 1000.0);
}

double ToMs(int64_t ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(Frequency());
}

bool WaitUntil(HANDLE stopEvent, int64_t target) {
    for (;;) {
        const int64_t now = Now();
        if (now >= target) {
            // 期限到達。ただし停止要求が出ていれば優先する
            return WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0;
        }
        const double remain = ToMs(target - now);
        // 残りが多いときは OS の待機に任せ、最後の 1ms 未満だけ短く刻んで確認する
        DWORD wait = 0;
        if (remain > 2.0) wait = static_cast<DWORD>(remain - 1.0);
        else if (remain > 1.0) wait = 1;
        if (WaitForSingleObject(stopEvent, wait) == WAIT_OBJECT_0) return false;
        if (wait == 0) SwitchToThread();
    }
}

Rng::Rng() {
    state_ = static_cast<uint64_t>(Now()) ^ (static_cast<uint64_t>(GetCurrentThreadId()) << 32) ^
             0x9E3779B97F4A7C15ull;
    if (state_ == 0) state_ = 1;
}

int Rng::Range(int lo, int hi) {
    if (hi <= lo) return lo;
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    const uint64_t span = static_cast<uint64_t>(static_cast<int64_t>(hi) - lo + 1);
    return static_cast<int>(static_cast<int64_t>(lo) + static_cast<int64_t>(state_ % span));
}

HighResolutionScope::HighResolutionScope() { timeBeginPeriod(1); }
HighResolutionScope::~HighResolutionScope() { timeEndPeriod(1); }

}  // namespace timing
