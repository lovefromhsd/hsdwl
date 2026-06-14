#ifndef HSDWL_LAYER_SHELL_H
#define HSDWL_LAYER_SHELL_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct hsdwl_server;
struct wlr_xdg_popup;
struct wlr_scene_tree;
struct wlr_layer_surface_v1;

struct hsdwl_layer_popup
{
	struct wl_list link;
	struct wlr_xdg_popup *wlr_popup;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener destroy;
};

struct hsdwl_layer_surface
{
	struct wl_list link;
	struct hsdwl_server *server;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_tree *scene_tree;
	struct wl_list popups;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener new_popup;
};

bool layer_shell_init(struct hsdwl_server *server);
void layer_shell_rearrange(struct hsdwl_server *server);

#endif
