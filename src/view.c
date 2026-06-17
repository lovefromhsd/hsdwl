#define _GNU_SOURCE

#include "layer-shell.h"
#include "stage.h"
#include "tab-group.h"
#include "tab-group-layout.h"
#include "view.h"
#include "server.h"
#include "deco.h"

#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>

#include "animation.h"



static void view_handle_set_title(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, set_title);
	if (view->xdg_surface && view->xdg_surface->toplevel
			&& view->xdg_surface->toplevel->title)
	{
		strncpy(view->cached_title,
			view->xdg_surface->toplevel->title,
			sizeof(view->cached_title) - 1);
		view->cached_title[sizeof(view->cached_title) - 1] = '\0';
	}
	titlebar_text_update(view);
}

static void view_handle_map(struct wl_listener *listener, void *data)
{
	fprintf(stderr, "TRACE: view_handle_map\n");
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, map);
	if (!view->scene_tree)
	{
		int bw = view->server->config.border_width;
		int tb = view->server->config.titlebar_height;
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
			&view->content_tree->node, bw, tb > 0 ? tb : bw);
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

	titlebar_text_update(view);
	if (view->server->config.stage_manager_enabled)
		stage_manager_new_window(view->server, view);
	else
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
			if (!v->scene_tree || !v->scene_tree->node.enabled)
				continue;
			if (v->xwayland_surface
					&& v->xwayland_surface->override_redirect)
				continue;
			for (int i = 0; i < 4; i++)
			{
				if (v->border_rects[i])
					wlr_scene_rect_set_color(
						v->border_rects[i],
						server->config.border_color);
			}
			titlebar_text_update(v);
		}
		struct hsdwl_tab_group *tg;
		wl_list_for_each(tg, &server->tab_groups, link)
			hsdwl_tab_group_update_layout(tg);
		return;
	}
	if (!view->scene_tree)
		return;

	server->focused_views[server->current_workspace] = view;
	struct hsdwl_view *v;
	wl_list_for_each(v, &server->views, link)
	{
		if (!v->scene_tree || !v->scene_tree->node.enabled)
			continue;
		bool active = (v == view);
		if (v->xdg_surface && v->xdg_surface->configured
				&& v->xdg_surface->surface->buffer)
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
		if (v->xwayland_surface
				&& v->xwayland_surface->override_redirect)
			continue;
		float *color = active
			? server->config.border_color_focused
			: server->config.border_color;
		for (int i = 0; i < 4; i++)
		{
			if (v->border_rects[i])
				wlr_scene_rect_set_color(
					v->border_rects[i], color);
		}
		titlebar_text_update(v);
	}

	struct hsdwl_tab_group *tg;
	wl_list_for_each(tg, &server->tab_groups, link)
		hsdwl_tab_group_update_layout(tg);

	if (view->tab_group && view->tab_group->scene_tree)
		wlr_scene_node_raise_to_top(
			&view->tab_group->scene_tree->node);
	else
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
	if (!v->scene_tree || !v->scene_tree->node.enabled)
		return false;
	if (v->xdg_surface && v->xdg_surface->configured)
		return true;
	if (v->xwayland_surface && v->xwayland_surface->surface
			&& !v->xwayland_surface->override_redirect)
		return true;
	return false;
}

static bool scene_tree_is_descendant(struct wlr_scene_tree *tree,
		struct wlr_scene_tree *ancestor)
{
	if (!tree || !ancestor) return false;
	struct wlr_scene_tree *p = tree;
	while (p)
	{
		if (p == ancestor) return true;
		p = p->node.parent;
	}
	return false;
}

static bool view_on_workspace(struct hsdwl_view *v,
		struct wlr_scene_tree *ws)
{
	if (!v->scene_tree || !ws)
		return false;
	if (scene_tree_is_descendant(v->scene_tree->node.parent, ws))
		return true;
	if (v->tab_group && v->tab_group->scene_tree
			&& scene_tree_is_descendant(
				v->tab_group->scene_tree->node.parent, ws))
		return true;
	return false;
}

bool view_is_on_workspace(struct hsdwl_view *view, struct wlr_scene_tree *ws)
{
	return view_on_workspace(view, ws);
}



