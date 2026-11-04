#pragma once

#include <windows.h>

#include <cstdint>

// 高精度タイマーと待機のユーティリティ
namespace timing {

int64_t Now();                  // QueryPerformanceCounter の値
int64_t Frequency();            // 1 秒あたりのカウント数
int64_t FromMs(double ms);      // ミリ秒 -> カウント
double ToMs(int64_t ticks);     // カウント -> ミリ秒

// target（Now() と同じ単位）になるまで待つ。
// 途中で stopEvent がシグナル状態になったら false を返す。
bool WaitUntil(HANDLE stopEvent, int64_t target);

// スレッドごとに持たせる簡易乱数（xorshift）
class Rng {
public:
    Rng();
    // [lo, hi] の一様乱数
    int Range(int lo, int hi);

private:
    uint64_t state_;
};

// スコープ内だけ Windows のタイマー分解能を 1ms にする
class HighResolutionScope {
public:
    HighResolutionScope();
    ~HighResolutionScope();
    HighResolutionScope(const HighResolutionScope&) = delete;
    HighResolutionScope& operator=(const HighResolutionScope&) = delete;
};

}  // namespace timing
