# Otzaria voice service.
#
#   cmake -S service -B build
#   cmake --build build --config Release
#
# Produces build/otzaria-voice (build/Release/otzaria-voice.exe with Visual
# Studio). ONNX Runtime is not linked: it is loaded at run time (Otzaria's own
# copy, or one the installer puts next to the program). On Android this file is
# used by service/android (Gradle, externalNativeBuild) and builds the JNI
# library libotzaria_voice.so instead of a program.
cmake_minimum_required(VERSION 3.16)
project(otzaria_voice LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(OTZARIA_VOICE_MINIAUDIO
  "Record with miniaudio on Windows too (other systems always use it)" OFF)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# common/ is shared by both features; dictation/ and tts/ belong to one each.
# All three are on the include path, so #include "x.h" works across them.
set(SRC_COMMON ${SRC}/common)
set(SRC_DICTATION ${SRC}/dictation)
set(SRC_TTS ${SRC}/tts)
include_directories(${SRC_COMMON} ${SRC_DICTATION} ${SRC_TTS})
set(COMMON_SOURCES
  ${SRC_COMMON}/audio.cpp
  ${SRC_COMMON}/handoff.cpp
  ${SRC_DICTATION}/engine.cpp
  ${SRC_TTS}/light.cpp
  ${SRC_TTS}/nakdan.cpp
  ${SRC_TTS}/narrator.cpp
  ${SRC_TTS}/nikud_lexicon.cpp
  ${SRC_TTS}/voicemodel.cpp
  ${SRC_COMMON}/pack.cpp
  ${SRC_COMMON}/platform.cpp
  ${SRC_COMMON}/service.cpp
  ${SRC_COMMON}/util.cpp)

if(WIN32 AND NOT OTZARIA_VOICE_MINIAUDIO)
  set(AUDIO_SOURCES ${SRC_COMMON}/audio_win.cpp)
else()
  set(AUDIO_SOURCES ${SRC_COMMON}/audio_miniaudio.cpp)
  # miniaudio 0.11.22 (one 4MB header, public domain or MIT-0: third_party/miniaudio/LICENSE)
  # is not kept in the repository: the Otzaria plugin store reviews every source file and
  # takes none over 2MB. It is the upstream release, fetched at configure time and pinned by
  # its SHA-256; a copy already in third_party/miniaudio/ (for builds without network) is
  # used when its hash is the same.
  set(MINIAUDIO_SHA256 9019743287e443c55e5737a7297f38e5e358561701d6db2d905afb114390c410)
  set(MINIAUDIO_URL
    https://raw.githubusercontent.com/mackron/miniaudio/350784a9467a79d0fa65802132668e5afbcf3777/miniaudio.h)
  set(MINIAUDIO_LOCAL ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio/miniaudio.h)
  set(MINIAUDIO_DIR ${CMAKE_BINARY_DIR}/third_party/miniaudio)
  set(MINIAUDIO_HEADER ${MINIAUDIO_DIR}/miniaudio.h)
  if(EXISTS ${MINIAUDIO_HEADER})
    file(SHA256 ${MINIAUDIO_HEADER} MINIAUDIO_HAVE)
  endif()
  if(NOT MINIAUDIO_HAVE STREQUAL MINIAUDIO_SHA256 AND EXISTS ${MINIAUDIO_LOCAL})
    file(SHA256 ${MINIAUDIO_LOCAL} MINIAUDIO_LOCAL_HASH)
    if(MINIAUDIO_LOCAL_HASH STREQUAL MINIAUDIO_SHA256)
      file(COPY ${MINIAUDIO_LOCAL} DESTINATION ${MINIAUDIO_DIR})
      set(MINIAUDIO_HAVE ${MINIAUDIO_SHA256})
    endif()
  endif()
  if(NOT MINIAUDIO_HAVE STREQUAL MINIAUDIO_SHA256)
    file(DOWNLOAD ${MINIAUDIO_URL} ${MINIAUDIO_HEADER}
      EXPECTED_HASH SHA256=${MINIAUDIO_SHA256} TLS_VERIFY ON STATUS MINIAUDIO_STATUS)
    list(GET MINIAUDIO_STATUS 0 MINIAUDIO_CODE)
    if(NOT MINIAUDIO_CODE EQUAL 0)
      message(FATAL_ERROR "miniaudio: download failed (${MINIAUDIO_STATUS}). Without network, "
        "put the 0.11.22 miniaudio.h (SHA-256 ${MINIAUDIO_SHA256}) in third_party/miniaudio/.")
    endif()
  endif()
  include_directories(${MINIAUDIO_DIR})
endif()

if(ANDROID)
  add_library(otzaria_voice SHARED
    ${COMMON_SOURCES} ${AUDIO_SOURCES} ${SRC_COMMON}/pack_android.cpp ${SRC_COMMON}/jni.cpp
    ${SRC_TTS}/speech_none.cpp ${SRC_TTS}/live_none.cpp)
  target_link_libraries(otzaria_voice PRIVATE log android)
  # Small and quick to load; nothing outside the JNI entry points is exported.
  set_target_properties(otzaria_voice PROPERTIES CXX_VISIBILITY_PRESET hidden)
  # CI ships Gradle's debug build (assembleDebug), whose CMAKE_BUILD_TYPE=Debug
  # compiles at -O0; the later -O2 wins, as on the desktop.
  target_compile_options(otzaria_voice PRIVATE -O2)
  target_link_options(otzaria_voice PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
  return()
endif()

if(WIN32)
  add_executable(otzaria-voice WIN32
    ${COMMON_SOURCES} ${AUDIO_SOURCES}
    ${SRC_COMMON}/main.cpp ${SRC_COMMON}/pack_win.cpp ${SRC_COMMON}/install_win.cpp
    ${SRC_TTS}/speech_win.cpp ${SRC_TTS}/live_win.cpp)
  target_compile_definitions(otzaria-voice PRIVATE UNICODE _UNICODE NDEBUG)
  # C++/WinRT (speech_win.cpp) under C++17 includes <experimental/coroutine>,
  # which newer MSVC refuses; with C++20 it uses <coroutine>.
  target_compile_features(otzaria-voice PRIVATE cxx_std_20)
  target_link_libraries(otzaria-voice PRIVATE
    ws2_32 winmm advapi32 shell32 user32 gdi32 winhttp bcrypt psapi
    # Reading aloud to MP3 (speech_win.cpp): the system voice and the encoder.
    windowsapp mfplat mfreadwrite mfuuid)
  if(MSVC)
    # Static runtime: one file to copy, nothing else to install.
    set_property(TARGET otzaria-voice PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(otzaria-voice PRIVATE /utf-8 /W3 /EHsc /O2)
    target_link_options(otzaria-voice PRIVATE /OPT:REF /OPT:ICF)
  endif()
else()
  add_executable(otzaria-voice
    ${COMMON_SOURCES} ${AUDIO_SOURCES}
    ${SRC_COMMON}/main.cpp ${SRC_COMMON}/pack_posix.cpp ${SRC_COMMON}/install_posix.cpp ${SRC_TTS}/speech_none.cpp ${SRC_TTS}/live_none.cpp)
  find_package(Threads REQUIRED)
  target_link_libraries(otzaria-voice PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
  target_compile_options(otzaria-voice PRIVATE -O2 -Wall)
  if(APPLE)
    target_link_libraries(otzaria-voice PRIVATE
      "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
    # macOS asks for microphone permission only for programs that say why:
    # the reason is embedded in the executable as its Info.plist.
    target_link_options(otzaria-voice PRIVATE
      "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist")
    set_target_properties(otzaria-voice PROPERTIES INSTALL_RPATH "@executable_path")
  else()
    # libstdc++ and libgcc inside: runs on older distributions than the build machine.
    target_link_options(otzaria-voice PRIVATE -static-libstdc++ -static-libgcc)
    set_target_properties(otzaria-voice PROPERTIES INSTALL_RPATH "$ORIGIN")
  endif()
endif()

# Unit tests (ctest -C Release): not part of the program that ships.
enable_testing()
add_executable(otzaria-voice-tests tests/handoff_test.cpp ${SRC_COMMON}/handoff.cpp)
if(MSVC)
  set_property(TARGET otzaria-voice-tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  target_compile_options(otzaria-voice-tests PRIVATE /utf-8 /W3 /EHsc)
endif()
add_test(NAME handoff COMMAND otzaria-voice-tests)
if(WIN32)
  add_executable(otzaria-voice-pack-tests tests/pack_test.cpp
    ${SRC_COMMON}/pack_win.cpp ${SRC_COMMON}/platform.cpp ${SRC_COMMON}/util.cpp)
  target_compile_definitions(otzaria-voice-pack-tests PRIVATE UNICODE _UNICODE)
  target_compile_features(otzaria-voice-pack-tests PRIVATE cxx_std_20)
  target_link_libraries(otzaria-voice-pack-tests PRIVATE winhttp bcrypt advapi32 psapi)
  if(MSVC)
    set_property(TARGET otzaria-voice-pack-tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(otzaria-voice-pack-tests PRIVATE /utf-8 /W3 /EHsc)
  endif()
  add_test(NAME pack COMMAND otzaria-voice-pack-tests)
endif()
