#include "android.h"

#include "core/dir.h"
#include "core/file.h"
#include "platform/android/asset_handler.h"
#include "platform/android/jni.h"
#include "platform/file_manager.h"

#include "SDL.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ANDROID_BASELINE_DPI 160.0f
#define DEFAULT_SCREEN_DENSITY 1.0f

// Subdirectory of the app's internal files dir where AssetSelectionActivity copies the C3 data.
#define C3_DATA_SUBDIR "c3"
// Sentinel returned to platform_file_manager_create_directory when the directory already exists.
#define ANDROID_DIRECTORY_EXISTS (-1)

static int has_directory;
static char base_path[FILE_NAME_MAX];

// New data model (2026-07-12): the wallpaper reads C3 data from plain internal storage using
// standard POSIX file I/O against base_path, NOT via SAF/ContentResolver. Every relative path
// coming from the engine is resolved under base_path (the internal .../files/c3 directory).
static void build_internal_path(char *dest, const char *filename)
{
    if (filename[0] == '.' && (filename[1] == '/' || filename[1] == '\\')) {
        filename += 2;
    }
    if (filename[0] == '/') {
        snprintf(dest, FILE_NAME_MAX, "%s", filename);
    } else {
        snprintf(dest, FILE_NAME_MAX, "%s/%s", base_path, filename);
    }
    for (char *c = dest; *c; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
}

const char *android_get_c3_path(void)
{
    static char c3_path[FILE_NAME_MAX];
    const char *internal_storage = SDL_AndroidGetInternalStoragePath();
    if (!internal_storage) {
        return NULL;
    }
    snprintf(c3_path, FILE_NAME_MAX, "%s/%s", internal_storage, C3_DATA_SUBDIR);
    return c3_path;
}

int android_has_c3_path(void)
{
    return has_directory;
}

int android_show_c3_path_dialog(int again)
{
    // New data model: C3 data is copied to internal storage by AssetSelectionActivity, so there is
    // no runtime SAF folder-grant dialog. Report the data path as available on the first request;
    // on a retry (pre-init could not load C3 files there) stop, so pre_init doesn't spin forever.
    if (again) {
        return 0;
    }
    has_directory = 1;
    return 1;
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
    if (!*base_path || !filename) {
        return 0;
    }
    char full_path[FILE_NAME_MAX];
    build_internal_path(full_path, filename);

    int flags;
    if (strchr(mode, 'a')) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
    } else if (strchr(mode, 'w')) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else {
        flags = O_RDONLY;
    }
    int fd = open(full_path, flags, 0644);
    return fd < 0 ? 0 : fd;
}

void *android_open_asset(const char *asset, const char *mode)
{
    return asset_handler_open_asset(asset, mode);
}

int android_set_base_path(const char *path)
{
    if (!path || !*path) {
        return 0;
    }
    snprintf(base_path, FILE_NAME_MAX, "%s", path);
    return 1;
}

int android_get_directory_contents(const char *dir, int type, const char *extension, int (*callback)(const char *, long))
{
    if (strncmp(dir, ASSETS_DIRECTORY, strlen(ASSETS_DIRECTORY)) == 0) {
        return asset_handler_get_directory_contents(dir + strlen(ASSETS_DIRECTORY), type, extension, callback);
    }
    if (!*base_path) {
        return LIST_ERROR;
    }
    char full_dir[FILE_NAME_MAX];
    build_internal_path(full_dir, dir);
    DIR *d = opendir(full_dir);
    if (!d) {
        return LIST_ERROR;
    }
    int match = LIST_NO_MATCH;
    struct dirent *entry;
    struct stat file_info;
    char entry_path[FILE_NAME_MAX];
    while ((entry = readdir(d)) != 0) {
        const char *name = entry->d_name;
        snprintf(entry_path, FILE_NAME_MAX, "%s/%s", full_dir, name);
        if (stat(entry_path, &file_info) == -1) {
            continue;
        }
        int mode = file_info.st_mode;
        int is_regular_file = S_ISREG(mode) || S_ISLNK(mode);
        if ((!(type & TYPE_FILE) && is_regular_file) ||
            (!(type & TYPE_DIR) && S_ISDIR(mode)) ||
            S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode) || S_ISSOCK(mode)) {
            continue;
        }
        if (is_regular_file && !file_has_extension(name, extension)) {
            continue;
        }
        if ((type & TYPE_DIR) && name[0] == '.') {
            // Skip current (.), parent (..) and hidden directories (.*)
            continue;
        }
        match = callback(name, (long) file_info.st_mtime);
        if (match == LIST_MATCH) {
            break;
        }
    }
    closedir(d);
    return match;
}

int android_create_directory(const char *name)
{
    if (!*base_path) {
        return 0;
    }
    char full_path[FILE_NAME_MAX];
    build_internal_path(full_path, name);
    if (mkdir(full_path, 0755) == 0) {
        return 1;
    }
    return errno == EEXIST ? ANDROID_DIRECTORY_EXISTS : 0;
}

int android_remove_file(const char *filename)
{
    if (!*base_path) {
        return 0;
    }
    char full_path[FILE_NAME_MAX];
    build_internal_path(full_path, filename);
    return remove(full_path) == 0;
}

JNIEXPORT void JNICALL Java_com_github_Keriew_augustus_AssetSelectionActivity_gotDirectory(JNIEnv *env, jobject thiz)
{
    has_directory = 1;
}

// Values MUST match WALLPAPER_EVENT_* in SDLActivity.java (0/1/2/3); translated to the
// WALLPAPER_EVENT_HIDE/UPDATE_CONFIGS/RESIZE_DISPLAY/NEXT_POI codes (android.h) before being posted,
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
