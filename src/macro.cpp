#include "macro.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

bool Step::HasPosition() const {
    switch (type) {
        case StepType::Click:
        case StepType::Down:
        case StepType::Up:
        case StepType::Move:
        case StepType::Wheel:
            return true;
        default:
            return false;
    }
}

bool Step::IsKey() const {
    return type == StepType::KeyPress || type == StepType::KeyDown || type == StepType::KeyUp;
}

namespace macro {

const std::vector<MouseAction>& MouseActions() {
    static const std::vector<MouseAction> actions = {
        {StepType::Click, MouseButton::Left, 1, 0, L"左クリック"},
        {StepType::Click, MouseButton::Right, 1, 0, L"右クリック"},
        {StepType::Click, MouseButton::Middle, 1, 0, L"中クリック"},
        {StepType::Click, MouseButton::Left, 2, 0, L"左ダブルクリック"},
        {StepType::Click, MouseButton::Right, 2, 0, L"右ダブルクリック"},
        {StepType::Down, MouseButton::Left, 1, 0, L"左ボタン押下"},
        {StepType::Up, MouseButton::Left, 1, 0, L"左ボタン解放"},
        {StepType::Down, MouseButton::Right, 1, 0, L"右ボタン押下"},
        {StepType::Up, MouseButton::Right, 1, 0, L"右ボタン解放"},
        {StepType::Down, MouseButton::Middle, 1, 0, L"中ボタン押下"},
        {StepType::Up, MouseButton::Middle, 1, 0, L"中ボタン解放"},
        {StepType::Move, MouseButton::Left, 1, 0, L"マウス移動"},
        {StepType::Wheel, MouseButton::Left, 1, WHEEL_DELTA, L"ホイール上"},
        {StepType::Wheel, MouseButton::Left, 1, -WHEEL_DELTA, L"ホイール下"},
        {StepType::Wait, MouseButton::Left, 1, 0, L"待機のみ"},
    };
    return actions;
}

int FindMouseAction(const Step& s) {
    const auto& actions = MouseActions();
    for (size_t i = 0; i < actions.size(); ++i) {
        const MouseAction& a = actions[i];
        if (a.type != s.type) continue;
        switch (s.type) {
            case StepType::Click:
                if (a.button == s.button && a.clicks == (s.clicks >= 2 ? 2 : 1)) return static_cast<int>(i);
                break;
            case StepType::Down:
            case StepType::Up:
                if (a.button == s.button) return static_cast<int>(i);
                break;
            case StepType::Wheel:
                if ((a.wheel > 0) == (s.wheel > 0)) return static_cast<int>(i);
                break;
            case StepType::Move:
            case StepType::Wait:
                return static_cast<int>(i);
            default:
                break;
        }
    }
    return -1;
}

void ApplyMouseAction(Step& s, int index) {
    const auto& actions = MouseActions();
    if (index < 0 || index >= static_cast<int>(actions.size())) return;
    const MouseAction& a = actions[static_cast<size_t>(index)];
    const bool wasWheel = s.type == StepType::Wheel;
    s.type = a.type;
    s.button = a.button;
    s.clicks = a.clicks;
    if (a.type == StepType::Wheel) {
        // 既存のホイール量は向きだけ合わせて維持する
        const int magnitude = wasWheel && s.wheel != 0 ? std::abs(s.wheel) : WHEEL_DELTA;
        s.wheel = a.wheel > 0 ? magnitude : -magnitude;
    } else {
        s.wheel = 0;
    }
}

std::wstring ActionText(const Step& s) {
    switch (s.type) {
        case StepType::Click:
            if (s.clicks >= 2) return std::wstring(input::ButtonName(s.button)) + L"ダブルクリック";
            return std::wstring(input::ButtonName(s.button)) + L"クリック";
        case StepType::Down: return std::wstring(input::ButtonName(s.button)) + L"ボタン押下";
        case StepType::Up: return std::wstring(input::ButtonName(s.button)) + L"ボタン解放";
        case StepType::Move: return L"マウス移動";
        case StepType::Wheel: return s.wheel >= 0 ? L"ホイール上" : L"ホイール下";
        case StepType::KeyPress: return L"キー入力";
        case StepType::KeyDown: return L"キー押下";
        case StepType::KeyUp: return L"キー解放";
        case StepType::Wait: return L"待機のみ";
    }
    return L"?";
}

std::wstring DetailText(const Step& s) {
    wchar_t buf[96];
    if (s.IsKey()) return input::KeyName(s.vk, s.scan, s.extended);
    if (s.type == StepType::Wait) return L"-";
    if (s.type == StepType::Wheel) {
        swprintf(buf, 96, L"(%d, %d)  量 %d", s.x, s.y, s.wheel);
        return buf;
    }
    swprintf(buf, 96, L"(%d, %d)", s.x, s.y);
    return buf;
}

// ---------------------------------------------------------------------------
// ファイル入出力
//
// テキスト形式（1 行 1 ステップ、# から始まる行はコメント）
//   CLICK   <待機ms> <L|R|M> <x> <y> <回数>
//   DOWN    <待機ms> <L|R|M> <x> <y>
//   UP      <待機ms> <L|R|M> <x> <y>
//   MOVE    <待機ms> <x> <y>
//   WHEEL   <待機ms> <x> <y> <量>
//   KEY     <待機ms> <仮想キー> <スキャンコード> <拡張キー 0|1>
//   KEYDOWN <待機ms> <仮想キー> <スキャンコード> <拡張キー 0|1>
//   KEYUP   <待機ms> <仮想キー> <スキャンコード> <拡張キー 0|1>
//   WAIT    <待機ms>
// ---------------------------------------------------------------------------
namespace {

char ButtonChar(MouseButton b) {
    switch (b) {
        case MouseButton::Right: return 'R';
        case MouseButton::Middle: return 'M';
        default: return 'L';
    }
}

bool ParseButton(const std::string& s, MouseButton& b) {
    if (s == "L" || s == "l") { b = MouseButton::Left; return true; }
    if (s == "R" || s == "r") { b = MouseButton::Right; return true; }
    if (s == "M" || s == "m") { b = MouseButton::Middle; return true; }
    return false;
}

bool ParseInt(const std::string& s, long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtol(s.c_str(), &end, 0);
    return end && *end == '\0';
}

std::string FormatStep(const Step& s) {
    char buf[160];
    const char b = ButtonChar(s.button);
    switch (s.type) {
        case StepType::Click:
            snprintf(buf, sizeof(buf), "CLICK %d %c %d %d %d", s.delayMs, b, s.x, s.y, s.clicks);
            break;
        case StepType::Down:
            snprintf(buf, sizeof(buf), "DOWN %d %c %d %d", s.delayMs, b, s.x, s.y);
            break;
        case StepType::Up:
            snprintf(buf, sizeof(buf), "UP %d %c %d %d", s.delayMs, b, s.x, s.y);
            break;
        case StepType::Move:
            snprintf(buf, sizeof(buf), "MOVE %d %d %d", s.delayMs, s.x, s.y);
            break;
        case StepType::Wheel:
            snprintf(buf, sizeof(buf), "WHEEL %d %d %d %d", s.delayMs, s.x, s.y, s.wheel);
            break;
        case StepType::KeyPress:
        case StepType::KeyDown:
        case StepType::KeyUp: {
            const char* name = s.type == StepType::KeyPress ? "KEY" : (s.type == StepType::KeyDown ? "KEYDOWN" : "KEYUP");
            snprintf(buf, sizeof(buf), "%s %d 0x%02X 0x%02X %d", name, s.delayMs, s.vk, s.scan, s.extended ? 1 : 0);
            break;
        }
        case StepType::Wait:
            snprintf(buf, sizeof(buf), "WAIT %d", s.delayMs);
            break;
    }
    return buf;
}

bool ParseLine(const std::string& line, Step& s, std::string& why) {
    std::istringstream is(line);
    std::vector<std::string> t;
    std::string tok;
    while (is >> tok) t.push_back(tok);
    if (t.empty()) return false;

    const std::string& kind = t[0];
    std::vector<long> n;  // 数値トークン（ボタン指定を除く）
    MouseButton button = MouseButton::Left;
    size_t expect = 0;
    bool hasButton = false;

    if (kind == "CLICK") { s.type = StepType::Click; expect = 4; hasButton = true; }
    else if (kind == "DOWN") { s.type = StepType::Down; expect = 3; hasButton = true; }
    else if (kind == "UP") { s.type = StepType::Up; expect = 3; hasButton = true; }
    else if (kind == "MOVE") { s.type = StepType::Move; expect = 3; }
    else if (kind == "WHEEL") { s.type = StepType::Wheel; expect = 4; }
    else if (kind == "KEY") { s.type = StepType::KeyPress; expect = 4; }
    else if (kind == "KEYDOWN") { s.type = StepType::KeyDown; expect = 4; }
    else if (kind == "KEYUP") { s.type = StepType::KeyUp; expect = 4; }
    else if (kind == "WAIT") { s.type = StepType::Wait; expect = 1; }
    else { why = "不明な種類 \"" + kind + "\""; return false; }

    const size_t needed = 1 + expect + (hasButton ? 1 : 0);
    if (t.size() < needed) { why = "値が足りません"; return false; }

    for (size_t i = 1; i < t.size() && n.size() < expect; ++i) {
        if (hasButton && i == 2) {
            if (!ParseButton(t[i], button)) { why = "ボタン指定は L / R / M です"; return false; }
            continue;
        }
        long v = 0;
        if (!ParseInt(t[i], v)) { why = "数値が不正です: " + t[i]; return false; }
        n.push_back(v);
    }
    if (n.size() < expect) { why = "値が足りません"; return false; }

    s.delayMs = n[0] < 0 ? 0 : static_cast<int>(n[0]);
    s.button = button;
    switch (s.type) {
        case StepType::Click:
            s.x = static_cast<int>(n[1]); s.y = static_cast<int>(n[2]);
            s.clicks = n[3] >= 2 ? 2 : 1;
            break;
        case StepType::Down:
        case StepType::Up:
        case StepType::Move:
            s.x = static_cast<int>(n[1]); s.y = static_cast<int>(n[2]);
            break;
        case StepType::Wheel:
            s.x = static_cast<int>(n[1]); s.y = static_cast<int>(n[2]);
            s.wheel = static_cast<int>(n[3]);
            break;
        case StepType::KeyPress:
        case StepType::KeyDown:
        case StepType::KeyUp:
            if (n[1] <= 0 || n[1] > 0xFE) { why = "仮想キーコードが不正です"; return false; }
            s.vk = static_cast<WORD>(n[1]);
            s.scan = static_cast<WORD>(n[2] & 0xFFFF);
            s.extended = n[3] != 0;
            break;
        case StepType::Wait:
            break;
    }
    return true;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], len);
    return w;
}

}  // namespace

