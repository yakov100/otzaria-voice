// Installed per user, without administrator rights: the program copies itself
// to %LOCALAPPDATA%\Programs\OtzariaVoice, starts at login (HKCU Run), and is
// listed in Windows' "Installed apps" with an uninstall entry.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "install.h"
#include "service.h"
#include "util.h"

namespace install {

namespace {

const wchar_t* kRunValue = L"OtzariaVoice";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OtzariaVoice";
const wchar_t* kTitle = L"שירות הדיבור של אוצריא";  // שירות הדיבור של אוצריא

std::wstring localAppData() { return platform::fromUtf8(platform::env("LOCALAPPDATA")).wstring(); }
std::wstring installDir() { return localAppData() + L"\\Programs\\OtzariaVoice"; }

bool sameFile(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

void message(const std::wstring& text, UINT icon) {
  MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | icon | MB_RTLREADING | MB_RIGHT | MB_SETFOREGROUND);
}

void setString(HKEY key, const wchar_t* name, const std::wstring& value) {
  RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                 DWORD((value.size() + 1) * sizeof(wchar_t)));
}

void setDword(HKEY key, const wchar_t* name, DWORD value) {
  RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
}

bool launchDetached(const std::wstring& exe) {
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  std::wstring line = L"\"" + exe + L"\"";
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                      nullptr, &si, &pi))
    return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

}  // namespace

platform::Path installedExe() { return platform::Path(installDir() + L"\\otzaria-voice.exe"); }

int install(bool quiet) {
  const std::wstring target = installedExe().wstring();
  const std::wstring self = platform::exePath().wstring();
  // A running copy (an older version, say) holds the file: ask it to quit first,
  // and wait until it is gone. Launched while the old copy still held its
  // one-instance mutex, the new copy saw "already running" and exited, which
  // left no service at all until the next login.
  service::postQuit();
  const std::wstring mutexName = L"Local\\OtzariaVoiceService-" + std::to_wstring(service::port());
  for (int i = 0; i < 40; i++) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, mutexName.c_str());
    if (!m) break;
    CloseHandle(m);
    Sleep(250);
  }
  CreateDirectoryW((localAppData() + L"\\Programs").c_str(), nullptr);
  CreateDirectoryW(installDir().c_str(), nullptr);
  if (!sameFile(self, target)) {
    bool copied = false;
    for (int i = 0; i < 20 && !copied; i++) {
      copied = CopyFileW(self.c_str(), target.c_str(), FALSE) != 0;
      if (!copied) Sleep(250);
    }
    if (!copied) {
      if (!quiet) message(L"ההתקנה נכשלה: לא ניתן להעתיק את הקובץ.", MB_ICONERROR);  // ההתקנה נכשלה: לא ניתן להעתיק את הקובץ.
      platform::log("install: copy failed");
      return 1;
    }
  }
  const std::wstring quoted = L"\"" + target + L"\"";
  HKEY h;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h,
                      nullptr) == ERROR_SUCCESS) {
    setString(h, kRunValue, quoted);
    RegCloseKey(h);
  }
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h,
                      nullptr) == ERROR_SUCCESS) {
    setString(h, L"DisplayName", kTitle);
    setString(h, L"DisplayVersion", util::widen(service::kVersion));
    setString(h, L"Publisher", L"Otzaria");
    setString(h, L"DisplayIcon", target);
    setString(h, L"InstallLocation", installDir());
    setString(h, L"UninstallString", quoted + L" --uninstall");
    setString(h, L"QuietUninstallString", quoted + L" --uninstall --quiet");
    setDword(h, L"NoModify", 1);
    setDword(h, L"NoRepair", 1);
    setDword(h, L"EstimatedSize", 1024);  // KB, without the downloaded voice pack
    RegCloseKey(h);
  }
  launchDetached(target);
  platform::log(std::string("installed ") + service::kVersion + " for login start");
  if (!quiet)
    message(L"שירות הדיבור הותקן ופועל.\n"
            L"מעכשיו הוא יעלה מעצמו בכל הפעלה של המחשב, והתוספים של אוצריא יוכלו להשתמש בו.\n"
            L"להסרה: הגדרות וינדוס ← אפליקציות מותקנות.",
            MB_ICONINFORMATION);
  // שירות הדיבור הותקן ופועל.
  // מעכשיו הוא יעלה מעצמו בכל הפעלה של המחשב, והתוספים של אוצריא יוכלו להשתמש בו.
  // להסרה: הגדרות וינדוס ← אפליקציות מותקנות.
  return 0;
}

int uninstall(bool quiet) {
  HKEY h;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
    RegDeleteValueW(h, kRunValue);
    RegCloseKey(h);
  }
  RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
  service::postQuit();
  platform::log("uninstalled");
  // A running program cannot delete its own file: a hidden shell does it a
  // moment after this process exits.
  const std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" +
                           installedExe().wstring() + L"\" & rmdir \"" + installDir() + L"\"";
  STARTUPINFOW si{sizeof si};
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::wstring line = cmd;
  if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                     nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  if (!quiet)
    message(L"שירות הדיבור הוסר.", MB_ICONINFORMATION);  // שירות הדיבור הוסר.
  return 0;
}

}  // namespace install
