#ifndef CITY_WALLPAPER_GUARDIAN_H
#define CITY_WALLPAPER_GUARDIAN_H

/**
 * Live-wallpaper "always positive" guardian. When the game runs in wallpaper
 * mode, clamps the soft city metrics (sentiment, health, treasury) once per
 * game-day so unrest, plague and bankruptcy can never develop. The caller is
 * responsible for invoking this only in wallpaper mode.
 */
void wallpaper_guardian_update(void);

#endif // CITY_WALLPAPER_GUARDIAN_H
