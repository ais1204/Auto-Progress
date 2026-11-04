#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "input.h"

// マクロの 1 ステップの種類
enum class StepType : int {
    Click,     // 指定位置でクリック（clicks 回）
    Down,      // ボタン押下（ドラッグ開始など）
    Up,        // ボタン解放
    Move,      // カーソル移動
    Wheel,     // ホイール
    KeyPress,  // キーを押して離す
    KeyDown,   // キー押下
    KeyUp,     // キー解放
    Wait,      // 待機のみ（何もしない）
};

struct Step {
    StepType type = StepType::Click;
    int delayMs = 0;  // このステップを実行する前に待つ時間
    MouseButton button = MouseButton::Left;
    int x = 0;
    int y = 0;
    int clicks = 1;  // Click: 1 = シングル, 2 = ダブル
    int wheel = 0;   // Wheel: 正 = 上 / 奥
    WORD vk = 0;     // キー系
    WORD scan = 0;
    bool extended = false;

    bool HasPosition() const;
    bool IsKey() const;
};

// 編集欄の「動作」コンボボックスに並べるマウス系の動作
struct MouseAction {
    StepType type;
    MouseButton button;
    int clicks;
    int wheel;
    const wchar_t* name;
};

namespace macro {

const std::vector<MouseAction>& MouseActions();
// ステップに対応する MouseActions() のインデックス（キー系などは -1）
int FindMouseAction(const Step& s);
// MouseActions()[index] の内容を s に反映（位置・待機時間は変えない）
void ApplyMouseAction(Step& s, int index);

std::wstring ActionText(const Step& s);  // 一覧の「動作」列
std::wstring DetailText(const Step& s);  // 一覧の「位置 / キー」列

bool SaveFile(const std::wstring& path, const std::vector<Step>& steps, std::wstring* error);
bool LoadFile(const std::wstring& path, std::vector<Step>& steps, std::wstring* error);

}  // namespace macro
