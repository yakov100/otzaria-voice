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
set(COMMON_SOURCES
  ${SRC}/audio.cpp
  ${SRC}/engine.cpp
  ${SRC}/narrator.cpp
  ${SRC}/voicemodel.cpp
  ${SRC}/pack.cpp
  ${SRC}/platform.cpp
  ${SRC}/service.cpp
  ${SRC}/util.cpp)

if(WIN32 AND NOT OTZARIA_VOICE_MINIAUDIO)
  set(AUDIO_SOURCES ${SRC}/audio_win.cpp)
else()
  set(AUDIO_SOURCES ${SRC}/audio_miniaudio.cpp)
endif()

if(ANDROID)
  add_library(otzaria_voice SHARED
    ${COMMON_SOURCES} ${AUDIO_SOURCES} ${SRC}/pack_android.cpp ${SRC}/jni.cpp
    ${SRC}/speech_none.cpp)
  target_link_libraries(otzaria_voice PRIVATE log android)
  # Small and quick to load; nothing outside the JNI entry points is exported.
  set_target_properties(otzaria_voice PROPERTIES CXX_VISIBILITY_PRESET hidden)
  target_link_options(otzaria_voice PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
  return()
endif()

if(WIN32)
  add_executable(otzaria-voice WIN32
    ${COMMON_SOURCES} ${AUDIO_SOURCES}
    ${SRC}/main.cpp ${SRC}/pack_win.cpp ${SRC}/install_win.cpp ${SRC}/typing_win.cpp
    ${SRC}/speech_win.cpp)
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
    ${SRC}/main.cpp ${SRC}/pack_posix.cpp ${SRC}/install_posix.cpp ${SRC}/speech_none.cpp)
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
