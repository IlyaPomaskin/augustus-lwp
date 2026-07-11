#include "android.h"

#include "core/dir.h"
#include "core/file.h"
#include "platform/android/asset_handler.h"
#include "platform/android/jni.h"
#include "platform/file_manager.h"

#include "SDL.h"

#include <string.h>

#define ANDROID_BASELINE_DPI 160.0f
#define DEFAULT_SCREEN_DENSITY 1.0f

static int has_directory;
static char path[FILE_NAME_MAX];

const char *android_get_c3_path(void)
{
    jni_function_handler handler;
    if (!jni_get_static_method_handler(CLASS_FILE_MANAGER, "getC3Path", "()Ljava/lang/String;", &handler)) {
        jni_destroy_function_handler(&handler);
        return NULL;
    }

    jobject result = (*handler.env)->CallStaticObjectMethod(handler.env, handler.class, handler.method);
    const char *temp_path = (*handler.env)->GetStringUTFChars(handler.env, (jstring) result, NULL);
    snprintf(path, FILE_NAME_MAX, "%s", temp_path);
    (*handler.env)->ReleaseStringUTFChars(handler.env, (jstring) result, temp_path);
    (*handler.env)->DeleteLocalRef(handler.env, result);
    jni_destroy_function_handler(&handler);

    return *path ? path : NULL;
}

int android_has_c3_path(void)
{
    return has_directory;
}

int android_show_c3_path_dialog(int again)
{
    (void) again;
    // The SAF folder-grant dialog lived on the deleted AugustusMainActivity. Under the current
    // data model the wallpaper reads C3 data copied to internal storage by AssetSelectionActivity
    // (see Task 6); this legacy grant flow is retired, so this is a no-op stub.
    has_directory = 0;
    return 0;
}

float android_get_screen_density(void)
{
    float diagonal_dpi = 0.0f;
    if (SDL_GetDisplayDPI(0, &diagonal_dpi, NULL, NULL) != 0 || diagonal_dpi <= 0.0f) {
        return DEFAULT_SCREEN_DENSITY;
    }
    return diagonal_dpi / ANDROID_BASELINE_DPI;
}

int android_get_file_descriptor(const char *filename, const char *mode)
{
    int result = 0;
    jni_function_handler handler;
    if (!jni_get_static_method_handler(CLASS_FILE_MANAGER, "openFileDescriptor",
        "(L" CLASS_CONTEXT ";Ljava/lang/String;Ljava/lang/String;)I", &handler)) {
        jni_destroy_function_handler(&handler);
        return 0;
    }
    jstring jfilename = (*handler.env)->NewStringUTF(handler.env, filename);
    jstring jmode = (*handler.env)->NewStringUTF(handler.env, mode);
    result = (int) (*handler.env)->CallStaticIntMethod(
        handler.env, handler.class, handler.method, handler.activity, jfilename, jmode);
    (*handler.env)->DeleteLocalRef(handler.env, jfilename);
    (*handler.env)->DeleteLocalRef(handler.env, jmode);
    jni_destroy_function_handler(&handler);

    return result;
}

void *android_open_asset(const char *asset, const char *mode)
{
    return asset_handler_open_asset(asset, mode);
}

int android_set_base_path(const char *path)
{
    int result = 0;
    jni_function_handler handler;
    if (!jni_get_static_method_handler(CLASS_FILE_MANAGER, "setBaseUri", "(Ljava/lang/String;)I", &handler)) {
        jni_destroy_function_handler(&handler);
        return 0;
    }
    jstring jpath = (*handler.env)->NewStringUTF(handler.env, path);
    result = (int) (*handler.env)->CallStaticIntMethod(handler.env, handler.class, handler.method, jpath);
    (*handler.env)->DeleteLocalRef(handler.env, jpath);
    jni_destroy_function_handler(&handler);

    return result;
}

