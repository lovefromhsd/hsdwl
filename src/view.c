#define _GNU_SOURCE

#include "view.h"
#include "server.h"

#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

static void view_handle_map(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, map);
	if (!view->scene_tree)
	{
		view->scene_tree = wlr_scene_xdg_surface_create(
			&view->server->scene->tree, view->xdg_surface);
		if (view->scene_tree)
		{
			view->scene_tree->node.data = view;
			wlr_scene_node_set_enabled(&view->scene_tree->node, true);
		}
	}

	if (view->xdg_surface->toplevel)
	{
		wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, true);
		wlr_xdg_surface_schedule_configure(view->xdg_surface);
	}

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(view->server->seat);
	if (kb)
	{
		wlr_seat_keyboard_notify_enter(view->server->seat,
			view->xdg_surface->surface, NULL, 0, NULL);
	}
}

static void view_handle_unmap(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, unmap);
	if (view->scene_tree)
		wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	if (view->xdg_surface->toplevel)
	{
		wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, false);
		wlr_xdg_surface_schedule_configure(view->xdg_surface);
	}
	wlr_seat_keyboard_clear_focus(view->server->seat);
}

static void view_handle_commit(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, commit);
	if (!view->xdg_surface)
		return;

	if (view->xdg_surface->initial_commit && view->xdg_surface->toplevel)
	{
		wlr_xdg_toplevel_set_fullscreen(view->xdg_surface->toplevel, true);
		wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, true);
		wlr_xdg_surface_schedule_configure(view->xdg_surface);
	}
}

static void view_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, destroy);
	if (view->scene_tree)
		view->scene_tree->node.data = NULL;
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->destroy.link);
	free(view);
}

void view_handle_new_xdg_toplevel(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;
	struct wlr_xdg_surface *xdg_surface = toplevel->base;

	struct hsdwl_view *view = calloc(1, sizeof(*view));
	if (!view)
		return;

	view->server = server;
	view->xdg_surface = xdg_surface;
	xdg_surface->data = view;

	view->map.notify = view_handle_map;
	wl_signal_add(&xdg_surface->surface->events.map, &view->map);
	view->unmap.notify = view_handle_unmap;
	wl_signal_add(&xdg_surface->surface->events.unmap, &view->unmap);
	view->commit.notify = view_handle_commit;
	wl_signal_add(&xdg_surface->surface->events.commit, &view->commit);
	view->destroy.notify = view_handle_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);
}
