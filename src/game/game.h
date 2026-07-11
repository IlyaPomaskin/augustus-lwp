#ifndef GAME_GAME_H
#define GAME_GAME_H

int game_pre_init(void);

int game_init(void);

/**
 * Initializes the game for live-wallpaper mode: loads the wallpaper save and
 * shows the wallpaper city window. Returns 1 on success, 0 on failure.
 */
int game_init_wallpaper(void);

/**
 * @return 1 if the game is running in live-wallpaper mode
 */
int game_wallpaper_mode(void);

void game_set_wallpaper_mode(int enabled);

int game_init_editor(void);

int game_reload_language(void);

void game_run(void);

void game_draw(void);

void game_display_fps(int fps);

void game_exit_editor(void);

void game_exit(void);

#endif // GAME_GAME_H
