// Typing anywhere (Windows): a global key combination makes the service listen
// and type what is said into the focused field (Otzaria's search fields, Word,
// mail...), correcting the end of the sentence while it is spoken. The same keys
// end it. A small sign at the top of the screen shows that it is listening.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <map>
#include <memory>
#include <thread>

#include "pack.h"
#include "service.h"
#include "util.h"

namespace service {

namespace {

constexpr int kHotkeyId = 1;
const wchar_t* kTitleBadge = L"Otzaria Voice";
constexpr UINT kMsgShow = WM_APP + 1;  // lParam: new std::wstring*
constexpr UINT kMsgHide = WM_APP + 2;
constexpr auto kMaxTyping = std::chrono::minutes(10);
HWND uiWindow = nullptr;  // hidden; owns the hotkey
HWND badge = nullptr;     // the small "listening" sign
std::wstring badgeText;
std::atomic<bool> typingActive{false};

// The first free combination wins: another program may already own one.
struct Hotkey {
  UINT mods, vk;
  const char* name;      // for plugins (status)
  const wchar_t* label;  // for the badge
};
const Hotkey kHotkeys[] = {
    {MOD_CONTROL | MOD_ALT, VK_SPACE, "Ctrl+Alt+Space", L"Ctrl+Alt+רווח"},
    {MOD_WIN | MOD_ALT, VK_SPACE, "Win+Alt+Space", L"Win+Alt+רווח"},
    {MOD_CONTROL | MOD_ALT, 'D', "Ctrl+Alt+D", L"Ctrl+Alt+D"},
};
std::atomic<const Hotkey*> hotkey{nullptr};

void showBadge(const std::wstring& text) {
  if (uiWindow) PostMessageW(uiWindow, kMsgShow, 0, LPARAM(new std::wstring(text)));
}

void hideBadge() {
  if (uiWindow) PostMessageW(uiWindow, kMsgHide, 0, 0);
}

bool modifiersDown() {
  for (int vk : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN})
    if (GetAsyncKeyState(vk) & 0x8000) return true;
  return false;
}

// Typing while Ctrl/Alt are still held would turn letters into shortcuts.
void waitForModifiers() {
  for (int i = 0; i < 100 && modifiersDown(); i++) Sleep(20);
}

void sendKeys(std::vector<INPUT>& in) {
  if (!in.empty()) SendInput(UINT(in.size()), in.data(), sizeof(INPUT));
  in.clear();
}

// Deletes [remove] characters before the cursor and types [text].
void typeEdit(size_t remove, const std::wstring& text) {
  waitForModifiers();
  std::vector<INPUT> in;
  for (size_t i = 0; i < remove; i++) {
    INPUT down{INPUT_KEYBOARD}, up{INPUT_KEYBOARD};
    down.ki.wVk = up.ki.wVk = VK_BACK;
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    in.push_back(down);
    in.push_back(up);
  }
  for (wchar_t c : text) {
    INPUT down{INPUT_KEYBOARD}, up{INPUT_KEYBOARD};
    down.ki.wScan = up.ki.wScan = c;
    down.ki.dwFlags = KEYEVENTF_UNICODE;
    up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    in.push_back(down);
    in.push_back(up);
  }
  sendKeys(in);
}

void typingSession() {
  if (active) return;  // a plugin is dictating
  std::lock_guard<std::mutex> session(sessionMu);
  active = true;
  typingActive = true;
  stopRequested = false;
  cancelRequested = false;
  struct Done {
    ~Done() {
      active = false;
      typingActive = false;
      touchEngine();
    }
  } done;

  const HWND target = GetForegroundWindow();
  if (!pack::ready()) {
    showBadge(L"חבילת הדיבור לא מותקנת — התקינו אותה מהתוסף «חיפוש בדיבור»");  // חבילת הדיבור לא מותקנת — התקינו אותה מהתוסף «חיפוש בדיבור»
    Sleep(4000);
    hideBadge();
    return;
  }
  Microphone mic;
  std::string micError;
  if (!mic.open(micError)) {
    showBadge(L"אין גישה למיקרופון");  // אין גישה למיקרופון
    Sleep(3000);
    hideBadge();
    return;
  }
  const Hotkey* hk = hotkey.load();
  const std::wstring listening = L"מקשיב — דברו. " +
                                 std::wstring(hk ? hk->label : L"") +
                                 L" לסיום";  // מקשיב — דברו. <צירוף> לסיום
  showBadge(L"טוען… אפשר כבר לדבר");  // טוען… אפשר כבר לדבר

  std::map<int, std::string> segments;
  std::wstring typed;
  bool moved = false;
  auto sink = [&](const std::vector<voice::LiveUpdate>& ups) {
    if (ups.empty() || moved) return;
    for (auto& u : ups) segments[u.segment] = u.text;
    std::string all;
    for (auto& [k, t] : segments)
      if (!t.empty()) all += (all.empty() ? "" : " ") + t;
    const std::wstring want = util::widen(all);
    size_t common = 0;
    while (common < want.size() && common < typed.size() && want[common] == typed[common]) common++;
    if (common == want.size() && common == typed.size()) return;
    // Text goes only where dictation began: another window must not receive
    // backspaces meant for this one.
    if (GetForegroundWindow() != target) {
      moved = true;
      return;
    }
    typeEdit(typed.size() - common, want.substr(common));
    typed = want;
  };
  std::string reason;
  try {
    reason = recognizeLive(
        mic, {}, {}, sink, [&] { return !moved; }, [&] { showBadge(listening); }, kMaxTyping);
  } catch (const std::exception& ex) {
    platform::log(std::string("typing error: ") + ex.what());
    reason = "error";
  }
  mic.close();
  if (moved) {
    showBadge(L"ההקלטה נעצרה: עברתם לחלון אחר");  // ההקלטה נעצרה: עברתם לחלון אחר
    Sleep(2500);
  } else if (!typed.empty() && GetForegroundWindow() == target) {
    typeEdit(0, L" ");  // like typing: the next word does not stick to this one
  }
  hideBadge();
}

void toggleTyping() {
  if (typingActive) {
    stopRequested = true;
    showBadge(L"משלים את המשפט האחרון…");  // משלים את המשפט האחרון…
  } else if (!active) {
    std::thread(typingSession).detach();
  }
}

LRESULT CALLBACK uiProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
    case WM_HOTKEY:
      if (w == kHotkeyId) toggleTyping();
      return 0;
    case kMsgShow: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(l));
      badgeText = *text;
      // Top centre of the screen the user is working on.
      HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
      MONITORINFO mi{sizeof mi};
      GetMonitorInfoW(mon, &mi);
      const UINT dpi = GetDpiForWindow(badge) ? GetDpiForWindow(badge) : 96;
      const int width = MulDiv(440, dpi, 96), height = MulDiv(40, dpi, 96);
      const int x = (mi.rcWork.left + mi.rcWork.right - width) / 2;
      SetWindowPos(badge, HWND_TOPMOST, x, mi.rcWork.top + MulDiv(12, dpi, 96), width, height,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
      InvalidateRect(badge, nullptr, TRUE);
      return 0;
    }
    case kMsgHide:
      ShowWindow(badge, SW_HIDE);
      return 0;
  }
  return DefWindowProcW(h, msg, w, l);
}

