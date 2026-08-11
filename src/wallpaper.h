#ifndef HSDWL_WALLPAPER_H
#define HSDWL_WALLPAPER_H

struct hsdwl_server;
struct wlr_output;
struct wlr_scene_buffer;

/* Load the built-in wallpaper from config (background_wallpaper) into
 * the bottom layer, if one is set.  Cover-fit layout per output is
 * applied by hsdwl_wallpaper_update. */
void hsdwl_wallpaper_load(struct hsdwl_server *server);
void hsdwl_wallpaper_update(struct hsdwl_server *server,
		struct wlr_output *output);
void hsdwl_wallpaper_destroy(struct hsdwl_server *server);

#endif
