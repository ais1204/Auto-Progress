#pragma once

#include <windows.h>

#include <atomic>
#include <functional>

// 終了理由（WM_APP_WORKER_DONE の lParam）
enum class StopReason : int {
    Completed = 0,  // 回数・時間などの条件を満たして終了
    User = 1,       // ユーザーが停止した
    Failsafe = 2,   // 画面の左上隅にカーソルを移動して緊急停止
};

// ワーカースレッドが終了すると notify ウィンドウへ送られる
// wParam = MAKEWPARAM(Worker の id, 起動世代), lParam = StopReason
constexpr UINT WM_APP_WORKER_DONE = WM_APP + 1;

// 停止イベント付きのワーカースレッド
class Worker {
public:
    explicit Worker(int id) : id_(id) {}
    virtual ~Worker() { Stop(); }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool Running() const { return running_.load(); }
    // 開始待機中なら true（開始前の待機時間をカウントしている間）
    bool Waiting() const { return waiting_.load(); }

    // 停止を要求し、スレッドの終了を待つ
    void Stop();

    // Launch のたびに増える番号。古い終了通知を無視するために使う
    WORD Generation() const { return generation_; }

protected:
    // body をワーカースレッドで実行する。body は StopReason を返す。
    bool Launch(HWND notify, std::function<StopReason(HANDLE stopEvent)> body);
    void SetWaiting(bool w) { waiting_.store(w); }

private:
    static DWORD WINAPI ThreadMain(LPVOID param);

    int id_;
    WORD generation_ = 0;
    HWND notify_ = nullptr;
    HANDLE thread_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::function<StopReason(HANDLE)> body_;
    std::atomic<bool> running_{false};
    std::atomic<bool> waiting_{false};
};
