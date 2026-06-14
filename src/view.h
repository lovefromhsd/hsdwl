#ifndef HSDWL_VIEW_H
#define HSDWL_VIEW_H

#include <wayland-server-core.h>

struct hsdwl_server;
struct wlr_xdg_surface;
struct wlr_scene_tree;

struct hsdwl_view
{
	struct hsdwl_server *server;
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
};

void view_handle_new_xdg_toplevel(struct wl_listener *listener, void *data);

#endif
