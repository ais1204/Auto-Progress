#include "app.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "clicker.h"
#include "input.h"
#include "macro.h"
#include "recorder.h"
#include "resource.h"
#include "settings.h"

namespace {

// ---------------------------------------------------------------------------
// 定数
// ---------------------------------------------------------------------------
constexpr wchar_t kClassName[] = L"AutoProgress.AutoClicker";
constexpr wchar_t kPageClassName[] = L"AutoProgress.AutoClicker.Page";
constexpr wchar_t kShowMessageName[] = L"AutoProgress.AutoClicker.Show";
constexpr wchar_t kMutexName[] = L"Local\\AutoProgress.AutoClicker";
constexpr wchar_t kAppTitle[] = L"オートクリッカー";
constexpr wchar_t kVersion[] = L"1.0.0";

constexpr UINT WM_APP_TRAY = WM_APP + 10;
constexpr UINT WM_APP_STARTUP = WM_APP + 11;
constexpr UINT_PTR kStatusTimer = 1;

constexpr int kMaxMs = 24 * 60 * 60 * 1000;  // 入力できる時間の上限（24 時間）
constexpr int kMaxCount = 999999999;
constexpr size_t kUndoLimit = 30;

// レイアウトの基準サイズ（96 DPI 時のピクセル）
constexpr int kClientWidth = 480;
constexpr int kClientHeight = 556;  // ステータスバーを除く
constexpr int kTabX = 8, kTabY = 8, kTabW = 464, kTabH = 514;

enum Page : int { kPageClicker = 0, kPageMacro = 1, kPageSettings = 2, kPageCount = 3, kPageMain = -1 };

enum ControlId : int {
    IDC_TAB = 100,
    IDC_STATUS,
    IDC_TOPMOST,

    IDC_C_INTERVAL = 200,
    IDC_C_UNIT,
    IDC_C_JITTER,
    IDC_C_BUTTON,
    IDC_C_TYPE,
    IDC_C_REP_INF,
    IDC_C_REP_COUNT,
    IDC_C_COUNT,
    IDC_C_REP_TIME,
    IDC_C_DURATION,
    IDC_C_POS_CUR,
    IDC_C_POS_FIX,
    IDC_C_X,
    IDC_C_Y,
    IDC_C_PICK,
    IDC_C_RESTORE,
    IDC_C_START,
    IDC_C_STOP,
    IDC_C_HINT,

    IDC_M_RECORD = 300,
    IDC_M_REC_MOVES,
    IDC_M_REC_KEYS,
    IDC_M_REC_APPEND,
    IDC_M_LIST,
    IDC_M_EDIT_GROUP,
    IDC_M_ACTION,
    IDC_M_DELAY,
    IDC_M_X,
    IDC_M_Y,
    IDC_M_PICK,
    IDC_M_APPLY,
    IDC_M_ADD,
    IDC_M_UP,
    IDC_M_DOWN,
    IDC_M_DELETE,
    IDC_M_CLEAR,
    IDC_M_SETALL,
    IDC_M_OPEN,
    IDC_M_SAVE,
    IDC_M_LOOPS,
    IDC_M_SPEED,
    IDC_M_LOOPDELAY,
    IDC_M_PLAY,
    IDC_M_STOP,

    IDC_S_HOTKEY0 = 400,  // 400..403
    IDC_S_HOTKEY_APPLY = 410,
    IDC_S_START_DELAY,
    IDC_S_CORNER,
    IDC_S_SOUND,
    IDC_S_TRAY,
    IDC_S_AUTOSAVE,
    IDC_S_INFO,
    IDC_S_OPEN_FOLDER,

    IDM_TRAY_SHOW = 900,
    IDM_TRAY_CLICKER,
    IDM_TRAY_PLAY,
    IDM_TRAY_EXIT,
};

// ---------------------------------------------------------------------------
// アプリケーションの状態
// ---------------------------------------------------------------------------
struct Control {
    HWND hwnd;
    int page;  // kPageMain ならメインウィンドウ直下
    int x, y, w, h;
    bool big;  // 大きいフォントを使う
};

struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND tab = nullptr;
    HWND status = nullptr;
    HWND list = nullptr;
    HWND pages[kPageCount] = {};
    HFONT font = nullptr;
    HFONT bigFont = nullptr;
    HICON iconBig = nullptr;
    HICON iconSmall = nullptr;
    UINT dpi = 96;

    std::vector<Control> controls;
    std::unordered_map<int, HWND> byId;
    int page = kPageClicker;

    Config cfg;
    Clicker clicker;
    Player player;
    WORD handledGeneration[3] = {};

    std::vector<Step> steps;
    std::vector<std::vector<Step>> undo;
    bool dirty = false;
    std::wstring macroPath;
    int highlightedStep = -1;

    int pickTarget = 0;  // 1 = 連打の位置, 2 = ステップ編集の位置
    std::wstring message;
    std::wstring lastStatus;
    std::wstring lastCursorText;
    bool trayIcon = false;
    UINT taskbarCreatedMsg = 0;
    UINT showMsg = 0;
    HWND lastFocus = nullptr;
};

App g;

// ---------------------------------------------------------------------------
// DPI 関連（古い Windows でも起動できるよう動的に読み込む）
// ---------------------------------------------------------------------------
template <class T>
T User32Proc(const char* name) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return nullptr;
    return reinterpret_cast<T>(reinterpret_cast<void (*)(void)>(GetProcAddress(user32, name)));
}

UINT SystemDpi() {
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

UINT WindowDpi(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static const Fn fn = User32Proc<Fn>("GetDpiForWindow");
    if (fn) {
        const UINT dpi = fn(hwnd);
        if (dpi > 0) return dpi;
    }
    return SystemDpi();
}

void AdjustRectForDpi(RECT* rc, DWORD style, DWORD exStyle, UINT dpi) {
    using Fn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const Fn fn = User32Proc<Fn>("AdjustWindowRectExForDpi");
    if (fn && fn(rc, style, FALSE, exStyle, dpi)) return;
    AdjustWindowRectEx(rc, style, FALSE, exStyle);
}

int S(int v) { return MulDiv(v, static_cast<int>(g.dpi), 96); }

void CreateFonts() {
    if (g.font) DeleteObject(g.font);
    if (g.bigFont) DeleteObject(g.bigFont);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    using Fn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static const Fn spiForDpi = User32Proc<Fn>("SystemParametersInfoForDpi");
    bool ok = spiForDpi && spiForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi);
    if (!ok) {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, static_cast<int>(g.dpi),
                                            static_cast<int>(SystemDpi()));
    }
    LOGFONTW lf = ncm.lfMessageFont;
    g.font = CreateFontIndirectW(&lf);
    lf.lfHeight = lf.lfHeight * 5 / 4;
    lf.lfWeight = FW_BOLD;
    g.bigFont = CreateFontIndirectW(&lf);
}

// ---------------------------------------------------------------------------
// コントロール操作のヘルパー
// ---------------------------------------------------------------------------
HWND Ctl(int id) {
    auto it = g.byId.find(id);
    return it == g.byId.end() ? nullptr : it->second;
}

HWND AddControl(int page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
                int id = -1, DWORD exStyle = 0, bool big = false) {
    HWND parent = page == kPageMain ? g.hwnd : g.pages[page];
    HWND hwnd = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.inst, nullptr);
    g.controls.push_back({hwnd, page, x, y, w, h, big});
    if (id >= 0) g.byId[id] = hwnd;
    return hwnd;
}

HWND Label(int page, const wchar_t* text, int x, int y, int w, int h = 24, int id = -1) {
    return AddControl(page, L"STATIC", text, SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, x, y, w, h, id);
}

HWND MultiLabel(int page, const wchar_t* text, int x, int y, int w, int h, int id = -1) {
    return AddControl(page, L"STATIC", text, SS_LEFT | SS_NOPREFIX, x, y, w, h, id);
}

HWND Group(int page, const wchar_t* text, int x, int y, int w, int h, int id = -1) {
    return AddControl(page, L"BUTTON", text, BS_GROUPBOX | WS_GROUP, x, y, w, h, id);
}

HWND Edit(int page, int id, int x, int y, int w, bool number, bool group = true) {
    HWND e = AddControl(page, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | (number ? ES_NUMBER : 0) | (group ? WS_GROUP : 0),
                        x, y, w, 24, id, WS_EX_CLIENTEDGE);
    SendMessageW(e, EM_SETLIMITTEXT, 10, 0);
    return e;
}

HWND Combo(int page, int id, int x, int y, int w, std::initializer_list<const wchar_t*> items) {
    HWND c = AddControl(page, L"COMBOBOX", L"", WS_TABSTOP | WS_GROUP | WS_VSCROLL | CBS_DROPDOWNLIST, x, y, w, 300, id);
    for (const wchar_t* item : items) SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    return c;
}

HWND Button(int page, int id, const wchar_t* text, int x, int y, int w, int h, bool big = false) {
    return AddControl(page, L"BUTTON", text, WS_TABSTOP | WS_GROUP | BS_PUSHBUTTON, x, y, w, h, id, 0, big);
}

HWND Check(int page, int id, const wchar_t* text, int x, int y, int w, int h = 22) {
    return AddControl(page, L"BUTTON", text, WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX, x, y, w, h, id);
}

