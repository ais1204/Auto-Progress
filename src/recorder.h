#pragma once

#include <windows.h>

#include <vector>

#include "macro.h"

// 位置選択の結果（wParam = x, lParam = y）
constexpr UINT WM_APP_PICK_DONE = WM_APP + 2;
// 位置選択のキャンセル
constexpr UINT WM_APP_PICK_CANCEL = WM_APP + 3;

// 低レベルフック（WH_MOUSE_LL / WH_KEYBOARD_LL）を使った操作の記録と位置選択。
// すべて UI スレッドから呼び出すこと（フックはそのスレッドのメッセージループで動く）。
namespace recorder {

struct Options {
    bool recordMoves = false;     // マウス移動も記録する
    bool recordKeys = false;      // キー入力も記録する
    std::vector<UINT> ignoreVks;  // 記録しないキー（ホットキーなど）
};

bool Start(const Options& options);
// 記録を終了し、整形したステップを返す
std::vector<Step> Stop();
bool IsRecording();
size_t EventCount();

// 次の左クリックの位置を取得する（クリックは対象アプリに渡さない）。
// 結果は notify に WM_APP_PICK_DONE / WM_APP_PICK_CANCEL で届く。
bool BeginPick(HWND notify);
void EndPick();
bool IsPicking();

}  // namespace recorder
