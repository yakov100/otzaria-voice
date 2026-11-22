; NSIS script for the Otzaria voice service: the Windows installer offered by the
; Otzaria plugin store (otzaria.org/plugins), with the dictation plugin inside.
;
; Compile (NSIS 3, Unicode):
;   makensis /DAppVersion=2.5.5 /DSourceExe=path\otzaria-voice.exe
;            /DSourceExeArm64=path\otzaria-voice-arm64.exe /DPluginFile=path\plugin.otzplugin
;            [/DPackZip=path\otzaria-voice-he-v5.zip]
;            /DOutFile=path\setup.exe otzaria-voice.nsi
;
; Why NSIS and not Inno Setup (otzaria-voice.iss): the store opens every
; installer with 7-Zip, without running it, to find the plugin inside and
; compare it with the plugin file submitted next to it. 7-Zip cannot open Inno
; Setup installers. It opens NSIS ones, and the store refuses an entry whose
; size 7-Zip cannot tell before extracting. Compressed files have none (each
; alone), and in one solid block the last file has none; stored files always
; do. So the files are stored. Without the voice pack the installer is about
; 2.6MB; with it about 337MB (the pack's zip is compressed already).
;
; What it does is what the Inno installer does, plus the plugin:
;   0. With /DPackZip (the store's installer: one download that also works
;      offline), the voice pack comes first, before any service runs:
;      `otzaria-voice.exe --install-pack <zip>` stops a running service, checks the
;      zip against the pack this service version installs (size, SHA-256) and
;      unpacks it to %APPDATA%\otzaria\stt\pack. A complete pack already there is
;      left as is, whatever its version: the service offers a newer one itself.
;      Without disk space, or on any failure, setup goes on without the pack, and
;      the service downloads it (data only) the first time dictation needs it.
;   1. The service installs itself: `otzaria-voice.exe --install --quiet` stops a
;      running older copy, copies itself (and on Windows on ARM the ARM64 build)
;      to %LOCALAPPDATA%\Programs\OtzariaVoice, registers login start (HKCU Run)
;      and its own "Installed apps" entry, and starts. That entry is the
;      uninstaller, so this setup writes no uninstaller of its own.
;   2. The plugin goes to a new folder under %TEMP%, and
;      `otzaria-voice.exe --deliver-plugin <file>` asks Otzaria to install it
;      (otzaria://plugin/install-local). Otzaria asks the user first, so the
;      file stays; uninstalling the service removes it.
;
; This file is UTF-8 with a BOM (NSIS reads the Hebrew text as UTF-8 then).

Unicode true
ManifestDPIAware true
RequestExecutionLevel user
SetCompressor lzma  ; the script header
SetCompress off     ; the files: see above

!ifndef AppVersion
  !error "Define AppVersion: /DAppVersion=X.Y.Z"
!endif
!ifndef SourceExe
  !error "Define SourceExe: /DSourceExe=path\otzaria-voice.exe"
!endif
!ifndef SourceExeArm64
  !error "Define SourceExeArm64: /DSourceExeArm64=path\otzaria-voice-arm64.exe"
!endif
!ifndef PluginFile
  !error "Define PluginFile: /DPluginFile=path\plugin.otzplugin"
!endif
!ifndef OutFile
  !define OutFile "otzaria-voice-windows-${AppVersion}.exe"
!endif
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"
!include "MUI2.nsh"

!define AppName "שירות הדיבור של אוצריא"

Name "${AppName}"
Caption "${AppName} ${AppVersion}"
OutFile "${OutFile}"
; Nothing is written here: the service copies itself (step 1 above).
InstallDir "$LOCALAPPDATA\Programs\OtzariaVoice"
ShowInstDetails nevershow
BrandingText "${AppName} ${AppVersion}"

VIProductVersion "${AppVersion}.0"
VIAddVersionKey /LANG=1037 "ProductName" "${AppName}"
VIAddVersionKey /LANG=1037 "ProductVersion" "${AppVersion}"
VIAddVersionKey /LANG=1037 "FileVersion" "${AppVersion}"
VIAddVersionKey /LANG=1037 "FileDescription" "${AppName}"
VIAddVersionKey /LANG=1037 "CompanyName" "yakov100"
VIAddVersionKey /LANG=1037 "LegalCopyright" "https://github.com/yakov100/otzaria-voice"

!define MUI_ABORTWARNING
!define MUI_LICENSEPAGE_BUTTON "$(^NextBtn)"
!define MUI_LICENSEPAGE_TEXT_BOTTOM "לחצו על הבא כדי להתקין."
!insertmacro MUI_PAGE_LICENSE "notices-he.txt"
!insertmacro MUI_PAGE_INSTFILES
Var PluginNote  ; set by the section: what happened to the plugin
Var PackNote    ; and to the voice pack
!define MUI_FINISHPAGE_TITLE "שירות הדיבור הותקן"
!define MUI_FINISHPAGE_TEXT "השירות פועל, ויעלה מעצמו בכל כניסה למחשב.$\r$\n$\r$\n$PackNote$PluginNote$\r$\n$\r$\nלהסרת השירות: הגדרות Windows ← אפליקציות מותקנות ← $\"${AppName}$\"."
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "Hebrew"

Function .onInit
  ; The service is built for x64 and ARM64; 32-bit Windows cannot run it.
  ${IfNot} ${IsNativeAMD64}
  ${AndIfNot} ${IsNativeARM64}
    MessageBox MB_OK|MB_ICONSTOP|MB_RIGHT|MB_RTLREADING "שירות הדיבור דורש Windows של 64 סיביות (x64 או ARM64)." /SD IDOK
    SetErrorLevel 2
    Quit
  ${EndIf}
FunctionEnd

!ifdef PackZip
; Step 0 (see the top): the voice pack, before any service runs. Never stops
; setup: without the pack the service downloads it when dictation needs it.
Function InstallPack
  ; The zip (about 320MB) goes to $PLUGINSDIR, the pack (about 400MB) beside
  ; Otzaria's data, and an older copy stays until the new one is in place.
  ${GetRoot} "$PLUGINSDIR" $R0
  ${GetRoot} "$APPDATA" $R1
  ${DriveSpace} "$R0\" "/D=F /S=M" $R2
  ${DriveSpace} "$R1\" "/D=F /S=M" $R3
  StrCpy $R6 "ok"
  ${If} $R0 == $R1
    ${If} $R2 < 820
      StrCpy $R6 "space"
    ${EndIf}
  ${ElseIf} $R2 < 360
  ${OrIf} $R3 < 460
    StrCpy $R6 "space"
  ${EndIf}
  ${If} $R6 == "space"
    StrCpy $PackNote "אין מספיק מקום בדיסק לחבילת הדיבור (צריך כ-820 מגה-בייט פנויים), ולכן היא לא הותקנה. השירות יציע להוריד אותה בשימוש הראשון.$\r$\n$\r$\n"
    MessageBox MB_OK|MB_ICONEXCLAMATION|MB_RIGHT|MB_RTLREADING "$PackNote" /SD IDOK
    Return
  ${EndIf}

  SetDetailsPrint textonly
  DetailPrint "מעתיק את חבילת הדיבור (כ-320 מגה-בייט)..."
  SetOutPath "$PLUGINSDIR"
  File "/oname=otzaria-voice-he.zip" "${PackZip}"
  DetailPrint "מתקין את חבילת הדיבור..."
  ClearErrors
  ExecWait '"$PLUGINSDIR\otzaria-voice.exe" --install-pack "$PLUGINSDIR\otzaria-voice-he.zip"' $R7
  ${If} ${Errors}
    StrCpy $R7 "?"  ; the program did not start
  ${EndIf}
  Delete "$PLUGINSDIR\otzaria-voice-he.zip"
  ${If} $R7 == 0
    ; The zip carries MODELS-LICENSE.md, which notices-he.txt says is in the pack.
    StrCpy $PackNote "חבילת הדיבור מותקנת: ההכתבה עובדת גם בלי אינטרנט.$\r$\n$\r$\n"
  ${Else}
    StrCpy $PackNote "חבילת הדיבור לא הותקנה (קוד $R7). השירות יציע להוריד אותה בשימוש הראשון.$\r$\n$\r$\n"
    MessageBox MB_OK|MB_ICONEXCLAMATION|MB_RIGHT|MB_RTLREADING "$PackNote" /SD IDOK
  ${EndIf}
  SetDetailsPrint none
FunctionEnd
!endif

Section "Install"
  SetDetailsPrint textonly
  DetailPrint "מתקין את שירות הדיבור..."
  SetDetailsPrint none

  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File "/oname=otzaria-voice.exe" "${SourceExe}"
  ${If} ${IsNativeARM64}
    File "/oname=otzaria-voice-arm64.exe" "${SourceExeArm64}"
  ${EndIf}

  StrCpy $PackNote ""
!ifdef PackZip
  Call InstallPack
!endif

  SetDetailsPrint textonly
  DetailPrint "מתקין את שירות הדיבור..."
  SetDetailsPrint none
  ClearErrors
  ExecWait '"$PLUGINSDIR\otzaria-voice.exe" --install --quiet' $0
  ${If} ${Errors}
  ${OrIf} $0 <> 0
    SetDetailsPrint both
    DetailPrint "otzaria-voice.exe --install --quiet: $0"
    MessageBox MB_OK|MB_ICONSTOP|MB_RIGHT|MB_RTLREADING "התקנת שירות הדיבור נכשלה (קוד $0). נסו לסגור את שירות הדיבור ולהריץ את ההתקנה שוב." /SD IDOK
    SetErrorLevel 1
    Abort
  ${EndIf}

  ; A folder of its own for the plugin, which Otzaria reads after the user agrees.
  GetTempFileName $1
  Delete "$1"
  CreateDirectory "$1"
  SetOutPath "$1"
  File "/oname=plugin.otzplugin" "${PluginFile}"
  SetOutPath "$PLUGINSDIR"

  StrCpy $PluginNote "אוצריא תבקש לאשר את התקנת התוסף $\"חיפוש בדיבור$\"."
  ClearErrors
  ExecWait '"$PLUGINSDIR\otzaria-voice.exe" --deliver-plugin "$1\plugin.otzplugin"' $2
  ${If} ${Errors}
    StrCpy $2 "?"  ; the program did not start
  ${EndIf}
  ${If} $2 == 3
    StrCpy $PluginNote "אוצריא לא נמצאה במחשב, ולכן התוסף $\"חיפוש בדיבור$\" לא הותקן. התקינו את אוצריא (otzaria.org), ואז את התוסף מחנות התוספים. השירות כבר מוכן."
    MessageBox MB_OK|MB_ICONINFORMATION|MB_RIGHT|MB_RTLREADING "שירות הדיבור הותקן, אבל $PluginNote" /SD IDOK
  ${ElseIf} $2 != 0
    StrCpy $PluginNote "לא הצלחנו להעביר את התוסף $\"חיפוש בדיבור$\" לאוצריא (קוד $2). אפשר להתקין אותו מחנות התוספים."
    MessageBox MB_OK|MB_ICONEXCLAMATION|MB_RIGHT|MB_RTLREADING "שירות הדיבור הותקן, אבל $PluginNote" /SD IDOK
  ${EndIf}
SectionEnd