bool SaveFile(const std::wstring& path, const std::vector<Step>& steps, std::wstring* error) {
    std::string text = "# AutoClicker Macro v1\r\n# TYPE DELAY_MS ARGS...\r\n";
    for (const Step& s : steps) {
        text += FormatStep(s);
        text += "\r\n";
    }

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (error) *error = L"ファイルを作成できませんでした。";
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(f);
    if (!ok || written != text.size()) {
        if (error) *error = L"ファイルへの書き込みに失敗しました。";
        return false;
    }
    return true;
}

bool LoadFile(const std::wstring& path, std::vector<Step>& steps, std::wstring* error) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (error) *error = L"ファイルを開けませんでした。";
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart > 64LL * 1024 * 1024) {
        CloseHandle(f);
        if (error) *error = L"ファイルが大きすぎます。";
        return false;
    }
    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = text.empty() || ReadFile(f, &text[0], static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(f);
    if (!ok) {
        if (error) *error = L"ファイルの読み込みに失敗しました。";
        return false;
    }
    text.resize(read);
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);  // UTF-8 BOM
    }

    std::vector<Step> result;
    std::istringstream is(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(is, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        Step s;
        std::string why;
        if (!ParseLine(line.substr(first), s, why)) {
            if (error) *error = std::to_wstring(lineNo) + L" 行目: " + Widen(why);
            return false;
        }
        result.push_back(s);
    }
    steps.swap(result);
    return true;
}

}  // namespace macro
