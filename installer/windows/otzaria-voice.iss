; Inno Setup script for the Otzaria voice service (Windows, per user, no admin).
;
; Compile (Inno Setup 6):
;   ISCC /DAppVersion=1.0.0 /DSourceExe=path\otzaria-voice.exe [/DPackDir=path\pack] /O<out-dir> otzaria-voice.iss
;
; Without /DPackDir the installer carries only the service; it downloads the
; voice pack the first time it is needed. With /DPackDir (a directory produced
; by installer/fetch-pack.sh: the five pack files + version.txt) the pack is
; unpacked to %APPDATA%\otzaria\stt\pack, the directory the service reads.
;
; The service installs itself: the installer hands the exe to
; `otzaria-voice.exe --install --quiet`, which stops a running older copy,
; copies itself to %LOCALAPPDATA%\Programs\OtzariaVoice, registers login start
; (HKCU Run) and its own "Installed apps" entry, and starts. That entry is the
; uninstaller, so this setup is not itself uninstallable (no second entry).
;
; This file is UTF-8 with a BOM (Inno Setup needs the BOM for Hebrew text).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceExe
  #error Define SourceExe: /DSourceExe=path\otzaria-voice.exe
#endif
#define PackVersion "3"
#ifdef PackDir
  #define Variant "with-models"
#else
  #define Variant "without-models"
#endif

[Setup]
AppId={{D42937D0-4038-4208-9917-763525F79884}
AppName=שירות הדיבור של אוצריא
AppVersion={#AppVersion}
AppVerName=שירות הדיבור של אוצריא {#AppVersion}
AppPublisher=yakov100
AppPublisherURL=https://github.com/yakov100/otzaria-voice
AppSupportURL=https://github.com/yakov100/otzaria-voice/issues
VersionInfoVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\OtzariaVoice
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
Uninstallable=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=otzaria-voice-windows-x64-{#Variant}-{#AppVersion}
#ifdef PackDir
; The model files are already compressed; spend no time recompressing them.
Compression=lzma2/fast
SolidCompression=no
#else
Compression=lzma2/max
SolidCompression=yes
#endif
WizardStyle=modern
InfoBeforeFile=notices-he.txt
ShowLanguageDialog=no
SetupLogging=yes

[Languages]
Name: "he"; MessagesFile: "compiler:Languages\Hebrew.isl"

[Files]
Source: "{#SourceExe}"; DestDir: "{tmp}"; DestName: "otzaria-voice.exe"; Flags: deleteafterinstall ignoreversion
#ifdef PackDir
Source: "{#PackDir}\*"; DestDir: "{userappdata}\otzaria\stt\pack"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: PackNeeded
#endif

[Code]
const
  PackFiles = 'ctc\model.int8.onnx|ctc\vocab.json|ctc\preprocessor.json|lm\library.otzlm|LICENSES.txt';

// True unless a pack of this version or newer is already complete on disk;
// then it is left untouched (its files may be open by the running service).
function PackNeeded: Boolean;
var
  Dir, Rest, F: String;
  V: AnsiString;
  P: Integer;
begin
  Result := True;
  Dir := ExpandConstant('{userappdata}\otzaria\stt\pack');
  if not LoadStringFromFile(Dir + '\version.txt', V) then Exit;
  if StrToIntDef(Trim(String(V)), 0) < StrToInt('{#PackVersion}') then Exit;
  Rest := PackFiles;
  while Rest <> '' do begin
    P := Pos('|', Rest);
    if P = 0 then begin F := Rest; Rest := ''; end
    else begin F := Copy(Rest, 1, P - 1); Rest := Copy(Rest, P + 1, Length(Rest)); end;
    if not FileExists(Dir + '\' + F) then Exit;
  end;
  Log('Voice pack already installed in ' + Dir + '; leaving it as is.');
  Result := False;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if CurStep <> ssPostInstall then Exit;
  WizardForm.StatusLabel.Caption := 'רושם את שירות הדיבור להפעלה עם כניסה למחשב...';
  if not Exec(ExpandConstant('{tmp}\otzaria-voice.exe'), '--install --quiet', '', SW_HIDE,
              ewWaitUntilTerminated, Code) or (Code <> 0) then
  begin
    Log('otzaria-voice.exe --install --quiet failed, code ' + IntToStr(Code));
    // An exception here ends setup with an error (and a non-zero exit code
    // in silent mode).
    RaiseException('התקנת שירות הדיבור נכשלה (קוד ' + IntToStr(Code) + '). ' +
                   'נסו לסגור את שירות הדיבור ולהריץ את ההתקנה שוב.');
  end;
  Log('otzaria-voice.exe --install --quiet succeeded');
end;
