// Typing anywhere (Windows): a global key combination makes the service listen
// and type what is said into the focused field (Otzaria's search fields, Word,
// mail...), correcting the end of the sentence while it is spoken. The same keys
// end it. A small sign at the top of the screen shows that it is listening.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "pack.h"
#include "platform.h"
#include "service.h"
#include "util.h"

namespace service {

namespace {

constexpr int kHotkeyId = 1;
constexpr int kPauseId = 2;
std::atomic<bool> pauseToggle{false};  // set by the pause key, read by the typing thread
const wchar_t* kTitleBadge = L"Otzaria Voice";
constexpr UINT kMsgShow = WM_APP + 1;  // lParam: new std::wstring*
constexpr UINT kMsgHide = WM_APP + 2;
// The pause key (second line of hotkey.txt, default Ctrl+Alt+P).
UINT pauseMods = MOD_CONTROL | MOD_ALT, pauseVk = 'P';
std::string pauseName = "Ctrl+Alt+P";
std::wstring pauseLabel = L"Ctrl+Alt+P";
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

// A combination the user chose, one line in %APPDATA%\otzaria\stt\hotkey.txt (an
// optional second line chooses the pause key, default Ctrl+Alt+P)
// (for example "Ctrl+Shift+Space": Ctrl, Alt, Shift, Win, plus Space, a letter,
// a digit or F1..F24), is tried before the built-in ones. When it is taken by
// another program, the built-in list is used and the log says so.
std::string userName;
std::wstring userLabel;
Hotkey userHotkey{0, 0, "", L""};

bool parseCombo(const std::string& text, UINT& outMods, UINT& outVk, std::string& outName) {
  UINT mods = 0, vk = 0;
  std::string name;
  size_t start = 0;
  for (;;) {
    size_t end = text.find('+', start);
    std::string tok = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    while (!tok.empty() && isspace((unsigned char)tok.back())) tok.pop_back();
    while (!tok.empty() && isspace((unsigned char)tok.front())) tok.erase(0, 1);
    std::string low;
    for (char c : tok) low += (char)tolower((unsigned char)c);
    if (low == "ctrl" || low == "control") mods |= MOD_CONTROL;
    else if (low == "alt") mods |= MOD_ALT;
    else if (low == "shift") mods |= MOD_SHIFT;
    else if (low == "win") mods |= MOD_WIN;
    else if (low == "space") vk = VK_SPACE;
    else if (low.size() == 1 && isalnum((unsigned char)low[0])) vk = (UINT)toupper((unsigned char)low[0]);
    else if (low.size() >= 2 && low[0] == 'f' && isdigit((unsigned char)low[1])) {
      int n = atoi(low.c_str() + 1);
      if (n < 1 || n > 24) return false;
      vk = VK_F1 + n - 1;
    } else if (!low.empty()) return false;
    if (!tok.empty()) {
      if (!name.empty()) name += '+';
      name += tok;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (!mods || !vk) return false;
  outMods = mods;
  outVk = vk;
  outName = name;
  return true;
}

bool parseHotkey(const std::string& text) {
  UINT mods, vk;
  std::string name;
  if (!parseCombo(text, mods, vk, name)) return false;
  userName = name;
  userLabel = std::wstring(name.begin(), name.end());
  userHotkey = Hotkey{mods, vk, userName.c_str(), userLabel.c_str()};
  return true;
}

bool readUserHotkey() {
  std::filesystem::path p =
      platform::fromUtf8(platform::env("APPDATA")) / L"otzaria" / L"stt" / L"hotkey.txt";
  std::ifstream f(p);
  std::string line;
  if (!f || !std::getline(f, line)) return false;
  if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line.erase(0, 3);  // UTF-8 BOM
  if (!line.empty() && line.back() == '\r') line.pop_back();
  const bool ok = parseHotkey(line);
  std::string second;  // optional: the pause key
  if (std::getline(f, second)) {
    if (!second.empty() && second.back() == '\r') second.pop_back();
    UINT m, v;
    std::string n;
    if (parseCombo(second, m, v, n)) {
      pauseMods = m;
      pauseVk = v;
      pauseName = n;
      pauseLabel = std::wstring(n.begin(), n.end());
    }
  }
  return ok;
}

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

// OTZARIA_VOICE_TYPING_LOG=<file> (tests only): the typing calls are written to that file
// instead of being sent as keys, and the foreground-window checks are skipped.
const std::string& typingLog() {
  static const std::string path = platform::env("OTZARIA_VOICE_TYPING_LOG");
  return path;
}

// Deletes [remove] characters before the cursor and types [text].
void typeEdit(size_t remove, const std::wstring& text) {
  if (!typingLog().empty()) {
    std::ofstream log(typingLog(), std::ios::app | std::ios::binary);
    log << "BS " << remove << " | " << util::narrow(text) << "\n";
    return;
  }
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

// Text said while paused that cannot be typed because the session ended: it goes to the
// clipboard, not into whatever window happens to be in front.
void copyToClipboard(const std::wstring& text) {
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (void* mem = GlobalLock(g)) {
      memcpy(mem, text.c_str(), bytes);
      GlobalUnlock(g);
      if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    } else {
      GlobalFree(g);
    }
  }
  CloseClipboard();
}

void typingSession() {
  if (active) return;  // a plugin is dictating
  std::lock_guard<std::mutex> session(sessionMu);
  active = true;
  typingActive = true;
  stopRequested = false;
  cancelRequested = false;
  pauseToggle = false;
  struct Done {
    ~Done() {
      active = false;
      typingActive = false;
      touchEngine();
    }
  } done;

  HWND target = GetForegroundWindow();
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
                                 L" לסיום · " + pauseLabel + L" להשהיה";  // מקשיב — דברו. <צירוף> לסיום
  showBadge(L"טוען… אפשר כבר לדבר");  // טוען… אפשר כבר לדבר

  std::map<int, std::string> segments;
  std::wstring typed;
  bool moved = false;
  // Pause: speech keeps being recognized but nothing is typed, so the user can go to
  // another page, read from it aloud and come back. [base] is the last segment already
  // typed; what is said while paused is typed in one go on resume (at the cursor of the
  // window the user comes back to). A sentence in progress is finished first.
  bool paused = false, pausePending = false, typedBefore = false, open = false;
  int base = 0, lastSegment = 0;
  const std::wstring pausedMsg =
      L"מושהה — הטקסט נשמר. חזרו למסמך ו-" + pauseLabel + L" להמשך";
  auto wanted = [&] {
    std::string all;
    for (auto& [k, t] : segments)
      if (k > base && !t.empty()) all += (all.empty() ? "" : " ") + t;
    return util::widen(all);
  };
  auto enterPause = [&] {
    paused = true;
    pausePending = false;
    base = lastSegment;
    if (!typed.empty()) typedBefore = true;
    typed.clear();
    showBadge(pausedMsg);
  };
  auto resume = [&] {
    paused = false;
    target = GetForegroundWindow();
    const std::wstring want = wanted();
    if (!want.empty()) {
      typeEdit(0, typedBefore ? L" " + want : want);
      typed = want;
      typedBefore = true;
    }
    showBadge(listening);
  };
  auto sink = [&](const std::vector<voice::LiveUpdate>& ups) {
    if (ups.empty() || moved) return;
    for (auto& u : ups) {
      segments[u.segment] = u.text;
      lastSegment = std::max(lastSegment, u.segment);
      open = !u.isFinal;
    }
    if (paused) return;
    const std::wstring want = wanted();
    size_t common = 0;
    while (common < want.size() && common < typed.size() && want[common] == typed[common]) common++;
    if (!(common == want.size() && common == typed.size())) {
      // Text goes only where dictation began: another window must not receive
      // backspaces meant for this one.
      if (typingLog().empty() && GetForegroundWindow() != target) {
        moved = true;
        return;
      }
      typeEdit(typed.size() - common, want.substr(common));
      typed = want;
    }
    if (pausePending && !open) enterPause();
  };
  auto alive = [&] {
    if (pauseToggle.exchange(false)) {
      if (paused) {
        resume();
      } else if (pausePending) {
        pausePending = false;
        showBadge(listening);
      } else if (open) {
        pausePending = true;
        showBadge(L"משלים את המשפט, ואז מושהה…");  // משלים את המשפט, ואז מושהה…
      } else {
        enterPause();
      }
    }
    return !moved;
  };
  std::string reason;
  try {
    reason = recognizeLive(mic, {}, {}, sink, alive, [&] { showBadge(listening); }, kMaxTyping);
  } catch (const std::exception& ex) {
    platform::log(std::string("typing error: ") + ex.what());
    reason = "error";
  }
  mic.close();
  if (paused && !wanted().empty()) {
    copyToClipboard(wanted());
    showBadge(L"הטקסט שנאמר בהשהיה הועתק ללוח — Ctrl+V להדבקה");  // ... הועתק ללוח — Ctrl+V להדבקה
    Sleep(5000);
  } else if (moved) {
    showBadge(L"ההקלטה נעצרה: עברתם לחלון אחר");  // ההקלטה נעצרה: עברתם לחלון אחר
    Sleep(2500);
  } else if (!typed.empty() && (!typingLog().empty() || GetForegroundWindow() == target)) {
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
      if (w == kPauseId && typingActive) pauseToggle = true;
      return 0;
    case kMsgShow: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(l));
      badgeText = *text;
      // Top centre of the screen the user is working on.
      HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
      MONITORINFO mi{sizeof mi};
      GetMonitorInfoW(mon, &mi);
      const UINT dpi = GetDpiForWindow(badge) ? GetDpiForWindow(badge) : 96;
      const int width = MulDiv(560, dpi, 96), height = MulDiv(40, dpi, 96);
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
  if (readUserHotkey()) {
    if (RegisterHotKey(uiWindow, kHotkeyId, userHotkey.mods | MOD_NOREPEAT, userHotkey.vk)) {
      hotkey = &userHotkey;
      platform::log(std::string("hotkey ") + userHotkey.name + " (from hotkey.txt)");
    } else {
      platform::log(std::string("hotkey ") + userHotkey.name +
                    " from hotkey.txt is taken by another program");
    }
  }
  if (!hotkey.load()) {
    for (const auto& k : kHotkeys) {
      if (RegisterHotKey(uiWindow, kHotkeyId, k.mods | MOD_NOREPEAT, k.vk)) {
        hotkey = &k;
        platform::log(std::string("hotkey ") + k.name);
        break;
      }
      platform::log(std::string("hotkey ") + k.name + " is taken by another program");
    }
  }
  if (RegisterHotKey(uiWindow, kPauseId, pauseMods | MOD_NOREPEAT, pauseVk)) {
    platform::log("pause hotkey " + pauseName);
  } else {
    platform::log("pause hotkey " + pauseName + " is taken by another program");
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
