#ifndef PLATFORM_ANDROID_JNI_H
#define PLATFORM_ANDROID_JNI_H

#ifdef __ANDROID__

#include <jni.h>

typedef struct {
    JNIEnv *env;
    jclass class;
    jobject activity;
    jmethodID method;
} jni_function_handler;

// JNI type descriptor for the Context parameter FileManager's static methods take.
// jni_get_activity() resolves to the WallpaperService instance, which is itself a Context.
// (Previously a reference to the now-deleted AugustusMainActivity.)
#define CLASS_CONTEXT "android/content/Context"
#define CLASS_FILE_MANAGER "com/github/Keriew/augustus/FileManager"

JNIEnv *jni_get_env(void);
jobject jni_get_activity(void);
int jni_init_function_handler(const char *class_name, jni_function_handler *handler);
int jni_get_static_method_handler(
    const char *class_name, const char *method_name, const char *method_signature, jni_function_handler *handler);
int jni_get_method_handler(
    const char *class_name, const char *method_name, const char *method_signature, jni_function_handler *handler);
void jni_destroy_function_handler(jni_function_handler *handler);

#endif // __ANDROID__
#endif // PLATFORM_ANDROID_JNI_H
