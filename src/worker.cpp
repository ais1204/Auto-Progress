#include "worker.h"

bool Worker::Launch(HWND notify, std::function<StopReason(HANDLE)> body) {
    Stop();
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return false;
    notify_ = notify;
    body_ = std::move(body);
    ++generation_;
    running_.store(true);
    waiting_.store(false);
    thread_ = CreateThread(nullptr, 0, &Worker::ThreadMain, this, 0, nullptr);
    if (!thread_) {
        running_.store(false);
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        return false;
    }
    return true;
}

void Worker::Stop() {
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_) {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    running_.store(false);
    waiting_.store(false);
}

DWORD WINAPI Worker::ThreadMain(LPVOID param) {
    auto* self = static_cast<Worker*>(param);
    const WORD generation = self->generation_;
    StopReason reason = StopReason::User;
    try {
        reason = self->body_(self->stopEvent_);
    } catch (...) {
        reason = StopReason::User;
    }
    self->waiting_.store(false);
    self->running_.store(false);
    if (self->notify_) {
        PostMessageW(self->notify_, WM_APP_WORKER_DONE, MAKEWPARAM(self->id_, generation),
                     static_cast<LPARAM>(reason));
    }
    return 0;
}
