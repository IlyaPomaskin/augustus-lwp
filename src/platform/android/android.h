#ifndef PLATFORM_ANDROID_H
#define PLATFORM_ANDROID_H

// SDL_USEREVENT codes bridging Android wallpaper lifecycle callbacks (SDLActivity.
// pushWallpaperEvent(), values 0/1/2/3) into the game's event loop (platform/SDL2/augustus.c).
// Offset above the desktop USER_EVENT_* range (0-4, see that file) so the two code spaces
// pushed through the same SDL_USEREVENT queue can never collide.
#define WALLPAPER_EVENT_CODE_BASE 100
#define WALLPAPER_EVENT_HIDE (WALLPAPER_EVENT_CODE_BASE + 0)
#define WALLPAPER_EVENT_UPDATE_CONFIGS (WALLPAPER_EVENT_CODE_BASE + 1)
#define WALLPAPER_EVENT_RESIZE_DISPLAY (WALLPAPER_EVENT_CODE_BASE + 2)
#define WALLPAPER_EVENT_NEXT_POI (WALLPAPER_EVENT_CODE_BASE + 3)

#ifdef __ANDROID__

#define PLATFORM_NO_USER_DIRECTORIES

int android_show_c3_path_dialog(int again);
int android_has_c3_path(void);
const char *android_get_c3_path(void);

float android_get_screen_density(void);
int android_get_file_descriptor(const char *filename, const char *mode);
int android_set_base_path(const char *path);
int android_get_directory_contents(const char *dir, int type, const char *extension, int (*callback)(const char *, long));
int android_create_directory(const char *name);
int android_remove_file(const char *filename);

void *android_open_asset(const char *asset, const char *mode);

#define PLATFORM_USE_VIRTUAL_KEYBOARD
void platform_show_virtual_keyboard(void);
void platform_hide_virtual_keyboard(void);

#endif // __ANDROID__
#endif // PLATFORM_ANDROID_H
