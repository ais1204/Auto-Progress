#include "input.h"

#include <cstdlib>
#include <vector>

namespace input {
namespace {

// 仮想スクリーン座標を SendInput 用の正規化座標 (0..65535) に変換する
void ToAbsolute(int x, int y, LONG& ax, LONG& ay) {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    long long rx = static_cast<long long>(x) - left;
    long long ry = static_cast<long long>(y) - top;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx >= w) rx = w - 1;
    if (ry >= h) ry = h - 1;

    // Windows は「正規化座標 * 幅 / 65536」の切り捨てでピクセル位置を求めるため、
    // +1 しておくと丸め誤差で 1 ピクセル手前にずれるのを防げる
    long long nx = rx * 65536 / w + 1;
    long long ny = ry * 65536 / h + 1;
    if (nx > 65535) nx = 65535;
    if (ny > 65535) ny = 65535;
    ax = static_cast<LONG>(nx);
    ay = static_cast<LONG>(ny);
}

INPUT MouseInput(DWORD flags, const POINT* at, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    if (at) {
        ToAbsolute(at->x, at->y, in.mi.dx, in.mi.dy);
        in.mi.dwFlags |= MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    }
    return in;
}

DWORD DownFlag(MouseButton b) {
    switch (b) {
        case MouseButton::Right: return MOUSEEVENTF_RIGHTDOWN;
        case MouseButton::Middle: return MOUSEEVENTF_MIDDLEDOWN;
        default: return MOUSEEVENTF_LEFTDOWN;
    }
}

DWORD UpFlag(MouseButton b) {
    switch (b) {
        case MouseButton::Right: return MOUSEEVENTF_RIGHTUP;
        case MouseButton::Middle: return MOUSEEVENTF_MIDDLEUP;
        default: return MOUSEEVENTF_LEFTUP;
    }
}

void Send(std::vector<INPUT>& inputs) {
    if (!inputs.empty()) {
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
}

}  // namespace

void MoveTo(int x, int y) {
    const POINT p{x, y};
    std::vector<INPUT> v{MouseInput(0, &p)};
    Send(v);
}

void Click(MouseButton b, int count, const POINT* at, const POINT* restore) {
    if (count < 1) count = 1;
    std::vector<INPUT> v;
    v.reserve(static_cast<size_t>(count) * 2 + 1);
    for (int i = 0; i < count; ++i) {
        // 位置付きのイベントにしておくと、ユーザーがマウスを動かしても狙った位置に入力される
        v.push_back(MouseInput(DownFlag(b), at));
        v.push_back(MouseInput(UpFlag(b), at));
    }
    if (restore) v.push_back(MouseInput(0, restore));
    Send(v);
}

void Button(MouseButton b, bool down, const POINT* at) {
    std::vector<INPUT> v{MouseInput(down ? DownFlag(b) : UpFlag(b), at)};
    Send(v);
}

void Wheel(int delta, const POINT* at) {
    std::vector<INPUT> v;
    if (at) v.push_back(MouseInput(0, at));
    v.push_back(MouseInput(MOUSEEVENTF_WHEEL, nullptr, static_cast<DWORD>(delta)));
    Send(v);
}

void Key(WORD vk, WORD scan, bool extended, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = scan ? scan : static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(INPUT));
}

bool CursorInCorner(const POINT* lastSet) {
    POINT p;
    if (!GetCursorPos(&p)) return false;
    HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    const bool corner = p.x <= mi.rcMonitor.left + 1 && p.y <= mi.rcMonitor.top + 1;
    if (!corner) return false;
    // 自分でその位置へ動かした直後なら緊急停止扱いにしない
    if (lastSet && std::llabs(static_cast<long long>(lastSet->x) - p.x) <= 2 &&
        std::llabs(static_cast<long long>(lastSet->y) - p.y) <= 2) {
        return false;
    }
    return true;
}

bool IsExtendedKey(UINT vk) {
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT:
        case VK_UP: case VK_DOWN: case VK_NUMLOCK: case VK_DIVIDE:
        case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN:
        case VK_APPS: case VK_SNAPSHOT: case VK_CANCEL:
            return true;
        default:
            return false;
    }
}

std::wstring KeyName(UINT vk, UINT scan, bool extended) {
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Num " + std::to_wstring(vk - VK_NUMPAD0);

    switch (vk) {
        case VK_LBUTTON: return L"左ボタン";
        case VK_RBUTTON: return L"右ボタン";
        case VK_MBUTTON: return L"中ボタン";
        case VK_BACK: return L"BackSpace";
        case VK_TAB: return L"Tab";
        case VK_RETURN: return L"Enter";
        case VK_SHIFT: return L"Shift";
        case VK_CONTROL: return L"Ctrl";
        case VK_MENU: return L"Alt";
        case VK_LSHIFT: return L"左Shift";
        case VK_RSHIFT: return L"右Shift";
        case VK_LCONTROL: return L"左Ctrl";
        case VK_RCONTROL: return L"右Ctrl";
        case VK_LMENU: return L"左Alt";
        case VK_RMENU: return L"右Alt";
        case VK_LWIN: return L"左Win";
        case VK_RWIN: return L"右Win";
        case VK_APPS: return L"アプリケーション";
        case VK_PAUSE: return L"Pause";
        case VK_CAPITAL: return L"CapsLock";
        case VK_ESCAPE: return L"Esc";
        case VK_SPACE: return L"Space";
        case VK_PRIOR: return L"PageUp";
        case VK_NEXT: return L"PageDown";
        case VK_END: return L"End";
        case VK_HOME: return L"Home";
        case VK_LEFT: return L"←";
        case VK_UP: return L"↑";
        case VK_RIGHT: return L"→";
        case VK_DOWN: return L"↓";
        case VK_SNAPSHOT: return L"PrintScreen";
        case VK_INSERT: return L"Insert";
        case VK_DELETE: return L"Delete";
        case VK_MULTIPLY: return L"Num *";
        case VK_ADD: return L"Num +";
        case VK_SUBTRACT: return L"Num -";
        case VK_DECIMAL: return L"Num .";
        case VK_DIVIDE: return L"Num /";
        case VK_NUMLOCK: return L"NumLock";
        case VK_SCROLL: return L"ScrollLock";
        default: break;
    }

    // それ以外はキーボードレイアウトに名前を問い合わせる
    if (scan == 0) scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (scan != 0) {
        LONG lparam = static_cast<LONG>(scan << 16);
        if (extended || IsExtendedKey(vk)) lparam |= (1 << 24);
        wchar_t buf[64] = {};
        if (GetKeyNameTextW(lparam, buf, 64) > 0) return buf;
    }
    wchar_t buf[32];
    wsprintfW(buf, L"VK 0x%02X", vk);
    return buf;
}

const wchar_t* ButtonName(MouseButton b) {
    switch (b) {
        case MouseButton::Right: return L"右";
        case MouseButton::Middle: return L"中";
        default: return L"左";
    }
}

}  // namespace input
