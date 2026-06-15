#define _GNU_SOURCE

#include "layer-shell.h"
#include "view.h"
#include "server.h"

#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

static void view_handle_map(struct wl_listener *listener, void *data)
{
	fprintf(stderr, "TRACE: view_handle_map\n");
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, map);
	if (!view->scene_tree)
	{
		int bw = view->server->config.border_width;
		view->scene_tree = wlr_scene_tree_create(
			view->server->workspaces[
				view->server->current_workspace]);
		if (!view->scene_tree)
			return;
		view->scene_tree->node.data = view;
		view->content_tree = wlr_scene_xdg_surface_create(
			view->scene_tree, view->xdg_surface);
		if (!view->content_tree)
			return;
		wlr_scene_node_set_position(
			&view->content_tree->node, bw, bw);
		view_borders_create(view);
		wlr_scene_node_set_enabled(
			&view->scene_tree->node, true);
	}

	if (view->xdg_surface->toplevel)
	{
		if (view->decoration && !view->decoration_request_mode.notify)
		{
			view->decoration_request_mode.notify =
				decoration_handle_request_mode;
			wl_signal_add(&view->decoration->events.request_mode,
				&view->decoration_request_mode);
			wlr_xdg_toplevel_decoration_v1_set_mode(
				view->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		}
	}

	view_focus(view->server, view);
}

struct wlr_surface *view_get_surface(struct hsdwl_view *view)
{
	return view->xdg_surface
		? view->xdg_surface->surface
		: view->xwayland_surface
			? view->xwayland_surface->surface
			: NULL;
}

void view_borders_create(struct hsdwl_view *view)
{
	fprintf(stderr, "TRACE: view_borders_create\n");
	struct hsdwl_config *cfg = &view->server->config;
	for (int i = 0; i < 4; i++)
	{
		view->border_rects[i] = wlr_scene_rect_create(
			view->scene_tree, 1, 1,
			cfg->border_color);
	}
	view_borders_update(view);
}

void view_borders_update(struct hsdwl_view *view)
{
	fprintf(stderr, "TRACE: view_borders_update\n");
	if (!view->scene_tree || !view->content_tree)
		return;
	int cw = 0, ch = 0;
	if (view->xdg_surface && view->xdg_surface->configured)
	{
		cw = view->xdg_surface->geometry.width;
		ch = view->xdg_surface->geometry.height;
	}
	else if (view->xwayland_surface)
	{
		cw = view->xwayland_surface->width;
		ch = view->xwayland_surface->height;
	}
	if (cw < 1 || ch < 1)
		return;
	int bw = view->server->config.border_width;
	if (bw < 1)
	{
		for (int i = 0; i < 4; i++)
			wlr_scene_node_set_enabled(
				&view->border_rects[i]->node, false);
		return;
	}
	wlr_scene_rect_set_size(
		view->border_rects[0], cw + bw * 2, bw);
	wlr_scene_node_set_position(
		&view->border_rects[0]->node, 0, 0);
	wlr_scene_rect_set_size(
		view->border_rects[1], cw + bw * 2, bw);
	wlr_scene_node_set_position(
		&view->border_rects[1]->node, 0, ch + bw);
	wlr_scene_rect_set_size(
		view->border_rects[2], bw, ch);
	wlr_scene_node_set_position(
		&view->border_rects[2]->node, 0, bw);
	wlr_scene_rect_set_size(
		view->border_rects[3], bw, ch);
	wlr_scene_node_set_position(
		&view->border_rects[3]->node, cw + bw, bw);
}