int android_get_directory_contents(const char *dir, int type, const char *extension, int (*callback)(const char *, long))
{
    if (strncmp(dir, ASSETS_DIRECTORY, strlen(ASSETS_DIRECTORY)) == 0) {
        return asset_handler_get_directory_contents(dir + strlen(ASSETS_DIRECTORY), type, extension, callback);
    }
    jni_function_handler handler;
    jni_function_handler get_name;
    jni_function_handler get_last_modified_time;

    if (!jni_get_static_method_handler(CLASS_FILE_MANAGER, "getDirectoryFileList",
        "(L" CLASS_CONTEXT ";Ljava/lang/String;ILjava/lang/String;)[L" CLASS_FILE_MANAGER "$FileInfo;",
        &handler)) {
        jni_destroy_function_handler(&handler);
        return LIST_ERROR;
    }
    if (!jni_get_method_handler(CLASS_FILE_MANAGER "$FileInfo", "getName", "()Ljava/lang/String;", &get_name)) {
        jni_destroy_function_handler(&get_name);
        jni_destroy_function_handler(&handler);
        return LIST_ERROR;
    }
    if (!jni_get_method_handler(CLASS_FILE_MANAGER "$FileInfo", "getModifiedTime", "()J", &get_last_modified_time)) {
        jni_destroy_function_handler(&get_last_modified_time);
        jni_destroy_function_handler(&get_name);
        jni_destroy_function_handler(&handler);
        return LIST_ERROR;
    }

    jstring jdir = (*handler.env)->NewStringUTF(handler.env, dir);
    jstring jextension = (*handler.env)->NewStringUTF(handler.env, extension);
    jobjectArray result = (jobjectArray) (*handler.env)->CallStaticObjectMethod(
        handler.env, handler.class, handler.method, handler.activity, jdir, type, jextension);
    (*handler.env)->DeleteLocalRef(handler.env, jdir);
    (*handler.env)->DeleteLocalRef(handler.env, jextension);
    int match = LIST_NO_MATCH;
    int len = (*handler.env)->GetArrayLength(handler.env, result);
    for (int i = 0; i < len; ++i) {
        jobject jfile_info = (jobject) (*handler.env)->GetObjectArrayElement(handler.env, result, i);
        jstring jfilename = (jstring) (*handler.env)->CallObjectMethod(handler.env, jfile_info, get_name.method);
        const char *filename = (*handler.env)->GetStringUTFChars(handler.env, jfilename, NULL);
        long last_modified = (long) (*handler.env)->CallLongMethod(handler.env, jfile_info,
            get_last_modified_time.method);
        match = callback(filename, last_modified);
        (*handler.env)->ReleaseStringUTFChars(handler.env, (jstring) jfilename, filename);
        (*handler.env)->DeleteLocalRef(handler.env, jfilename);
        (*handler.env)->DeleteLocalRef(handler.env, jfile_info);
        if (match == LIST_MATCH) {
            break;
        }
    }
    (*handler.env)->DeleteLocalRef(handler.env, result);
    jni_destroy_function_handler(&get_last_modified_time);
    jni_destroy_function_handler(&get_name);
    jni_destroy_function_handler(&handler);
    return match;
}

int android_create_directory(const char *name)
{
    int result = 0;
    jni_function_handler handler;
    if (!jni_get_static_method_handler(CLASS_FILE_MANAGER, "createFolder",
        "(L" CLASS_CONTEXT ";Ljava/lang/String;)I", &handler)) {
        jni_destroy_function_handler(&handler);
        return 0;
    }
    jstring jname = (*handler.env)->NewStringUTF(handler.env, name);
    result = (int) (*handler.env)->CallStaticIntMethod(
        handler.env, handler.class, handler.method, handler.activity, jname);
    (*handler.env)->DeleteLocalRef(handler.env, jname);
    jni_destroy_function_handler(&handler);

    return result;
}

int android_remove_file(const char *filename)
{
    int result = 0;
    jni_function_handler handler;
    if (!jni_get_static_method_handler(CLASS_FILE_MANAGER, "deleteFile",
        "(L" CLASS_CONTEXT ";Ljava/lang/String;)Z", &handler)) {
        jni_destroy_function_handler(&handler);
        return 0;
    }
    jstring jfilename = (*handler.env)->NewStringUTF(handler.env, filename);
    result = (int) (*handler.env)->CallStaticBooleanMethod(
        handler.env, handler.class, handler.method, handler.activity, jfilename);
    (*handler.env)->DeleteLocalRef(handler.env, jfilename);
    jni_destroy_function_handler(&handler);

    return result;
}

JNIEXPORT void JNICALL Java_com_github_Keriew_augustus_DirectorySelectionActivity_gotDirectory(JNIEnv *env, jobject thiz)
{
    has_directory = 1;
}

// Values MUST match WALLPAPER_EVENT_* in SDLActivity.java (0/1/2); translated to the
// WALLPAPER_EVENT_HIDE/UPDATE_CONFIGS/RESIZE_DISPLAY codes (android.h) before being posted,
// so they can't collide with the desktop USER_EVENT_* codes sharing the same SDL_USEREVENT type.
JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_pushWallpaperEvent(JNIEnv *env, jclass cls, jint code)
{
    (void) env; (void) cls;
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0) {
        return;
    }
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_USEREVENT;
    event.user.code = WALLPAPER_EVENT_CODE_BASE + (int) code;
    SDL_PushEvent(&event);
}
