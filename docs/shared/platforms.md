# Voice service per platform — build, command line, paths

For the installers (repo `otzaria-voice-ci`, `installer/`) and anyone packaging
the service. Status per platform is at the end.

## Build

```sh
cmake -S service -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # Linux, macOS
cmake -S service -B build -A x64                                # Windows (Visual Studio)
cmake --build build --config Release
```

Output: an executable named `otzaria-voice` (`otzaria-voice.exe`), in `build/`
(Ninja) or `build/Release/` (Visual Studio). `service\build.cmd` does the
Windows build and copies the result to `service\out\otzaria-voice.exe`.

- Requirements: C++17 with `<filesystem>` (GCC 9+, Clang 10+, MSVC 2019+),
  CMake 3.16+. Nothing to link beyond the system: ONNX Runtime is loaded at
  run time; miniaudio (vendored, `service/third_party/miniaudio`) loads
  PulseAudio/ALSA/JACK at run time on Linux, so no `-dev` audio packages are
  needed to build (installing them does no harm).
- macOS links CoreFoundation, CoreAudio, AudioToolbox and embeds
  `service/macos/Info.plist` (with `NSMicrophoneUsageDescription`) in the
  executable, so macOS can ask for the microphone. ORT 1.28 exists for arm64
  only: build with `-DCMAKE_OSX_ARCHITECTURES=arm64`.
- Linux links libstdc++/libgcc statically.
- `-DOTZARIA_VOICE_MINIAUDIO=ON` records with miniaudio on Windows too
  (default there: waveIn).
- Android: `service/android` is a Gradle project (module `app`) that builds the
  same CMakeLists.txt as `libotzaria_voice.so` through the NDK
  (`./gradlew assembleDebug` after generating a wrapper; none is committed).

## Command line

| Command | What it does |
|---|---|
| `otzaria-voice --serve` | Runs the service (HTTP on 127.0.0.1:47811). This is what login start runs. Exits at once (code 0 on Windows, 1 elsewhere) if another copy already listens on the port. |
| `otzaria-voice --install [--quiet]` | Installs for the current user (no admin rights), registers login start, starts it now. Stops a running copy first. |
| `otzaria-voice --uninstall [--quiet]` | Stops it, removes login start and the per-user program files. The voice pack stays. |
| `otzaria-voice --eval <manifest.jsonl> <out.jsonl>` | Accuracy evaluation on recordings (developers). |
| `otzaria-voice --version` | Prints the version (not visible on Windows: GUI program). |
| no arguments | Windows: installs itself unless it is the installed copy (double-click from a download), else serves. Elsewhere: serves. |

`--quiet`: no message box (Windows) / no stdout line (elsewhere). Exit code 0
on success.

Environment (all optional): `OTZARIA_VOICE_PORT` (default 47811),
`OTZARIA_VOICE_PACK_DIR`, `OTZARIA_ORT_PATH` (full path of the ONNX Runtime
library), `OTZARIA_VOICE_PACK_URL`, `OTZARIA_VOICE_TEST_WAV` (testing: plays a
16 kHz mono WAV instead of the microphone).

## Paths