struct hsdwl_view *view_next(struct hsdwl_server *server,
		struct hsdwl_view *current)
{
	struct wlr_scene_tree *ws =
		server->workspaces[server->current_workspace];
	struct hsdwl_view *first = NULL;
	struct hsdwl_view *v;
	bool found = false;
	wl_list_for_each(v, &server->views, link)
	{
		if (!view_is_usable(v) || !view_on_workspace(v, ws))
			continue;
		if (v->tab_group && v != v->tab_group->active)
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
	struct wlr_scene_tree *ws =
		server->workspaces[server->current_workspace];
	struct hsdwl_view *last = NULL;
	struct hsdwl_view *v;
	bool found = false;
	wl_list_for_each_reverse(v, &server->views, link)
	{
		if (!view_is_usable(v) || !view_on_workspace(v, ws))
			continue;
		if (v->tab_group && v != v->tab_group->active)
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
	fprintf(stderr, "TRACE: view_handle_unmap\n");
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
	if (!view->server->config.stage_manager_enabled)
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

	if (view->tab_group && view->xdg_surface->configured)
	{
		struct hsdwl_tab_group *g = view->tab_group;
		int cw = view->xdg_surface->geometry.width;
		int ch = view->xdg_surface->geometry.height;
		if (cw != g->content_area_box.width
				|| ch != g->content_area_box.height)
		{
			g->content_area_box.width = cw;
			g->content_area_box.height = ch;
			struct hsdwl_view *vi;
			wl_list_for_each(vi, &g->views, tab_group_link)
			{
				if (vi != view)
					view_configure_size(vi, cw, ch);
			}
		}
		hsdwl_tab_group_update_layout(g);
	}

	view_borders_update(view);
	titlebar_text_update(view);

	if (view->server->config.stage_manager_enabled)
		stage_manager_notify_surface_commit(view->server, view);
}

static void view_handle_toplevel_destroy(struct wl_listener *listener, void *data)
{
	fprintf(stderr, "TRACE: view_handle_toplevel_destroy\n");
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, toplevel_destroy);
	wl_list_remove(&view->set_title.link);
	wl_list_init(&view->set_title.link);
	wl_list_remove(&view->toplevel_destroy.link);
}

static void view_handle_destroy(struct wl_listener *listener, void *data)
{
	fprintf(stderr, "TRACE: view_handle_destroy\n");
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, destroy);
	if (view->server->grabbed_view == view)
		view->server->grabbed_view = NULL;
	if (view->server->grab_target == view)
		view->server->grab_target = NULL;
	for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
	{
		if (view->server->focused_views[i] == view)
			view->server->focused_views[i] = NULL;
	}
	if (view->tab_group)
		hsdwl_tab_group_remove_view(view->tab_group, view);
	wl_list_remove(&view->tab_group_link);
	if (view->scene_tree)
	{
		view->scene_tree->node.data = NULL;
		wlr_scene_node_destroy(&view->scene_tree->node);
	}
	if (view->decoration_destroy.notify)
		wl_list_remove(&view->decoration_destroy.link);
	if (view->decoration_request_mode.notify)
		wl_list_remove(&view->decoration_request_mode.link);
	wl_list_remove(&view->link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->destroy.link);
	view->content_tree = NULL;
	view->scene_tree = NULL;
	if (view->server->config.stage_manager_enabled)
		stage_manager_notify_view_removed(view->server, view);

	
	{
		struct hsdwl_animation *anim, *tmp;
		wl_list_for_each_safe(anim, tmp,
			&view->server->animations, link)
		{
			if (anim->user_data == view)
			{
				wl_list_remove(&anim->link);
				free(anim);
			}
		}
	}
	if (view->anim_overlay)
	{
		wlr_scene_node_destroy(&view->anim_overlay->node);
		view->anim_overlay = NULL;
	}

	free(view);
}

void view_close(struct hsdwl_view *view)
{
	if (!view)
		return;
	if (view->xwayland_surface
			&& view->xwayland_surface->override_redirect)
		return;
	if (view->xdg_surface && view->xdg_surface->toplevel)
		wlr_xdg_toplevel_send_close(view->xdg_surface->toplevel);
	else if (view->xwayland_surface)
		wlr_xwayland_surface_close(view->xwayland_surface);
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
	view->tab_group = NULL;
	wl_list_init(&view->tab_group_link);
	view->maximized = false;
	view->zoomed = false;
	memset(&view->saved_geometry, 0, sizeof(view->saved_geometry));
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
	view->set_title.notify = view_handle_set_title;
	wl_signal_add(&xdg_surface->toplevel->events.set_title,
		&view->set_title);
	view->toplevel_destroy.notify = view_handle_toplevel_destroy;
	wl_signal_add(&xdg_surface->toplevel->events.destroy,
		&view->toplevel_destroy);
	if (xdg_surface->toplevel->title)
	{
		strncpy(view->cached_title, xdg_surface->toplevel->title,
			sizeof(view->cached_title) - 1);
		view->cached_title[sizeof(view->cached_title) - 1] = '\0';
	}
}