HWND Radio(int page, int id, const wchar_t* text, int x, int y, int w, bool first) {
    return AddControl(page, L"BUTTON", text, WS_TABSTOP | BS_AUTORADIOBUTTON | (first ? WS_GROUP : 0), x, y, w, 24, id);
}

void Enable(int id, bool enable) {
    if (HWND h = Ctl(id)) {
        if ((IsWindowEnabled(h) != FALSE) != enable) EnableWindow(h, enable ? TRUE : FALSE);
    }
}

void SetText(int id, const std::wstring& text) {
    HWND h = Ctl(id);
    if (!h) return;
    wchar_t current[256] = {};
    GetWindowTextW(h, current, 256);
    if (text != current) SetWindowTextW(h, text.c_str());
}

std::wstring GetText(int id) {
    HWND h = Ctl(id);
    if (!h) return L"";
    const int len = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(h, &s[0], len + 1);
    s.resize(static_cast<size_t>(len));
    return s;
}

int GetInt(int id, int def, int lo, int hi) {
    const std::wstring s = GetText(id);
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    const long long v = std::wcstoll(s.c_str(), &end, 10);
    if (end == s.c_str()) return def;
    return static_cast<int>(std::min<long long>(hi, std::max<long long>(lo, v)));
}

void SetInt(int id, long long v) { SetText(id, std::to_wstring(v)); }