LRESULT CALLBACK badgeProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT r;
    GetClientRect(h, &r);
    HBRUSH bg = CreateSolidBrush(RGB(32, 33, 36));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    const UINT dpi = GetDpiForWindow(h) ? GetDpiForWindow(h) : 96;
    HFONT font = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0,
                             0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    const std::wstring text = L"\U0001F399  " + badgeText;
    DrawTextW(dc, text.c_str(), int(text.size()), &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_RTLREADING | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    DeleteObject(font);
    EndPaint(h, &ps);
    return 0;
  }
  return DefWindowProcW(h, msg, w, l);
}

void uiLoop() {
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSW ui{};
  ui.lpfnWndProc = uiProc;
  ui.hInstance = inst;
  ui.lpszClassName = L"OtzariaVoiceUi";
  RegisterClassW(&ui);
  WNDCLASSW bc{};
  bc.lpfnWndProc = badgeProc;
  bc.hInstance = inst;
  bc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  bc.lpszClassName = L"OtzariaVoiceBadge";
  RegisterClassW(&bc);
  uiWindow = CreateWindowExW(0, ui.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst,
                             nullptr);
  // Never takes focus and lets clicks through: typing goes to the user's window.
  badge = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                              WS_EX_TRANSPARENT,
                          bc.lpszClassName, kTitleBadge, WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                          inst, nullptr);
  SetLayeredWindowAttributes(badge, 0, 235, LWA_ALPHA);
  for (const auto& k : kHotkeys) {
    if (RegisterHotKey(uiWindow, kHotkeyId, k.mods | MOD_NOREPEAT, k.vk)) {
      hotkey = &k;
      platform::log(std::string("hotkey ") + k.name);
      break;
    }
    platform::log(std::string("hotkey ") + k.name + " is taken by another program");
  }
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
}

}  // namespace

void startTyping() { std::thread(uiLoop).detach(); }

const char* typingHotkey() {
  const Hotkey* hk = hotkey.load();
  return hk ? hk->name : "";
}

}  // namespace service
