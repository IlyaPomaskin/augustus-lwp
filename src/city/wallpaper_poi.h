#ifndef CITY_WALLPAPER_POI_H
#define CITY_WALLPAPER_POI_H

/**
 * @file
 * Point-of-interest camera for the live wallpaper. Scans the city for
 * interesting locations (landmarks, dense neighborhoods, industry clusters),
 * scores and samples them, and cycles the camera through them on each recenter.
 */

// Force a rescan of the POI list on the next advance (call when a city loads).
void wallpaper_poi_invalidate(void);

// Advance to the next POI and center the camera on it (instant jump). Rescans on
// first use and each time the list is fully cycled; falls back to a random tile
// when the city has no interesting locations.
void wallpaper_poi_next(void);

#endif // CITY_WALLPAPER_POI_H
