// Android: the same service, run by the app's foreground service through JNI
// (service/android, class org.otzaria.voice.NativeBridge). Java owns what only
// Java can do on Android: downloading over HTTPS and unpacking the zip.
#include <jni.h>

#include <stdexcept>
#include <string>

#include "pack.h"
#include "platform.h"
#include "service.h"

namespace {

JavaVM* vm = nullptr;
jclass bridge = nullptr;  // global ref: native threads cannot look classes up

std::string str(JNIEnv* env, jstring s) {
  if (!s) return "";
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out = c ? c : "";
  if (c) env->ReleaseStringUTFChars(s, c);
  return out;
}

// The calling native thread, attached to the VM for the duration of a call.
struct Attached {
  JNIEnv* env = nullptr;
  bool detach = false;
  Attached() {
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
#ifndef JNI_DESKTOP_HEADERS  // the NDK's jni.h takes JNIEnv**, the JDK's (for checks) void**
      vm->AttachCurrentThread(&env, nullptr);
#else
      vm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr);
#endif
      detach = true;
    }
  }
  ~Attached() {
    if (detach) vm->DetachCurrentThread();
  }
};

// A pending Java exception becomes a C++ one with the same (Hebrew) message.
void rethrow(JNIEnv* env) {
  jthrowable ex = env->ExceptionOccurred();
  if (!ex) return;
  env->ExceptionClear();
  jclass cls = env->GetObjectClass(ex);
  jmethodID getMessage = env->GetMethodID(cls, "getMessage", "()Ljava/lang/String;");
  auto msg = static_cast<jstring>(env->CallObjectMethod(ex, getMessage));
  std::string text = str(env, msg);
  env->ExceptionClear();
  throw std::runtime_error(text.empty() ? "ההורדה נכשלה" : text);
}

void javaDownload(const std::string& url, const std::string& part, uint64_t expected,
                  const pack::sys::OnProgress& progress) {
  Attached a;
  JNIEnv* env = a.env;
  jmethodID m = env->GetStaticMethodID(bridge, "download", "(Ljava/lang/String;Ljava/lang/String;JJ)V");
  jstring jUrl = env->NewStringUTF(url.c_str());
  jstring jPart = env->NewStringUTF(part.c_str());
  // Java reports progress through nativeProgress(handle, done).
  auto handle = reinterpret_cast<jlong>(&progress);
  env->CallStaticVoidMethod(bridge, m, jUrl, jPart, jlong(expected), handle);
  env->DeleteLocalRef(jUrl);
  env->DeleteLocalRef(jPart);
  rethrow(env);
}

void javaExtract(const std::string& zip, const std::string& to) {
  Attached a;
  JNIEnv* env = a.env;
  jmethodID m = env->GetStaticMethodID(bridge, "extract", "(Ljava/lang/String;Ljava/lang/String;)V");
  jstring jZip = env->NewStringUTF(zip.c_str());
  jstring jTo = env->NewStringUTF(to.c_str());
  env->CallStaticVoidMethod(bridge, m, jZip, jTo);
  env->DeleteLocalRef(jZip);
  env->DeleteLocalRef(jTo);
  rethrow(env);
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* javaVm, void*) {
  vm = javaVm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass local = env->FindClass("org/otzaria/voice/NativeBridge");
  if (!local) return JNI_ERR;
  bridge = static_cast<jclass>(env->NewGlobalRef(local));
  env->DeleteLocalRef(local);
  return JNI_VERSION_1_6;
}

// Blocks until quit; run it on its own thread.
JNIEXPORT jint JNICALL Java_org_otzaria_voice_NativeBridge_serve(JNIEnv* env, jclass,
                                                                 jstring filesDir,
                                                                 jstring nativeLibDir) {
  platform::setAndroidDirs(platform::fromUtf8(str(env, filesDir)),
                           platform::fromUtf8(str(env, nativeLibDir)));
  pack::AndroidHooks hooks;
  hooks.download = javaDownload;
  hooks.extract = javaExtract;
  pack::setAndroidHooks(std::move(hooks));
  return service::serve();
}

JNIEXPORT void JNICALL Java_org_otzaria_voice_NativeBridge_quit(JNIEnv*, jclass) {
  service::requestQuit();
}

JNIEXPORT void JNICALL Java_org_otzaria_voice_NativeBridge_setMicrophoneAllowed(JNIEnv*, jclass,
                                                                                jboolean allowed) {
  service::microphoneAllowed = allowed == JNI_TRUE;
}

JNIEXPORT void JNICALL Java_org_otzaria_voice_NativeBridge_nativeProgress(JNIEnv*, jclass,
                                                                          jlong handle, jlong done) {
  const auto* progress = reinterpret_cast<const pack::sys::OnProgress*>(handle);
  if (progress && *progress) (*progress)(uint64_t(done));
}

}  // extern "C"