bool IsChecked(int id) { return SendMessageW(Ctl(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void SetCheck(int id, bool on) { SendMessageW(Ctl(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

int ComboSel(int id) { return static_cast<int>(SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0)); }
void SetComboSel(int id, int index) { SendMessageW(Ctl(id), CB_SETCURSEL, static_cast<WPARAM>(index), 0); }

bool IsClass(HWND h, const wchar_t* cls) {
    wchar_t buf[64] = {};
    if (!h || !GetClassNameW(h, buf, 64)) return false;
    return lstrcmpiW(buf, cls) == 0;
}

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    vswprintf(buf, 512, fmt, args);
    va_end(args);
    return buf;
}

// ---------------------------------------------------------------------------
// ホットキー
// ---------------------------------------------------------------------------
std::wstring HotkeyText(const Hotkey& hk) {
    if (hk.vk == 0) return L"なし";
    std::wstring s;
    if (hk.mods & MOD_CONTROL) s += L"Ctrl+";
    if (hk.mods & MOD_SHIFT) s += L"Shift+";
    if (hk.mods & MOD_ALT) s += L"Alt+";
    if (hk.mods & MOD_WIN) s += L"Win+";
    return s + input::KeyName(hk.vk);
}

// ボタンなどに添える " [F6]" 形式の文字列
std::wstring HotkeySuffix(int index) {
    const Hotkey& hk = g.cfg.hotkeys[index];
    return hk.vk ? L"  [" + HotkeyText(hk) + L"]" : L"";
}

WORD ToHotkeyControl(const Hotkey& hk) {
    BYTE m = 0;
    if (hk.mods & MOD_SHIFT) m |= HOTKEYF_SHIFT;
    if (hk.mods & MOD_CONTROL) m |= HOTKEYF_CONTROL;
    if (hk.mods & MOD_ALT) m |= HOTKEYF_ALT;
    if (input::IsExtendedKey(hk.vk)) m |= HOTKEYF_EXT;
    return MAKEWORD(static_cast<BYTE>(hk.vk), m);
}

Hotkey FromHotkeyControl(WORD v) {
    Hotkey hk;
    hk.vk = LOBYTE(v);
    const BYTE m = HIBYTE(v);
    if (m & HOTKEYF_SHIFT) hk.mods |= MOD_SHIFT;
    if (m & HOTKEYF_CONTROL) hk.mods |= MOD_CONTROL;
    if (m & HOTKEYF_ALT) hk.mods |= MOD_ALT;
    return hk;
}

void UnregisterHotkeys() {
    for (int i = 0; i < kHotkeyCount; ++i) UnregisterHotKey(g.hwnd, i + 1);
}

// 登録できなかったホットキーの一覧を返す
std::wstring RegisterHotkeys() {
    UnregisterHotkeys();
    std::wstring failed;
    for (int i = 0; i < kHotkeyCount; ++i) {
        const Hotkey& hk = g.cfg.hotkeys[i];
        if (hk.vk == 0) continue;
        if (!RegisterHotKey(g.hwnd, i + 1, hk.mods | MOD_NOREPEAT, hk.vk)) {
            if (!failed.empty()) failed += L", ";
            failed += HotkeyText(hk);
        }
    }
    return failed;
}

// ホットキー入力欄にフォーカスがある間は、グローバルホットキーを一時的に解除する
// （解除しないと、今のキーを入力しようとしたときにホットキーが反応してしまう）
LRESULT CALLBACK HotkeyControlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_SETFOCUS: UnregisterHotkeys(); break;
        case WM_KILLFOCUS: RegisterHotkeys(); break;
        case WM_NCDESTROY: RemoveWindowSubclass(hwnd, HotkeyControlProc, 0); break;
        default: break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// 状態
// ---------------------------------------------------------------------------
bool Busy() {
    return g.clicker.Running() || g.player.Running() || recorder::IsRecording() || recorder::IsPicking();
}

void Sound(UINT type) {
    if (g.cfg.sound) MessageBeep(type);
}

void UpdateStatus();
void UpdateUi();

void ShowMessage(const std::wstring& text) {
    g.message = text;
    UpdateStatus();
}

void UpdateTitle() {
    std::wstring title = kAppTitle;
    if (!g.macroPath.empty()) {
        const size_t slash = g.macroPath.find_last_of(L"\\/");
        title += L" - " + (slash == std::wstring::npos ? g.macroPath : g.macroPath.substr(slash + 1));
    }
    if (g.dirty && !g.steps.empty()) title += L" *";
    SetWindowTextW(g.hwnd, title.c_str());
}

// ---------------------------------------------------------------------------
// 設定 <-> コントロール
// ---------------------------------------------------------------------------
void LoadControlsFromConfig() {
    const Config& c = g.cfg;
    SetInt(IDC_C_INTERVAL, c.intervalValue);
    SetComboSel(IDC_C_UNIT, c.intervalUnit);
    SetInt(IDC_C_JITTER, c.jitterMs);
    SetComboSel(IDC_C_BUTTON, c.button);
    SetComboSel(IDC_C_TYPE, c.clickType);
    SetCheck(IDC_C_REP_INF, c.repeatMode == 0);
    SetCheck(IDC_C_REP_COUNT, c.repeatMode == 1);
    SetCheck(IDC_C_REP_TIME, c.repeatMode == 2);
    SetInt(IDC_C_COUNT, c.repeatCount);
    SetInt(IDC_C_DURATION, c.durationSec);
    SetCheck(IDC_C_POS_CUR, c.positionMode == 0);
    SetCheck(IDC_C_POS_FIX, c.positionMode == 1);
    SetInt(IDC_C_X, c.posX);
    SetInt(IDC_C_Y, c.posY);
    SetCheck(IDC_C_RESTORE, c.restoreCursor);

    SetCheck(IDC_M_REC_MOVES, c.recordMoves);
    SetCheck(IDC_M_REC_KEYS, c.recordKeys);
    SetCheck(IDC_M_REC_APPEND, c.recordAppend);
    const int actionCount = static_cast<int>(macro::MouseActions().size());
    SetComboSel(IDC_M_ACTION, c.editAction < actionCount ? c.editAction : 0);
    SetInt(IDC_M_DELAY, c.editDelayMs);
    SetInt(IDC_M_X, 0);
    SetInt(IDC_M_Y, 0);
    SetInt(IDC_M_LOOPS, c.loops);
    SetInt(IDC_M_SPEED, c.speedPercent);
    SetInt(IDC_M_LOOPDELAY, c.loopDelayMs);

    SetCheck(IDC_TOPMOST, c.topmost);
    SetInt(IDC_S_START_DELAY, c.startDelaySec);
    SetCheck(IDC_S_CORNER, c.cornerStop);
    SetCheck(IDC_S_SOUND, c.sound);
    SetCheck(IDC_S_TRAY, c.trayOnMinimize);
    SetCheck(IDC_S_AUTOSAVE, c.autoSaveMacro);
    for (int i = 0; i < kHotkeyCount; ++i) {
        SendMessageW(Ctl(IDC_S_HOTKEY0 + i), HKM_SETHOTKEY, ToHotkeyControl(c.hotkeys[i]), 0);
    }
}

void SyncConfigFromControls() {
    Config& c = g.cfg;
    c.intervalValue = GetInt(IDC_C_INTERVAL, c.intervalValue, 1, kMaxMs);
    c.intervalUnit = std::max(0, ComboSel(IDC_C_UNIT));
    c.jitterMs = GetInt(IDC_C_JITTER, c.jitterMs, 0, kMaxMs);
    c.button = std::max(0, ComboSel(IDC_C_BUTTON));
    c.clickType = std::max(0, ComboSel(IDC_C_TYPE));
    c.repeatMode = IsChecked(IDC_C_REP_COUNT) ? 1 : (IsChecked(IDC_C_REP_TIME) ? 2 : 0);
    c.repeatCount = GetInt(IDC_C_COUNT, c.repeatCount, 1, kMaxCount);
    c.durationSec = GetInt(IDC_C_DURATION, c.durationSec, 1, kMaxMs / 1000);
    c.positionMode = IsChecked(IDC_C_POS_FIX) ? 1 : 0;
    c.posX = GetInt(IDC_C_X, c.posX, -100000, 100000);
    c.posY = GetInt(IDC_C_Y, c.posY, -100000, 100000);
    c.restoreCursor = IsChecked(IDC_C_RESTORE);

    c.recordMoves = IsChecked(IDC_M_REC_MOVES);
    c.recordKeys = IsChecked(IDC_M_REC_KEYS);
    c.recordAppend = IsChecked(IDC_M_REC_APPEND);
    c.editDelayMs = GetInt(IDC_M_DELAY, c.editDelayMs, 0, kMaxMs);
    c.loops = GetInt(IDC_M_LOOPS, c.loops, 0, kMaxCount);
    c.speedPercent = GetInt(IDC_M_SPEED, c.speedPercent, 1, 10000);
    c.loopDelayMs = GetInt(IDC_M_LOOPDELAY, c.loopDelayMs, 0, kMaxMs);

    c.topmost = IsChecked(IDC_TOPMOST);
    c.startDelaySec = GetInt(IDC_S_START_DELAY, c.startDelaySec, 0, 3600);
    c.cornerStop = IsChecked(IDC_S_CORNER);
    c.sound = IsChecked(IDC_S_SOUND);
    c.trayOnMinimize = IsChecked(IDC_S_TRAY);
    c.autoSaveMacro = IsChecked(IDC_S_AUTOSAVE);
    c.tab = g.page;
}

// 入力欄の値を正規化した値で書き戻す（空欄や範囲外の値を直す）
void NormalizeClickerFields() {
    const Config& c = g.cfg;
    SetInt(IDC_C_INTERVAL, c.intervalValue);
    SetInt(IDC_C_JITTER, c.jitterMs);
    SetInt(IDC_C_COUNT, c.repeatCount);
    SetInt(IDC_C_DURATION, c.durationSec);
    SetInt(IDC_C_X, c.posX);
    SetInt(IDC_C_Y, c.posY);
    SetInt(IDC_S_START_DELAY, c.startDelaySec);
}

void NormalizeMacroFields() {
    const Config& c = g.cfg;
    SetInt(IDC_M_LOOPS, c.loops);
    SetInt(IDC_M_SPEED, c.speedPercent);
    SetInt(IDC_M_LOOPDELAY, c.loopDelayMs);
    SetInt(IDC_S_START_DELAY, c.startDelaySec);
}

void UpdateHotkeyTexts() {
    SetText(IDC_C_START, L"▶ 開始" + HotkeySuffix(kHotkeyClicker));
    SetText(IDC_C_STOP, L"■ 停止" + HotkeySuffix(kHotkeyClicker));
    SetText(IDC_M_RECORD, (recorder::IsRecording() ? L"■ 記録停止" : L"● 記録開始") + HotkeySuffix(kHotkeyRecord));
    SetText(IDC_M_PLAY, L"▶ 再生" + HotkeySuffix(kHotkeyPlay));
    SetText(IDC_M_STOP, L"■ 停止" + HotkeySuffix(kHotkeyPlay));
    SetText(IDC_M_ADD, g.cfg.hotkeys[kHotkeyAddPoint].vk ? L"追加 [" + HotkeyText(g.cfg.hotkeys[kHotkeyAddPoint]) + L"]" : L"追加");

    const Hotkey& add = g.cfg.hotkeys[kHotkeyAddPoint];
    SetText(IDC_M_EDIT_GROUP, add.vk ? L"ステップの編集（[" + HotkeyText(add) + L"] でカーソル位置を追加）"
                                     : L"ステップの編集");

    std::wstring hint;
    const Hotkey& start = g.cfg.hotkeys[kHotkeyClicker];
    if (start.vk) hint += L"[" + HotkeyText(start) + L"] でどの画面からでも開始 / 停止できます。\n";
    if (g.cfg.cornerStop) hint += L"緊急停止: マウスを画面の左上隅へ動かすと止まります。";
    SetText(IDC_C_HINT, hint);
}

// ---------------------------------------------------------------------------
// マクロ一覧
// ---------------------------------------------------------------------------
int FirstSelected() { return ListView_GetNextItem(g.list, -1, LVNI_SELECTED); }

std::vector<int> SelectedIndices() {
    std::vector<int> v;
    int i = -1;
    while ((i = ListView_GetNextItem(g.list, i, LVNI_SELECTED)) >= 0) v.push_back(i);
    return v;
}

void SelectOnly(int index) {
    ListView_SetItemState(g.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (index >= 0 && index < static_cast<int>(g.steps.size())) {
        ListView_SetItemState(g.list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g.list, index, FALSE);
    }
}

void RefreshList() {
    ListView_SetItemCountEx(g.list, static_cast<int>(g.steps.size()), LVSICF_NOSCROLL);
    InvalidateRect(g.list, nullptr, FALSE);
    UpdateTitle();
}

void PushUndo() {
    g.undo.push_back(g.steps);
    if (g.undo.size() > kUndoLimit) g.undo.erase(g.undo.begin());
}

void MarkDirty() {
    g.dirty = true;
    UpdateTitle();
}

void UpdateEditorEnables() {
    const bool editable = !g.player.Running() && !recorder::IsRecording();
    const int sel = FirstSelected();
    const int count = static_cast<int>(g.steps.size());
    const bool keyStep = sel >= 0 && sel < count && g.steps[static_cast<size_t>(sel)].IsKey();

    bool hasPosition = false;
    if (!keyStep) {
        int a = ComboSel(IDC_M_ACTION);
        if (a < 0) a = 0;
        hasPosition = macro::MouseActions()[static_cast<size_t>(a)].type != StepType::Wait;
    }

    Enable(IDC_M_ACTION, editable && !keyStep);
    Enable(IDC_M_DELAY, editable);
    Enable(IDC_M_X, editable && hasPosition);
    Enable(IDC_M_Y, editable && hasPosition);
    Enable(IDC_M_PICK, editable && hasPosition && !Busy());
    Enable(IDC_M_APPLY, editable && sel >= 0);
    Enable(IDC_M_ADD, editable);
    Enable(IDC_M_UP, editable && sel > 0);
    Enable(IDC_M_DOWN, editable && sel >= 0 && sel < count - 1);
    Enable(IDC_M_DELETE, editable && sel >= 0);
    Enable(IDC_M_CLEAR, editable && count > 0);
    Enable(IDC_M_SETALL, editable && count > 0);
    Enable(IDC_M_OPEN, editable);
    Enable(IDC_M_SAVE, !recorder::IsRecording() && count > 0);
    Enable(IDC_M_REC_MOVES, editable);
    Enable(IDC_M_REC_KEYS, editable);
    Enable(IDC_M_REC_APPEND, editable);
}

// 選択中のステップの内容を編集欄に表示する
void LoadEditorFromSelection() {
    const int sel = FirstSelected();
    if (sel >= 0 && sel < static_cast<int>(g.steps.size())) {
        const Step& s = g.steps[static_cast<size_t>(sel)];
        SetComboSel(IDC_M_ACTION, macro::FindMouseAction(s));
        SetInt(IDC_M_DELAY, s.delayMs);
        if (s.HasPosition()) {
            SetInt(IDC_M_X, s.x);
            SetInt(IDC_M_Y, s.y);
        }
    } else if (ComboSel(IDC_M_ACTION) < 0) {
        SetComboSel(IDC_M_ACTION, g.cfg.editAction);
    }
    UpdateEditorEnables();
}

void ApplyEditor() {
    const int sel = FirstSelected();
    if (sel < 0) {
        ShowMessage(L"変更するステップを一覧から選択してください");
        return;
    }
    PushUndo();
    Step& s = g.steps[static_cast<size_t>(sel)];
    s.delayMs = GetInt(IDC_M_DELAY, s.delayMs, 0, kMaxMs);
    if (!s.IsKey()) {
        const int a = ComboSel(IDC_M_ACTION);
        if (a >= 0) macro::ApplyMouseAction(s, a);
        if (s.HasPosition()) {
            s.x = GetInt(IDC_M_X, s.x, -100000, 100000);
            s.y = GetInt(IDC_M_Y, s.y, -100000, 100000);
        }
    }
    MarkDirty();
    RefreshList();
    LoadEditorFromSelection();
    ShowMessage(Format(L"ステップ %d を変更しました", sel + 1));
}

void AddStep(bool atCursor) {
    if (g.player.Running() || recorder::IsRecording()) return;
    Step s;
    int a = ComboSel(IDC_M_ACTION);
    if (a < 0) a = 0;
    macro::ApplyMouseAction(s, a);
    s.delayMs = GetInt(IDC_M_DELAY, g.cfg.editDelayMs, 0, kMaxMs);
    if (atCursor) {
        POINT p{};
        GetCursorPos(&p);
        s.x = p.x;
        s.y = p.y;
    } else {
        s.x = GetInt(IDC_M_X, 0, -100000, 100000);
        s.y = GetInt(IDC_M_Y, 0, -100000, 100000);
    }

    PushUndo();
    const std::vector<int> sel = SelectedIndices();
    const int pos = sel.empty() ? static_cast<int>(g.steps.size()) : sel.back() + 1;
    g.steps.insert(g.steps.begin() + pos, s);
    MarkDirty();
    RefreshList();
    SelectOnly(pos);
    LoadEditorFromSelection();
    ShowMessage(Format(L"ステップ %d を追加: %ls %ls", pos + 1, macro::ActionText(s).c_str(),
                       macro::DetailText(s).c_str()));
    Sound(MB_OK);
}

void MoveSelected(int direction) {
    const int i = FirstSelected();
    const int j = i + direction;
    if (i < 0 || j < 0 || j >= static_cast<int>(g.steps.size())) return;
    PushUndo();
    std::swap(g.steps[static_cast<size_t>(i)], g.steps[static_cast<size_t>(j)]);
    MarkDirty();
    RefreshList();
    SelectOnly(j);
}

void DeleteSelected() {
    if (g.player.Running() || recorder::IsRecording()) return;
    const std::vector<int> sel = SelectedIndices();
    if (sel.empty()) return;
    PushUndo();
    for (auto it = sel.rbegin(); it != sel.rend(); ++it) g.steps.erase(g.steps.begin() + *it);
    MarkDirty();
    RefreshList();
    SelectOnly(std::min(sel.front(), static_cast<int>(g.steps.size()) - 1));
    LoadEditorFromSelection();
    ShowMessage(Format(L"%d 件のステップを削除しました（Ctrl+Z で元に戻せます）", static_cast<int>(sel.size())));
}

void ClearSteps() {
    if (g.steps.empty()) return;
    if (MessageBoxW(g.hwnd, L"すべてのステップを削除しますか？", kAppTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    PushUndo();
    g.steps.clear();
    MarkDirty();
    RefreshList();
    LoadEditorFromSelection();
    ShowMessage(L"すべてのステップを削除しました（Ctrl+Z で元に戻せます）");
}

void SetAllDelays() {
    if (g.steps.empty()) return;
    const int delay = GetInt(IDC_M_DELAY, g.cfg.editDelayMs, 0, kMaxMs);
    const std::wstring q = Format(L"すべてのステップ（%d 件）の待機時間を %d ms にしますか？",
                                  static_cast<int>(g.steps.size()), delay);
    if (MessageBoxW(g.hwnd, q.c_str(), kAppTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    PushUndo();
    for (Step& s : g.steps) s.delayMs = delay;
    MarkDirty();
    RefreshList();
    ShowMessage(Format(L"待機時間を %d ms に揃えました", delay));
}

void Undo() {
    if (g.player.Running() || recorder::IsRecording()) return;
    if (g.undo.empty()) {
        ShowMessage(L"元に戻せる操作はありません");
        return;
    }
    g.steps = std::move(g.undo.back());
    g.undo.pop_back();
    MarkDirty();
    RefreshList();
    SelectOnly(-1);
    LoadEditorFromSelection();
    ShowMessage(L"元に戻しました");
}

const wchar_t kMacroFilter[] = L"マクロファイル (*.acm)\0*.acm\0テキストファイル (*.txt)\0*.txt\0すべてのファイル (*.*)\0*.*\0";

bool SaveMacroTo(const std::wstring& path) {
    std::wstring error;
    if (!macro::SaveFile(path, g.steps, &error)) {
        MessageBoxW(g.hwnd, error.c_str(), kAppTitle, MB_OK | MB_ICONERROR);
        return false;
    }
    g.macroPath = path;
    g.dirty = false;
    UpdateTitle();
    ShowMessage(L"保存しました: " + path);
    return true;
}

bool SaveMacroAs() {
    wchar_t file[MAX_PATH * 4] = L"macro.acm";
    if (!g.macroPath.empty() && g.macroPath.size() < std::size(file)) {
        wcsncpy(file, g.macroPath.c_str(), std::size(file) - 1);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = kMacroFilter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrDefExt = L"acm";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;
    return SaveMacroTo(file);
}

void QuickSave() {
    if (g.steps.empty()) return;
    if (g.macroPath.empty()) SaveMacroAs();
    else SaveMacroTo(g.macroPath);
}

// 未保存の変更があれば確認する。続行してよければ true
bool ConfirmDiscard(const wchar_t* action) {
    if (!g.dirty || g.steps.empty()) return true;
    const std::wstring q = std::wstring(L"現在のマクロは保存されていません。\n保存してから") + action + L"か？";
    const int r = MessageBoxW(g.hwnd, q.c_str(), kAppTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return g.macroPath.empty() ? SaveMacroAs() : SaveMacroTo(g.macroPath);
    return true;
}

void OpenMacro() {
    if (g.player.Running() || recorder::IsRecording()) return;
    if (!ConfirmDiscard(L"開きます")) return;
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = kMacroFilter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrDefExt = L"acm";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;

    std::vector<Step> loaded;
    std::wstring error;
    if (!macro::LoadFile(file, loaded, &error)) {
        MessageBoxW(g.hwnd, (L"読み込みに失敗しました。\n" + error).c_str(), kAppTitle, MB_OK | MB_ICONERROR);
        return;
    }
    PushUndo();
    g.steps = std::move(loaded);
    g.macroPath = file;
    g.dirty = false;
    RefreshList();
    SelectOnly(-1);
    LoadEditorFromSelection();
    UpdateUi();
    ShowMessage(Format(L"%d ステップを読み込みました", static_cast<int>(g.steps.size())));
}

// ---------------------------------------------------------------------------
// 連打 / 記録 / 再生
// ---------------------------------------------------------------------------
void OnWorkerStopped(int id, StopReason reason) {
    const wchar_t* what = id == kWorkerClicker ? L"連打" : L"再生";
    switch (reason) {
        case StopReason::Completed:
            ShowMessage(std::wstring(what) + L"が完了しました");
            Sound(MB_ICONASTERISK);
            break;
        case StopReason::User:
            ShowMessage(std::wstring(what) + L"を停止しました");
            Sound(MB_ICONASTERISK);
            break;
        case StopReason::Failsafe:
            ShowMessage(std::wstring(what) + L"を緊急停止しました（画面の左上隅）");
            Sound(MB_ICONHAND);
            break;
    }
    g.highlightedStep = -1;
    UpdateUi();
}

void StopWorker(Worker& w, int id) {
    if (!w.Running()) return;
    w.Stop();
    g.handledGeneration[id] = w.Generation();
    OnWorkerStopped(id, StopReason::User);
}

void ToggleClicker() {
    if (g.clicker.Running()) {
        StopWorker(g.clicker, kWorkerClicker);
        return;
    }
    if (Busy()) {
        ShowMessage(L"他の処理を実行中のため開始できません");
        return;
    }
    SyncConfigFromControls();
    NormalizeClickerFields();
    const Config& c = g.cfg;
    static const int kUnitMs[] = {1, 1000, 60000};
    ClickerConfig cc;
    const long long interval = static_cast<long long>(c.intervalValue) * kUnitMs[std::min(2, c.intervalUnit)];
    cc.intervalMs = static_cast<int>(std::min<long long>(kMaxMs, std::max<long long>(1, interval)));
    cc.jitterMs = c.jitterMs;
    cc.button = static_cast<MouseButton>(std::min(2, c.button));
    cc.clicks = c.clickType == 1 ? 2 : 1;
    cc.repeat = c.repeatMode == 1 ? ClickerConfig::Repeat::Count
                                  : (c.repeatMode == 2 ? ClickerConfig::Repeat::Duration : ClickerConfig::Repeat::Infinite);
    cc.repeatCount = c.repeatCount;
    cc.durationSec = c.durationSec;
    cc.fixedPosition = c.positionMode == 1;
    cc.x = c.posX;
    cc.y = c.posY;
    cc.restoreCursor = c.restoreCursor;
    cc.startDelayMs = c.startDelaySec * 1000;
    cc.cornerStop = c.cornerStop;
    cc.ownWindow = g.hwnd;
    if (!g.clicker.Start(g.hwnd, cc)) {
        MessageBoxW(g.hwnd, L"連打を開始できませんでした。", kAppTitle, MB_OK | MB_ICONERROR);
        return;
    }
    g.message.clear();
    Sound(MB_OK);
    UpdateUi();
}

void TogglePlay() {
    if (g.player.Running()) {
        StopWorker(g.player, kWorkerPlayer);
        return;
    }
    if (Busy()) {
        ShowMessage(L"他の処理を実行中のため再生できません");
        return;
    }
    if (g.steps.empty()) {
        ShowMessage(L"再生するステップがありません。記録するか、ステップを追加してください");
        Sound(MB_ICONEXCLAMATION);
        return;
    }
    SyncConfigFromControls();
    NormalizeMacroFields();
    PlayerConfig pc;
    pc.loops = g.cfg.loops;
    pc.speedPercent = g.cfg.speedPercent;
    pc.loopDelayMs = g.cfg.loopDelayMs;
    pc.startDelayMs = g.cfg.startDelaySec * 1000;
    pc.cornerStop = g.cfg.cornerStop;
    if (!g.player.Start(g.hwnd, g.steps, pc)) {
        MessageBoxW(g.hwnd, L"再生を開始できませんでした。", kAppTitle, MB_OK | MB_ICONERROR);
        return;
    }
    g.message.clear();
    g.highlightedStep = -1;
    Sound(MB_OK);
    UpdateUi();
}

void StopRecording() {
    std::vector<Step> recorded = recorder::Stop();
    if (recorded.empty()) {
        ShowMessage(L"記録された操作はありませんでした");
    } else {
        PushUndo();
        if (g.cfg.recordAppend) {
            g.steps.insert(g.steps.end(), recorded.begin(), recorded.end());
        } else {
            g.steps = std::move(recorded);
            g.macroPath.clear();
        }
        MarkDirty();
        ShowMessage(Format(L"記録を終了しました（全 %d ステップ）。Ctrl+Z で記録前に戻せます",
                           static_cast<int>(g.steps.size())));
    }
    RefreshList();
    SelectOnly(-1);
    LoadEditorFromSelection();
    Sound(MB_ICONASTERISK);
    UpdateUi();
}

void ToggleRecord() {
    if (recorder::IsRecording()) {
        StopRecording();
        return;
    }
    if (Busy()) {
        ShowMessage(L"他の処理を実行中のため記録できません");
        return;
    }
    SyncConfigFromControls();
    recorder::Options o;
    o.recordMoves = g.cfg.recordMoves;
    o.recordKeys = g.cfg.recordKeys;
    for (const Hotkey& hk : g.cfg.hotkeys) {
        if (hk.vk) o.ignoreVks.push_back(hk.vk);
    }
    if (!recorder::Start(o)) {
        MessageBoxW(g.hwnd, L"記録を開始できませんでした。", kAppTitle, MB_OK | MB_ICONERROR);
        return;
    }
    g.message.clear();
    Sound(MB_OK);
    UpdateUi();
}

void BeginPick(int target) {
    if (Busy()) return;
    if (!recorder::BeginPick(g.hwnd)) {
        MessageBoxW(g.hwnd, L"位置の選択を開始できませんでした。", kAppTitle, MB_OK | MB_ICONERROR);
        return;
    }
    g.pickTarget = target;
    UpdateUi();
}

void FinishPick(bool ok, int x, int y) {
    recorder::EndPick();
    const int target = g.pickTarget;
    g.pickTarget = 0;
    if (ok) {
        if (target == 1) {
            SetInt(IDC_C_X, x);
            SetInt(IDC_C_Y, y);
            SetCheck(IDC_C_POS_CUR, false);
            SetCheck(IDC_C_POS_FIX, true);
        } else if (target == 2) {
            SetInt(IDC_M_X, x);
            SetInt(IDC_M_Y, y);
        }
        ShowMessage(Format(L"位置を取得しました: (%d, %d)%ls", x, y,
                           target == 2 ? L"　「変更」または「追加」で反映します" : L""));
        Sound(MB_OK);
    } else {
        ShowMessage(L"位置の選択をキャンセルしました");
    }
    UpdateUi();
}

// ---------------------------------------------------------------------------
// 表示の更新
// ---------------------------------------------------------------------------
void EnsureFocus() {
    HWND f = GetFocus();
    const bool foreground = GetForegroundWindow() == g.hwnd;
    auto usable = [](HWND h) { return h && IsWindow(h) && IsWindowEnabled(h) && IsWindowVisible(h); };
    if (foreground && usable(f) && f != g.hwnd) return;
    if (!foreground && usable(g.lastFocus)) return;

    HWND preferred = g.tab;
    if (g.page == kPageClicker) {
        preferred = Ctl(g.clicker.Running() ? IDC_C_STOP : IDC_C_START);
    } else if (g.page == kPageMacro) {
        preferred = Ctl(g.player.Running() ? IDC_M_STOP : (recorder::IsRecording() ? IDC_M_RECORD : IDC_M_PLAY));
    }
    if (!usable(preferred)) preferred = g.tab;
    if (foreground) SetFocus(preferred);
    else g.lastFocus = preferred;
}

void UpdateClickerEnables() {
    const bool fixed = IsChecked(IDC_C_POS_FIX);
    Enable(IDC_C_COUNT, IsChecked(IDC_C_REP_COUNT));
    Enable(IDC_C_DURATION, IsChecked(IDC_C_REP_TIME));
    Enable(IDC_C_X, fixed);
    Enable(IDC_C_Y, fixed);
    Enable(IDC_C_PICK, !Busy());
    Enable(IDC_C_RESTORE, fixed);
}

void UpdateTray(const std::wstring& tip) {
    if (!g.trayIcon) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    const std::wstring text = std::wstring(kAppTitle) + L"\n" + tip;
    wcsncpy(nid.szTip, text.c_str(), std::size(nid.szTip) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void UpdateStatus() {
    std::wstring s;
    if (recorder::IsPicking()) {
        s = L"位置の選択中: 目的の場所を左クリック（右クリック / Esc でキャンセル）";
    } else if (recorder::IsRecording()) {
        s = Format(L"● 記録中: %d 件の操作", static_cast<int>(recorder::EventCount()));
        if (g.cfg.hotkeys[kHotkeyRecord].vk) s += L"　[" + HotkeyText(g.cfg.hotkeys[kHotkeyRecord]) + L"] で停止";
    } else if (g.clicker.Running()) {
        if (g.clicker.Waiting()) {
            s = L"開始待機中...";
        } else {
            s = Format(L"▶ 連打中: %lld 回", g.clicker.Count());
            if (g.clicker.Config().repeat == ClickerConfig::Repeat::Count) {
                s += Format(L" / %lld 回", g.clicker.Config().repeatCount);
            }
        }
    } else if (g.player.Running()) {
        if (g.player.Waiting()) {
            s = L"開始待機中...";
        } else {
            const int loops = g.player.Config().loops;
            s = Format(L"▶ 再生中: ループ %d / %ls　ステップ %d / %d", g.player.CurrentLoop(),
                       loops == 0 ? L"∞" : std::to_wstring(loops).c_str(), g.player.CurrentStep() + 1,
                       g.player.StepCount());
        }
    } else {
        s = g.message.empty() ? L"待機中" : g.message;
    }
    if (s != g.lastStatus) {
        g.lastStatus = s;
        SendMessageW(g.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(s.c_str()));
        UpdateTray(s);
    }

    POINT p{};
    if (GetCursorPos(&p)) {
        const std::wstring c = Format(L"X: %d　Y: %d", static_cast<int>(p.x), static_cast<int>(p.y));
        if (c != g.lastCursorText) {
            g.lastCursorText = c;
            SendMessageW(g.status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(c.c_str()));
        }
    }

    // 再生中は実行中のステップを一覧で強調表示する
    if (g.player.Running() && !g.player.Waiting()) {
        const int step = g.player.CurrentStep();
        if (step != g.highlightedStep && step >= 0 && step < static_cast<int>(g.steps.size())) {
            g.highlightedStep = step;
            SelectOnly(step);
        }
    }
}

void UpdateUi() {
    const bool clicking = g.clicker.Running();
    const bool playing = g.player.Running();
    const bool recording = recorder::IsRecording();
    const bool busy = Busy();

    Enable(IDC_C_START, !busy);
    Enable(IDC_C_STOP, clicking);
    Enable(IDC_M_RECORD, recording || !busy);
    Enable(IDC_M_PLAY, !busy);
    Enable(IDC_M_STOP, playing);
    UpdateHotkeyTexts();
    UpdateClickerEnables();
    UpdateEditorEnables();
    EnsureFocus();
    g.lastStatus.clear();
    UpdateStatus();
}

void ShowPage(int page) {
    if (page < 0 || page >= kPageCount) page = kPageClicker;
    g.page = page;
    if (TabCtrl_GetCurSel(g.tab) != page) TabCtrl_SetCurSel(g.tab, page);
    for (int i = 0; i < kPageCount; ++i) {
        if (i != page) ShowWindow(g.pages[i], SW_HIDE);
    }
    ShowWindow(g.pages[page], SW_SHOW);
    EnsureFocus();
}

// ---------------------------------------------------------------------------
// タスクトレイ
// ---------------------------------------------------------------------------
void AddTrayIcon() {
    if (g.trayIcon) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g.iconSmall;
    wcsncpy(nid.szTip, kAppTitle, std::size(nid.szTip) - 1);
    g.trayIcon = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    g.lastStatus.clear();
    UpdateStatus();
}

void RemoveTrayIcon() {
    if (!g.trayIcon) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g.trayIcon = false;
}

void RestoreWindow() {
    ShowWindow(g.hwnd, SW_SHOW);
    if (IsIconic(g.hwnd)) ShowWindow(g.hwnd, SW_RESTORE);
    SetForegroundWindow(g.hwnd);
    RemoveTrayIcon();
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW, L"ウィンドウを表示");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_CLICKER, g.clicker.Running() ? L"連打を停止" : L"連打を開始");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_PLAY, g.player.Running() ? L"再生を停止" : L"マクロを再生");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"終了");
    SetMenuDefaultItem(menu, IDM_TRAY_SHOW, FALSE);
    POINT p{};
    GetCursorPos(&p);
    SetForegroundWindow(g.hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, g.hwnd, nullptr);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void ApplyTopmost() {
    SetWindowPos(g.hwnd, g.cfg.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// コントロールの作成とレイアウト
// ---------------------------------------------------------------------------
void CreateClickerPage() {
    const int P = kPageClicker;
    Group(P, L"クリック間隔", 8, 6, 440, 62);
    Label(P, L"間隔", 22, 30, 40);
    Edit(P, IDC_C_INTERVAL, 64, 30, 84, true);
    Combo(P, IDC_C_UNIT, 154, 30, 84, {L"ミリ秒", L"秒", L"分"});
    Label(P, L"ランダム ±", 254, 30, 76);
    Edit(P, IDC_C_JITTER, 332, 30, 64, true);
    Label(P, L"ms", 400, 30, 36);

    Group(P, L"クリックの種類", 8, 74, 440, 62);
    Label(P, L"ボタン", 22, 98, 44);
    Combo(P, IDC_C_BUTTON, 68, 98, 110, {L"左ボタン", L"右ボタン", L"中ボタン"});
    Label(P, L"種類", 196, 98, 40);
    Combo(P, IDC_C_TYPE, 238, 98, 150, {L"シングルクリック", L"ダブルクリック"});

    Group(P, L"繰り返し", 8, 142, 440, 104);
    Radio(P, IDC_C_REP_INF, L"停止するまで繰り返す", 22, 164, 300, true);
    Radio(P, IDC_C_REP_COUNT, L"回数を指定", 22, 190, 120, false);
    Edit(P, IDC_C_COUNT, 146, 190, 96, true, false);
    Label(P, L"回", 248, 190, 40);
    Radio(P, IDC_C_REP_TIME, L"時間を指定", 22, 216, 120, false);
    Edit(P, IDC_C_DURATION, 146, 216, 96, true, false);
    Label(P, L"秒間", 248, 216, 60);

    Group(P, L"クリックする位置", 8, 252, 440, 110);
    Radio(P, IDC_C_POS_CUR, L"現在のカーソル位置", 22, 274, 300, true);
    Radio(P, IDC_C_POS_FIX, L"指定した位置", 22, 302, 112, false);
    Label(P, L"X", 138, 302, 16);
    Edit(P, IDC_C_X, 156, 302, 64, false, false);
    Label(P, L"Y", 228, 302, 16);
    Edit(P, IDC_C_Y, 246, 302, 64, false, false);
    Button(P, IDC_C_PICK, L"位置を選択...", 320, 300, 116, 28);
    Check(P, IDC_C_RESTORE, L"クリック後にカーソルを元の位置に戻す", 22, 330, 400, 24);

    Button(P, IDC_C_START, L"▶ 開始", 8, 372, 214, 46, true);
    Button(P, IDC_C_STOP, L"■ 停止", 234, 372, 214, 46, true);
    MultiLabel(P, L"", 10, 428, 436, 44, IDC_C_HINT);
}

void CreateMacroPage() {
    const int P = kPageMacro;
    Button(P, IDC_M_RECORD, L"● 記録開始", 8, 6, 154, 36);
    Check(P, IDC_M_REC_MOVES, L"マウス移動も記録", 172, 2, 150, 20);
    Check(P, IDC_M_REC_KEYS, L"キー入力も記録", 172, 22, 150, 20);
    Check(P, IDC_M_REC_APPEND, L"末尾に追加して記録", 324, 2, 124, 20);

    g.list = AddControl(P, WC_LISTVIEWW, L"",
                        WS_TABSTOP | WS_GROUP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, 8, 46, 440, 166,
                        IDC_M_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SetWindowTheme(g.list, L"Explorer", nullptr);
    const wchar_t* columns[] = {L"#", L"動作", L"位置 / キー", L"待機 (ms)"};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = (i == 0 || i == 3) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        col.pszText = const_cast<wchar_t*>(columns[i]);
        col.cx = 60;
        ListView_InsertColumn(g.list, i, &col);
    }

    Group(P, L"ステップの編集", 8, 216, 440, 88, IDC_M_EDIT_GROUP);
    Label(P, L"動作", 20, 238, 36);
    HWND action = Combo(P, IDC_M_ACTION, 58, 238, 156, {});
    for (const MouseAction& a : macro::MouseActions()) {
        SendMessageW(action, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(a.name));
    }
    Label(P, L"待機", 226, 238, 36);
    Edit(P, IDC_M_DELAY, 264, 238, 84, true);
    Label(P, L"ms", 352, 238, 30);
    Label(P, L"X", 20, 268, 16);
    Edit(P, IDC_M_X, 38, 268, 60, false);
    Label(P, L"Y", 106, 268, 16);
    Edit(P, IDC_M_Y, 124, 268, 60, false);
    Button(P, IDC_M_PICK, L"位置を選択", 192, 267, 84, 26);
    Button(P, IDC_M_APPLY, L"変更", 282, 267, 70, 26);
    Button(P, IDC_M_ADD, L"追加", 358, 267, 82, 26);

    Button(P, IDC_M_UP, L"▲", 8, 310, 38, 28);
    Button(P, IDC_M_DOWN, L"▼", 50, 310, 38, 28);
    Button(P, IDC_M_DELETE, L"削除", 92, 310, 56, 28);
    Button(P, IDC_M_CLEAR, L"全削除", 152, 310, 60, 28);
    Button(P, IDC_M_SETALL, L"待機を一括設定", 216, 310, 88, 28);
    Button(P, IDC_M_OPEN, L"開く...", 308, 310, 68, 28);
    Button(P, IDC_M_SAVE, L"保存...", 380, 310, 68, 28);

    Group(P, L"再生", 8, 344, 440, 84);
    Label(P, L"繰り返し回数", 20, 366, 88);
    Edit(P, IDC_M_LOOPS, 110, 366, 60, true);
    Label(P, L"回（0 = 無限）", 176, 366, 110);
    Label(P, L"速度", 296, 366, 36);
    Edit(P, IDC_M_SPEED, 334, 366, 56, true);
    Label(P, L"%", 394, 366, 30);
    Label(P, L"ループ間の待機", 20, 394, 104);
    Edit(P, IDC_M_LOOPDELAY, 126, 394, 70, true);
    Label(P, L"ms", 200, 394, 30);

    Button(P, IDC_M_PLAY, L"▶ 再生", 8, 434, 214, 40, true);
    Button(P, IDC_M_STOP, L"■ 停止", 234, 434, 214, 40, true);
}

void CreateSettingsPage() {
    const int P = kPageSettings;
    Group(P, L"ホットキー（欄をクリックして新しいキーを押す）", 8, 6, 440, 172);
    const wchar_t* labels[kHotkeyCount] = {L"連打の開始 / 停止", L"マクロ再生の開始 / 停止", L"記録の開始 / 停止",
                                           L"カーソル位置をステップに追加"};
    for (int i = 0; i < kHotkeyCount; ++i) {
        const int y = 30 + i * 28;
        Label(P, labels[i], 22, y, 210);
        HWND hk = AddControl(P, HOTKEY_CLASSW, L"", WS_TABSTOP | WS_GROUP, 236, y, 160, 24, IDC_S_HOTKEY0 + i,
                             WS_EX_CLIENTEDGE);
        SetWindowSubclass(hk, HotkeyControlProc, 0, 0);
    }
    Button(P, IDC_S_HOTKEY_APPLY, L"ホットキーを適用", 236, 142, 160, 28);

    Group(P, L"動作", 8, 186, 440, 150);
    Label(P, L"開始前の待機", 22, 208, 100);
    Edit(P, IDC_S_START_DELAY, 124, 208, 60, true);
    Label(P, L"秒（連打・再生の開始時）", 190, 208, 220);
    Check(P, IDC_S_CORNER, L"マウスを画面の左上隅に移動したら緊急停止する", 22, 236, 410);
    Check(P, IDC_S_SOUND, L"開始・停止時に音を鳴らす", 22, 260, 410);
    Check(P, IDC_S_TRAY, L"最小化したらタスクトレイに格納する", 22, 284, 410);
    Check(P, IDC_S_AUTOSAVE, L"終了時にマクロを自動保存し、次回起動時に復元する", 22, 308, 410);

    Group(P, L"情報", 8, 344, 440, 128);
    Label(P, (std::wstring(kAppTitle) + L"  バージョン " + kVersion).c_str(), 20, 364, 416);
    Label(P, L"設定の保存先", 20, 390, 86);
    AddControl(P, L"STATIC", settings::DataDir().c_str(), SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_PATHELLIPSIS,
               108, 390, 220, 24, IDC_S_INFO);
    Button(P, IDC_S_OPEN_FOLDER, L"フォルダーを開く", 332, 389, 108, 26);
    MultiLabel(P,
               L"キーボード操作: Tab で項目移動 / Space・Enter で押す / Ctrl+Tab でタブ切替 / "
               L"Ctrl+S 保存 / Ctrl+O 開く / Ctrl+Z 元に戻す / Delete でステップ削除",
               20, 422, 416, 44);
}

LRESULT CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
        case WM_NOTIFY:
            return SendMessageW(g.hwnd, msg, wp, lp);
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            // タブの中は白背景。無効な入力欄（Edit）は標準の灰色のままにする
            HWND child = reinterpret_cast<HWND>(lp);
            if (msg == WM_CTLCOLORSTATIC && IsClass(child, L"Edit")) break;
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CreateControls() {
    g.tab = AddControl(kPageMain, WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, kTabX, kTabY, kTabW, kTabH, IDC_TAB);
    const wchar_t* tabs[kPageCount] = {L"連打", L"記録・再生", L"設定"};
    for (int i = 0; i < kPageCount; ++i) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(tabs[i]);
        TabCtrl_InsertItem(g.tab, i, &item);
    }
    for (int i = 0; i < kPageCount; ++i) {
        // タブの中身はページごとのコンテナーウィンドウに入れる（位置は Layout で決める）
        g.pages[i] = CreateWindowExW(WS_EX_CONTROLPARENT, kPageClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                                     g.hwnd, nullptr, g.inst, nullptr);
    }
    Check(kPageMain, IDC_TOPMOST, L"常に最前面に表示", 14, 528, 240, 22);

    CreateClickerPage();
    CreateMacroPage();
    CreateSettingsPage();

    g.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g.hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), g.inst, nullptr);

    // タブコントロールは最背面に置き、上に重なるページを描画で消さないようにする
    SetWindowPos(g.tab, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ApplyFonts() {
    for (const Control& c : g.controls) {
        SendMessageW(c.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(c.big ? g.bigFont : g.font), TRUE);
    }
    SendMessageW(g.status, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
}

void Layout() {
    // タブ内の表示領域を求めてページを配置する
    SetWindowPos(g.tab, nullptr, S(kTabX), S(kTabY), S(kTabW), S(kTabH), SWP_NOZORDER | SWP_NOACTIVATE);
    RECT display{0, 0, S(kTabW), S(kTabH)};
    TabCtrl_AdjustRect(g.tab, FALSE, &display);
    for (HWND page : g.pages) {
        SetWindowPos(page, nullptr, S(kTabX) + display.left, S(kTabY) + display.top, display.right - display.left,
                     display.bottom - display.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    for (const Control& c : g.controls) {
        if (c.hwnd == g.tab) continue;
        SetWindowPos(c.hwnd, nullptr, S(c.x), S(c.y), S(c.w), S(c.h), SWP_NOZORDER | SWP_NOACTIVATE);
    }

    const int widths[4] = {44, 128, 160, 84};
    for (int i = 0; i < 4; ++i) ListView_SetColumnWidth(g.list, i, S(widths[i]));

    // ステータスバーの高さを含めてウィンドウの大きさを決める
    SendMessageW(g.status, WM_SIZE, 0, 0);
    RECT sr{};
    GetWindowRect(g.status, &sr);
    const int statusHeight = sr.bottom - sr.top;
    int parts[2] = {S(kClientWidth - 130), -1};
    SendMessageW(g.status, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));

    RECT rc{0, 0, S(kClientWidth), S(kClientHeight) + statusHeight};
    AdjustRectForDpi(&rc, static_cast<DWORD>(GetWindowLongPtrW(g.hwnd, GWL_STYLE)),
                     static_cast<DWORD>(GetWindowLongPtrW(g.hwnd, GWL_EXSTYLE)), g.dpi);
    SetWindowPos(g.hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(g.status, WM_SIZE, 0, 0);
}

// ---------------------------------------------------------------------------
// 終了処理
// ---------------------------------------------------------------------------
void SaveAll() {
    SyncConfigFromControls();
    settings::Save(g.cfg);
    if (g.cfg.autoSaveMacro) macro::SaveFile(settings::LastMacroPath(), g.steps, nullptr);
}

// ---------------------------------------------------------------------------
// ウィンドウプロシージャ
// ---------------------------------------------------------------------------
void OnCommand(int id, int code) {
    switch (id) {
        case IDOK: {
            // Enter キー: フォーカスのあるボタンを押す / 編集欄なら変更を適用
            HWND f = GetFocus();
            const int fid = GetDlgCtrlID(f);
            if (IsClass(f, L"Button")) {
                const LONG_PTR type = GetWindowLongPtrW(f, GWL_STYLE) & BS_TYPEMASK;
                if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) SendMessageW(f, BM_CLICK, 0, 0);
            } else if (fid == IDC_M_DELAY || fid == IDC_M_X || fid == IDC_M_Y) {
                ApplyEditor();
            }
            break;
        }
        case IDCANCEL:
            break;

        case IDC_TOPMOST:
            g.cfg.topmost = IsChecked(IDC_TOPMOST);
            ApplyTopmost();
            break;

        // 連打
        case IDC_C_START:
            if (!g.clicker.Running()) ToggleClicker();
            break;
        case IDC_C_STOP:
            StopWorker(g.clicker, kWorkerClicker);
            break;
        case IDC_C_PICK:
            BeginPick(1);
            break;
        case IDC_C_REP_INF:
        case IDC_C_REP_COUNT:
        case IDC_C_REP_TIME:
        case IDC_C_POS_CUR:
        case IDC_C_POS_FIX:
            if (code == BN_CLICKED) UpdateClickerEnables();
            break;

        // マクロ
        case IDC_M_RECORD:
            ToggleRecord();
            break;
        case IDC_M_ACTION:
            if (code == CBN_SELCHANGE) {
                if (FirstSelected() < 0) g.cfg.editAction = std::max(0, ComboSel(IDC_M_ACTION));
                UpdateEditorEnables();
            }
            break;
        case IDC_M_PICK:
            BeginPick(2);
            break;
        case IDC_M_APPLY:
            ApplyEditor();
            break;
        case IDC_M_ADD:
            AddStep(false);
            break;
        case IDC_M_UP:
            MoveSelected(-1);
            break;
        case IDC_M_DOWN:
            MoveSelected(+1);
            break;
        case IDC_M_DELETE:
            DeleteSelected();
            break;
        case IDC_M_CLEAR:
            ClearSteps();
            break;
        case IDC_M_SETALL:
            SetAllDelays();
            break;
        case IDC_M_OPEN:
            OpenMacro();
            break;
        case IDC_M_SAVE:
            SaveMacroAs();
            break;
        case IDC_M_PLAY:
            if (!g.player.Running()) TogglePlay();
            break;
        case IDC_M_STOP:
            StopWorker(g.player, kWorkerPlayer);
            break;

        // 設定
        case IDC_S_HOTKEY_APPLY: {
            Hotkey next[kHotkeyCount];
            for (int i = 0; i < kHotkeyCount; ++i) {
                next[i] = FromHotkeyControl(static_cast<WORD>(SendMessageW(Ctl(IDC_S_HOTKEY0 + i), HKM_GETHOTKEY, 0, 0)));
            }
            for (int i = 0; i < kHotkeyCount; ++i) {
                for (int j = i + 1; j < kHotkeyCount; ++j) {
                    if (next[i].vk && next[i] == next[j]) {
                        MessageBoxW(g.hwnd, L"同じキーが複数の操作に割り当てられています。", kAppTitle,
                                    MB_OK | MB_ICONWARNING);
                        return;
                    }
                }
            }
            for (int i = 0; i < kHotkeyCount; ++i) g.cfg.hotkeys[i] = next[i];
            const std::wstring failed = RegisterHotkeys();
            UpdateUi();
            if (!failed.empty()) {
                MessageBoxW(g.hwnd, (L"次のホットキーは他のアプリが使用しているため登録できませんでした:\n" + failed).c_str(),
                            kAppTitle, MB_OK | MB_ICONWARNING);
            } else {
                ShowMessage(L"ホットキーを適用しました");
            }
            break;
        }
        case IDC_S_CORNER:
        case IDC_S_SOUND:
        case IDC_S_TRAY:
        case IDC_S_AUTOSAVE:
            SyncConfigFromControls();
            UpdateHotkeyTexts();
            break;

        case IDC_S_OPEN_FOLDER:
            ShellExecuteW(g.hwnd, L"open", settings::DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;

        // トレイメニュー
        case IDM_TRAY_SHOW:
            RestoreWindow();
            break;
        case IDM_TRAY_CLICKER:
            ToggleClicker();
            break;
        case IDM_TRAY_PLAY:
            TogglePlay();
            break;
        case IDM_TRAY_EXIT:
            SendMessageW(g.hwnd, WM_CLOSE, 0, 0);
            break;
        default:
            break;
    }
}

LRESULT OnNotify(NMHDR* nm) {
    if (nm->hwndFrom == g.tab && nm->code == TCN_SELCHANGE) {
        ShowPage(TabCtrl_GetCurSel(g.tab));
        return 0;
    }
    if (nm->hwndFrom != g.list) return 0;

    switch (nm->code) {
        case LVN_GETDISPINFOW: {
            auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
            const int i = di->item.iItem;
            if ((di->item.mask & LVIF_TEXT) && i >= 0 && i < static_cast<int>(g.steps.size()) && di->item.pszText &&
                di->item.cchTextMax > 0) {
                const Step& s = g.steps[static_cast<size_t>(i)];
                std::wstring text;
                switch (di->item.iSubItem) {
                    case 0: text = std::to_wstring(i + 1); break;
                    case 1: text = macro::ActionText(s); break;
                    case 2: text = macro::DetailText(s); break;
                    case 3: text = std::to_wstring(s.delayMs); break;
                    default: break;
                }
                wcsncpy(di->item.pszText, text.c_str(), static_cast<size_t>(di->item.cchTextMax) - 1);
                di->item.pszText[di->item.cchTextMax - 1] = L'\0';
            }
            return 0;
        }
        case LVN_ITEMCHANGED: {
            auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
            if ((lv->uChanged & LVIF_STATE) && ((lv->uOldState ^ lv->uNewState) & LVIS_SELECTED) &&
                !g.player.Running()) {
                LoadEditorFromSelection();
            }
            return 0;
        }
        case LVN_ODSTATECHANGED:
            if (!g.player.Running()) LoadEditorFromSelection();
            return 0;
        case LVN_KEYDOWN: {
            auto* kd = reinterpret_cast<NMLVKEYDOWN*>(nm);
            if (kd->wVKey == VK_DELETE) DeleteSelected();
            return 0;
        }
        case NM_DBLCLK:
            if (FirstSelected() >= 0) {
                HWND delay = Ctl(IDC_M_DELAY);
                SetFocus(delay);
                SendMessageW(delay, EM_SETSEL, 0, -1);
            }
            return 0;
        default:
            return 0;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g.taskbarCreatedMsg && msg == g.taskbarCreatedMsg) {
        // エクスプローラーが再起動したらトレイアイコンを作り直す
        if (g.trayIcon) {
            g.trayIcon = false;
            AddTrayIcon();
        }
        return 0;
    }
    if (g.showMsg && msg == g.showMsg) {
        RestoreWindow();
        return 0;
    }

    switch (msg) {
        case WM_CREATE: {
            g.hwnd = hwnd;
            g.dpi = WindowDpi(hwnd);
            CreateFonts();
            CreateControls();
            ApplyFonts();
            LoadControlsFromConfig();
            Layout();
            RefreshList();
            ShowPage(g.cfg.tab);
            ApplyTopmost();
            SetTimer(hwnd, kStatusTimer, 100, nullptr);
            PostMessageW(hwnd, WM_APP_STARTUP, 0, 0);
            UpdateUi();
            return 0;
        }
        case WM_APP_STARTUP: {
            const std::wstring failed = RegisterHotkeys();
            if (!failed.empty()) {
                MessageBoxW(hwnd,
                            (L"次のホットキーは他のアプリが使用しているため登録できませんでした:\n" + failed +
                             L"\n\n「設定」タブで別のキーを割り当ててください。")
                                .c_str(),
                            kAppTitle, MB_OK | MB_ICONWARNING);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            g.dpi = HIWORD(wp);
            CreateFonts();
            ApplyFonts();
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                HWND f = GetFocus();
                if (f && IsChild(hwnd, f)) g.lastFocus = f;
            } else if (g.lastFocus && IsWindow(g.lastFocus) && IsWindowVisible(g.lastFocus) &&
                       IsWindowEnabled(g.lastFocus)) {
                SetFocus(g.lastFocus);
            } else {
                EnsureFocus();
            }
            return 0;
        case WM_TIMER:
            if (wp == kStatusTimer) UpdateStatus();
            return 0;
        case WM_HOTKEY:
            switch (static_cast<int>(wp) - 1) {
                case kHotkeyClicker: ToggleClicker(); break;
                case kHotkeyPlay: TogglePlay(); break;
                case kHotkeyRecord: ToggleRecord(); break;
                case kHotkeyAddPoint:
                    if (!Busy() || g.clicker.Running()) AddStep(true);
                    break;
                default: break;
            }
            return 0;
        case WM_APP_WORKER_DONE: {
            const int id = LOWORD(wp);
            const WORD generation = HIWORD(wp);
            Worker* w = id == kWorkerClicker ? static_cast<Worker*>(&g.clicker)
                                             : (id == kWorkerPlayer ? static_cast<Worker*>(&g.player) : nullptr);
            if (w && generation == w->Generation() && g.handledGeneration[id] != generation) {
                w->Stop();
                g.handledGeneration[id] = generation;
                OnWorkerStopped(id, static_cast<StopReason>(lp));
            }
            return 0;
        }
        case WM_APP_PICK_DONE:
            FinishPick(true, static_cast<int>(static_cast<INT_PTR>(wp)), static_cast<int>(lp));
            return 0;
        case WM_APP_PICK_CANCEL:
            FinishPick(false, 0, 0);
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK:
                    RestoreWindow();
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowTrayMenu();
                    break;
                default:
                    break;
            }
            return 0;
        case WM_SIZE:
            SendMessageW(g.status, WM_SIZE, 0, 0);
            if (wp == SIZE_MINIMIZED && g.cfg.trayOnMinimize) {
                AddTrayIcon();
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;
        case WM_MOVE:
            if (!IsIconic(hwnd) && IsWindowVisible(hwnd)) {
                RECT r{};
                GetWindowRect(hwnd, &r);
                g.cfg.windowX = r.left;
                g.cfg.windowY = r.top;
            }
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_NOTIFY:
            return OnNotify(reinterpret_cast<NMHDR*>(lp));
        case WM_CLOSE:
            if (!g.cfg.autoSaveMacro && !ConfirmDiscard(L"終了します")) return 0;
            DestroyWindow(hwnd);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                g.clicker.Stop();
                g.player.Stop();
                SaveAll();
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kStatusTimer);
            g.clicker.Stop();
            g.player.Stop();
            if (recorder::IsRecording()) recorder::Stop();
            recorder::EndPick();
            UnregisterHotkeys();
            RemoveTrayIcon();
            SaveAll();
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// メッセージループで処理するショートカットキー
bool HandleShortcut(const MSG& msg) {
    if (msg.message != WM_KEYDOWN) return false;
    if (msg.hwnd != g.hwnd && !IsChild(g.hwnd, msg.hwnd)) return false;
    if (GetKeyState(VK_CONTROL) >= 0 || GetKeyState(VK_MENU) < 0) return false;
    HWND focus = GetFocus();
    if (IsClass(focus, HOTKEY_CLASSW)) return false;  // ホットキーの入力中はそのまま渡す

    const bool shift = GetKeyState(VK_SHIFT) < 0;
    switch (msg.wParam) {
        case VK_TAB:
            ShowPage((g.page + (shift ? kPageCount - 1 : 1)) % kPageCount);
            return true;
        case VK_NEXT:
            ShowPage((g.page + 1) % kPageCount);
            return true;
        case VK_PRIOR:
            ShowPage((g.page + kPageCount - 1) % kPageCount);
            return true;
        case 'S':
            QuickSave();
            return true;
        case 'O':
            OpenMacro();
            return true;
        case 'Z':
            if (IsClass(focus, L"Edit")) return false;  // 入力欄では通常の「元に戻す」
            Undo();
            return true;
        default:
            return false;
    }
}

// 既に起動していれば、そのウィンドウを前面に出して true を返す
bool ActivateExistingInstance() {
    HWND existing = FindWindowW(kClassName, nullptr);
    if (!existing) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(existing, &pid);
    AllowSetForegroundWindow(pid);
    PostMessageW(existing, RegisterWindowMessageW(kShowMessageName), 0, 0);
    return true;
}

}  // namespace

int RunApp(HINSTANCE instance, int showCommand) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        ActivateExistingInstance();
        CloseHandle(mutex);
        return 0;
    }

    g.inst = instance;
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_HOTKEY_CLASS | ICC_BAR_CLASSES |
                ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    settings::Load(g.cfg);
    if (g.cfg.autoSaveMacro) {
        std::vector<Step> last;
        if (macro::LoadFile(settings::LastMacroPath(), last, nullptr)) g.steps = std::move(last);
    }

    g.showMsg = RegisterWindowMessageW(kShowMessageName);
    g.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    g.iconBig = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    g.iconSmall = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (!g.iconBig) g.iconBig = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));  // IDI_APPLICATION
    if (!g.iconSmall) g.iconSmall = g.iconBig;

    WNDCLASSEXW page{};
    page.cbSize = sizeof(page);
    page.lpfnWndProc = PageProc;
    page.hInstance = instance;
    page.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    page.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    page.lpszClassName = kPageClassName;
    RegisterClassExW(&page);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hIcon = g.iconBig;
    wc.hIconSm = g.iconSmall;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return 1;

    // 前回のウィンドウ位置が今の画面内にあれば復元する
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (g.cfg.windowX != CW_USEDEFAULT && g.cfg.windowY != CW_USEDEFAULT) {
        const POINT p{g.cfg.windowX + 40, g.cfg.windowY + 10};
        if (MonitorFromPoint(p, MONITOR_DEFAULTTONULL)) {
            x = g.cfg.windowX;
            y = g.cfg.windowY;
        }
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    HWND hwnd = CreateWindowExW(0, kClassName, kAppTitle, style, x, y, 500, 640, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (HandleShortcut(msg)) continue;
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g.font) DeleteObject(g.font);
    if (g.bigFont) DeleteObject(g.bigFont);
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return static_cast<int>(msg.wParam);
}
