#ifndef HSDWL_VIEW_H
#define HSDWL_VIEW_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct hsdwl_server;
struct wlr_xdg_surface;
struct wlr_xwayland_surface;
struct wlr_scene_tree;

struct hsdwl_view
{
	struct wl_list link;
	struct hsdwl_server *server;
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_xwayland_surface *xwayland_surface;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener request_configure;
	struct wl_listener set_geometry;
};

void view_handle_new_xdg_toplevel(struct wl_listener *listener, void *data);
void view_focus(struct hsdwl_server *server, struct hsdwl_view *view);
struct hsdwl_view *view_next(struct hsdwl_server *server,
		struct hsdwl_view *current);
struct hsdwl_view *view_prev(struct hsdwl_server *server,
		struct hsdwl_view *current);
struct wlr_surface *view_get_surface(struct hsdwl_view *view);

#endif
