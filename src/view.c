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

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include "output.h"

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
	if (view->server->config.stage_manager_enabled
			&& !view_is_floating_toolbar(view))
		stage_manager_new_window(view->server, view, true);
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

	if (v->xdg_surface && v->xdg_surface->toplevel
			&& v->xdg_surface->toplevel->parent)
		return false;
	if (v->xwayland_surface && v->xwayland_surface->parent)
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

bool view_is_stage_managed(struct hsdwl_view *view)
{
	if (!view || !view->server || !view->scene_tree)
		return false;
	struct hsdwl_server *server = view->server;
	for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
	{
		if (!server->ws_stage_canvases[i])
			continue;
		if (scene_tree_is_descendant(view->scene_tree->node.parent,
				server->ws_stage_canvases[i]))
			return true;
	}
	if (view->tab_group && view->tab_group->scene_tree)
	{
		for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
		{
			if (!server->ws_stage_canvases[i])
				continue;
			if (scene_tree_is_descendant(
					view->tab_group->scene_tree->node.parent,
					server->ws_stage_canvases[i]))
				return true;
		}
	}
	return false;
}

bool view_is_floating_toolbar(struct hsdwl_view *view)
{
	if (!view)
		return false;

	if (view->xdg_surface && view->xdg_surface->toplevel
			&& view->xdg_surface->toplevel->parent)
		return true;
	if (view->xwayland_surface && view->xwayland_surface->parent)
		return true;

	int max_size = view->server->config.stage_float_max_size;
	if (max_size > 0)
	{
		int w = 0, h = 0;
		if (view->xdg_surface && view->xdg_surface->configured)
		{
			w = view->xdg_surface->geometry.width;
			h = view->xdg_surface->geometry.height;
		}
		else if (view->xwayland_surface)
		{
			w = view->xwayland_surface->width;
			h = view->xwayland_surface->height;
		}
		if (w > 0 && h > 0 && w < max_size && h < max_size)
			return true;
	}
	return false;
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


static void view_popup_handle_commit(
	struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_popup *popup = wl_container_of(listener, popup, commit);
	struct wlr_xdg_popup *wlr_popup = popup->wlr_popup;

	/* On the xdg_surface's first commit, the surface is now
	 * initialized. Send the initial configure with unconstrained
	 * position so the popup maps with correct geometry. */
	if (wlr_popup->base->initial_commit) {
		wlr_xdg_popup_unconstrain_from_box(wlr_popup,
			&popup->output_box);
	}
}

static void view_popup_handle_destroy(
	struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->link);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	wlr_scene_node_destroy(&popup->scene_tree->node);
	free(popup);
}

struct wlr_scene_tree *view_popup_parent_tree(struct hsdwl_view *view)
{
	/* Popup geometry is relative to the parent xdg_surface's coordinate
	 * system. view->content_tree IS the xdg_surface scene tree (returned
	 * by wlr_scene_xdg_surface_create), so popups go there. */
	return view->content_tree;
}

static void view_handle_unmap(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, unmap);

	struct hsdwl_popup *popup, *tmp;
	wl_list_for_each_safe(popup, tmp, &view->popups, link)
	{
		wl_list_remove(&popup->link);
		wl_list_remove(&popup->commit.link);
		wl_list_remove(&popup->destroy.link);
		wlr_scene_node_destroy(&popup->scene_tree->node);
		free(popup);
	}

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
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, toplevel_destroy);
	wl_list_remove(&view->set_title.link);
	wl_list_init(&view->set_title.link);
	wl_list_remove(&view->toplevel_destroy.link);
}

static void view_handle_destroy(struct wl_listener *listener, void *data)
{
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

	struct hsdwl_popup *popup, *tmp_popup;
	wl_list_for_each_safe(popup, tmp_popup, &view->popups, link)
	{
		wl_list_remove(&popup->link);
		wl_list_remove(&popup->commit.link);
		wl_list_remove(&popup->destroy.link);
		wlr_scene_node_destroy(&popup->scene_tree->node);
		free(popup);
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

void handle_xdg_shell_popup(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(listener, server, new_xdg_shell_popup);
	struct wlr_xdg_popup *wlr_popup = data;

	/* Find the parent view from the popup's parent surface */
	struct wlr_surface *parent_surface = wlr_popup->parent;
	if (!parent_surface)
		return;
	struct wlr_xdg_surface *parent_xdg =
		wlr_xdg_surface_try_from_wlr_surface(parent_surface);
	if (!parent_xdg)
		return;
	struct hsdwl_view *view = parent_xdg->data;
	if (!view)
		return;

	struct hsdwl_popup *popup = calloc(1, sizeof(*popup));
	if (!popup)
		return;

	popup->wlr_popup = wlr_popup;
	popup->server = server;

	/* Compute output box once at creation — used on initial commit */
	if (!wl_list_empty(&server->outputs)) {
		struct hsdwl_output *o = wl_container_of(
			server->outputs.next, o, link);
		wlr_output_layout_get_box(server->output_layout,
			o->wlr_output, &popup->output_box);
	} else {
		popup->output_box = (struct wlr_box){0, 0, 1920, 1080};
	}

	/* Connect commit BEFORE scene create so our handler fires */
	popup->commit.notify = view_popup_handle_commit;
	wl_signal_add(&wlr_popup->base->surface->events.commit, &popup->commit);

	struct wlr_scene_tree *parent_tree = view_popup_parent_tree(view);
	if (!parent_tree)
	{
		wl_list_remove(&popup->commit.link);
		free(popup);
		return;
	}

	popup->scene_tree = wlr_scene_xdg_surface_create(
		parent_tree, wlr_popup->base);
	if (!popup->scene_tree)
	{
		wl_list_remove(&popup->commit.link);
		free(popup);
		return;
	}

	popup->destroy.notify = view_popup_handle_destroy;
	wl_signal_add(&wlr_popup->events.destroy, &popup->destroy);

	wl_list_insert(&view->popups, &popup->link);
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
	wl_list_init(&view->popups);
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