| | Windows | Linux | macOS | Android |
|---|---|---|---|---|
| Per-user install by `--install` | `%LOCALAPPDATA%\Programs\OtzariaVoice\otzaria-voice.exe` | `~/.local/lib/otzaria-voice/otzaria-voice` (+ `libonnxruntime*` lying next to the source exe, symlinks kept) | `~/Library/Application Support/OtzariaVoice/otzaria-voice` (+ `libonnxruntime*`) | the APK |
| Installed for everyone (package) | — | exe under `/usr/` or `/opt/`: `--install` runs it in place and only adds login start | exe inside an `.app` bundle: same | — |
| Login start | `HKCU\...\Run\OtzariaVoice` = `"<exe>"`, plus an "Installed apps" entry (`HKCU\...\Uninstall\OtzariaVoice`) | `~/.config/autostart/otzaria-voice.desktop` (`Exec="<exe>" --serve`) | `~/Library/LaunchAgents/org.otzaria.voice.plist` (`<exe> --serve`, RunAtLoad, restarted only after a crash), loaded with `launchctl bootstrap gui/<uid>` | boot receiver starts the foreground service |
| ONNX Runtime looked up in | `OTZARIA_ORT_PATH`; `<exe dir>\onnxruntime\onnxruntime.dll`; Otzaria's install (registry "Uninstall" entries named Otzaria/אוצריא, `%LOCALAPPDATA%\Programs\Otzaria`, `%ProgramFiles%\Otzaria`) — always `<dir>\onnxruntime\onnxruntime.dll` | `OTZARIA_ORT_PATH`; next to the exe (`libonnxruntime.so`, `libonnxruntime.so.1`); `<exe>/lib`, `<exe>/onnxruntime`, `<exe>/../lib/otzaria-voice`; `/usr/lib/otzaria-voice`; Otzaria bundles (`/opt/otzaria`, `/usr/lib/otzaria`, `/usr/share/otzaria`, `/usr/local/lib/otzaria`, `/app/otzaria` + `/onnxruntime/`); finally the system loader | `OTZARIA_ORT_PATH`; next to the exe; `../Frameworks` (app bundle); `/Applications/Otzaria Voice.app/Contents/Frameworks`; `/Applications/Otzaria.app/Contents/Frameworks`; `~/Applications/Otzaria.app/...`; system loader | `libonnxruntime.so` from the APK (onnxruntime-android) |
| Voice pack read from (first complete) | `OTZARIA_VOICE_PACK_DIR`; `%APPDATA%\otzaria\stt\pack` | `OTZARIA_VOICE_PACK_DIR`; `~/.local/share/otzaria-voice/pack`; `~/.local/share/otzaria/stt/pack`; `/usr/share/otzaria-voice/pack` (read-only is fine) | `OTZARIA_VOICE_PACK_DIR`; `~/Library/Application Support/OtzariaVoice/pack`; `~/Library/Application Support/otzaria/stt/pack`; `<exe>/../Resources/pack`; `/Applications/Otzaria Voice.app/Contents/Resources/pack` | `<files>/stt/pack`; the with-models APK's `assets/pack` is copied there on first run and after each app update |
| Voice pack downloaded to | `OTZARIA_VOICE_PACK_DIR` if writable, else `%APPDATA%\otzaria\stt\pack` | same rule, else `~/.local/share/otzaria-voice/pack` (`$XDG_DATA_HOME` honoured) | same rule, else `~/Library/Application Support/OtzariaVoice/pack` | `<files>/stt/pack` |
| Log | `%LOCALAPPDATA%\OtzariaVoice\service.log` | `~/.local/state/otzaria-voice/service.log` (`$XDG_STATE_HOME`) | `~/Library/Logs/OtzariaVoice/service.log` | logcat tag `OtzariaVoice` and `<files>/service.log` |

A pack folder must contain `ctc/model.int8.onnx`, `ctc/vocab.json`,
`ctc/preprocessor.json`, `lm/library.otzlm`, `LICENSES.txt` (and optionally
`lm/phrases.txt`, `version.txt`). Download: the zip in
`service/src/pack.cpp` (`kRelease`), verified by SHA-256, unpacked by
`tar.exe` (Windows), `unzip` or `bsdtar` (Linux, macOS), `java.util.zip`
(Android). Linux and macOS download with the `curl` program — both must be
installed (they are on macOS and on common desktops).

## What differs per platform

- **Typing into any program with a global key combination** (Ctrl+Alt+Space,
  else Win+Alt+Space, else Ctrl+Alt+D) is Windows-only (`typing_win.cpp`);
  elsewhere `/status` reports `"hotkey": ""`. Linux would need per-desktop
  work (X11 vs Wayland portals), macOS Accessibility permission — not done.
- **Android microphone rule:** a service may record only when its foreground
  service was started while the app was on screen (Android 11+), and a
  microphone foreground service cannot start from boot (Android 14+). At boot
  the server runs without the microphone, `/status` reports
  `"microphone": false`, and `/dictate` answers 503 `microphone_background`
  until the user opens the app once. The plugin explains this.
- **macOS** asks for microphone permission the first time it records.
- Idle memory: the model loads on the first dictation and is released after
  3 idle minutes; on Windows the idle working set is ~210 KB.

## Status

| Platform | Written | Compiled | Tested |
|---|---|---|---|
| Windows x64 | yes | yes (MSVC 2022 via CMake and build.cmd) | yes: 110-recording eval identical to `out_ph_2_1.jsonl`; live NDJSON with a test recording; real microphone opens with waveIn and with miniaudio (~16 000 samples/s captured); idle working set ~210 KB; portable SHA-256 matches standard vectors and Windows' hash |
| Linux | yes | no (no Linux toolchain on the development machine) | no |
| macOS | yes | no | no |
| Android | yes | no (no SDK/NDK); `jni.cpp` and `pack_android.cpp` syntax-checked with MSVC + the JDK's `jni.h`, `NativeBridge.java` compiled with `javac` | no |
