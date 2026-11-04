#pragma once

#include <windows.h>

#include <string>

enum class MouseButton : int { Left = 0, Right = 1, Middle = 2 };

// SendInput を使った入力の送信
namespace input {

// 指定位置（仮想スクリーン座標・物理ピクセル）へカーソルを移動
void MoveTo(int x, int y);

// クリック（count 回連続）。at が指定されていればその位置でクリック、
// restore が指定されていればクリック後にその位置へカーソルを戻す。
void Click(MouseButton b, int count, const POINT* at, const POINT* restore = nullptr);

// ボタンの押下 / 解放のみ
void Button(MouseButton b, bool down, const POINT* at);

// ホイール（正 = 奥 / 上方向）
void Wheel(int delta, const POINT* at);

// キーの押下 / 解放
void Key(WORD vk, WORD scan, bool extended, bool down);

// カーソルがいずれかのモニターの左上隅にあるか（緊急停止用）。
// lastSet が指定され、カーソルがそこ（自分で動かした位置）にあるなら false。
bool CursorInCorner(const POINT* lastSet);

// 仮想キーが拡張キー（矢印キー等）かどうか
bool IsExtendedKey(UINT vk);

// 仮想キーの表示名（例: "F6", "A", "左Shift"）
std::wstring KeyName(UINT vk, UINT scan = 0, bool extended = false);

const wchar_t* ButtonName(MouseButton b);  // "左" / "右" / "中"

}  // namespace input