void view_focus(struct hsdwl_server *server, struct hsdwl_view *view)
{
	fprintf(stderr, "TRACE: view_focus view=%p\n", (void*)view);
	server->focused_layer = NULL;

	if (!view)
	{
		wlr_seat_keyboard_clear_focus(server->seat);
		struct hsdwl_view *v;
		wl_list_for_each(v, &server->views, link)
		{
			for (int i = 0; i < 4; i++)
			{
				if (v->border_rects[i])
					wlr_scene_rect_set_color(
						v->border_rects[i],
						server->config.border_color);
			}
		}
		return;
	}
	if (!view->scene_tree)
		return;

	struct hsdwl_view *v;
	wl_list_for_each(v, &server->views, link)
	{
		bool active = (v == view);
		if (v->xdg_surface && v->xdg_surface->configured)
		{
			wlr_xdg_toplevel_set_activated(
				v->xdg_surface->toplevel, active);
			wlr_xdg_surface_schedule_configure(v->xdg_surface);
		}
		if (v->xwayland_surface
				&& (!v->xwayland_surface->override_redirect
					|| wlr_xwayland_surface_override_redirect_wants_focus(
						v->xwayland_surface)))
		{
			wlr_xwayland_surface_activate(
				v->xwayland_surface, active);
		}
		float *color = active
			? server->config.border_color_focused
			: server->config.border_color;
		for (int i = 0; i < 4; i++)
		{
			if (v->border_rects[i])
				wlr_scene_rect_set_color(
					v->border_rects[i], color);
		}
	}

	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (kb)
	{
		struct wlr_surface *s = view_get_surface(view);
		if (s)
			wlr_seat_keyboard_notify_enter(server->seat,
				s, NULL, 0, NULL);
	}
}

static bool view_is_usable(struct hsdwl_view *v)
{
	if (!v->scene_tree)
		return false;
	if (v->xdg_surface && v->xdg_surface->configured)
		return true;
	if (v->xwayland_surface && v->xwayland_surface->surface
			&& (!v->xwayland_surface->override_redirect
				|| wlr_xwayland_surface_override_redirect_wants_focus(
					v->xwayland_surface)))
		return true;
	return false;
}

struct hsdwl_view *view_next(struct hsdwl_server *server,
		struct hsdwl_view *current)
{
	struct hsdwl_view *first = NULL;
	struct hsdwl_view *v;
	bool found = false;
	wl_list_for_each(v, &server->views, link)
	{
		if (!view_is_usable(v))
			continue;
		if (!first)
			first = v;
		if (found)
			return v;
		if (v == current)
			found = true;
	}
	return first;
}

struct hsdwl_view *view_prev(struct hsdwl_server *server,
		struct hsdwl_view *current)
{
	struct hsdwl_view *last = NULL;
	struct hsdwl_view *v;
	bool found = false;
	wl_list_for_each_reverse(v, &server->views, link)
	{
		if (!view_is_usable(v))
			continue;
		if (!last)
			last = v;
		if (found)
			return v;
		if (v == current)
			found = true;
	}
	return last;
}

static void view_handle_unmap(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, unmap);
	if (view->scene_tree)
		wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	if (view->xdg_surface && view->xdg_surface->toplevel)
	{
		wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, false);
		wlr_xdg_surface_schedule_configure(view->xdg_surface);
	}
	if (view->server->grabbed_view == view)
		view->server->grabbed_view = NULL;
	view_focus(view->server, view_next(view->server, view));
}

static void view_handle_commit(struct wl_listener *listener, void *data)
{
	fprintf(stderr, "TRACE: view_handle_commit\n");
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, commit);
	if (!view->xdg_surface)
		return;

	if (view->xdg_surface->initial_commit && view->xdg_surface->toplevel)
	{
		wlr_xdg_toplevel_set_activated(
			view->xdg_surface->toplevel, true);
		wlr_xdg_surface_schedule_configure(view->xdg_surface);
	}

	view_borders_update(view);
}

static void view_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, destroy);
	if (view->scene_tree)
		view->scene_tree->node.data = NULL;
	if (view->server->grabbed_view == view)
		view->server->grabbed_view = NULL;
	for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
	{
		if (view->server->focused_views[i] == view)
			view->server->focused_views[i] = NULL;
	}
	if (view->decoration)
	{
		wl_list_remove(&view->decoration_destroy.link);
		wl_list_remove(&view->decoration_request_mode.link);
	}
	wl_list_remove(&view->link);
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
	wl_list_insert(&server->views, &view->link);

	view->map.notify = view_handle_map;
	wl_signal_add(&xdg_surface->surface->events.map, &view->map);
	view->unmap.notify = view_handle_unmap;
	wl_signal_add(&xdg_surface->surface->events.unmap, &view->unmap);
	view->commit.notify = view_handle_commit;
	wl_signal_add(&xdg_surface->surface->events.commit, &view->commit);
	view->destroy.notify = view_handle_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);
}
