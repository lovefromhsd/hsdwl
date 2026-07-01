#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "xwayland.h"
#include "server.h"
#include "stage.h"
#include "tab-group.h"
#include "view.h"
#include "deco.h"

#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

bool xwayland_view_is_popup(struct hsdwl_view *view)
{
	struct wlr_xwayland_surface *xs = view->xwayland_surface;
	if (!xs)
		return false;
	if (xs->override_redirect)
		return true;
	if (!xs->parent)
		return false;
	/* Non-OR window with a transient parent and popup-like
	 * _NET_WM_WINDOW_TYPE (e.g. Ghidra/Java Swing menus).
	 * Treating these as OR for scene tree + focus avoids
	 * the focus-steal that triggers FocusOut on the parent
	 * and makes Swing dismiss the menu. */
	return wlr_xwayland_surface_has_window_type(xs,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU)
		|| wlr_xwayland_surface_has_window_type(xs,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU)
		|| wlr_xwayland_surface_has_window_type(xs,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU)
		|| wlr_xwayland_surface_has_window_type(xs,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP)
		|| wlr_xwayland_surface_has_window_type(xs,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO)
		|| wlr_xwayland_surface_has_window_type(xs,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND)
		|| wlr_xwayland_surface_has_window_type(xs,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION);
}

static void xwayland_view_handle_surface_map(
		struct wl_listener *listener, void *data)
{
	struct hsdwl_view *view = wl_container_of(listener, view, map);
	(void)data;
	struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
	bool is_popup = xwayland_view_is_popup(view);

	if (!view->scene_tree)
	{
		int bw = view->server->config.border_width;
		int tb = view->server->config.titlebar_height;
		if (tb < 0) tb = 0;
		struct wlr_scene_tree *parent =
			is_popup
			? view->server->override_tree
			: view->server->workspaces[
				view->server->current_workspace];
		view->scene_tree = wlr_scene_tree_create(parent);
		if (!view->scene_tree)
		{
			wlr_log(WLR_ERROR, "%s",
				"wlr_scene_tree_create failed");
			return;
		}
		view->scene_tree->node.data = view;
		if (is_popup)
			wlr_scene_node_raise_to_top(
				&view->scene_tree->node);

		view->content_tree = wlr_scene_tree_create(
			view->scene_tree);
		if (!view->content_tree)
		{
			wlr_log(WLR_ERROR, "%s",
				"wlr_scene_tree_create failed");
			return;
		}
		if (!wlr_scene_surface_create(view->content_tree,
				xsurface->surface))
		{
			wlr_log(WLR_ERROR, "%s",
				"wlr_scene_surface_create failed");
			return;
		}
		wlr_scene_node_set_position(
			&view->content_tree->node,
			is_popup ? 0 : bw,
			is_popup ? 0 : (tb > 0 ? tb : bw));
		if (!is_popup)
			view_borders_create(view);
	}
	else
	{
		int bw = view->server->config.border_width;
		int tb = view->server->config.titlebar_height;
		if (tb < 0) tb = 0;
		wlr_scene_node_destroy(&view->content_tree->node);
		view->content_tree = wlr_scene_tree_create(
			view->scene_tree);
		if (!view->content_tree)
		{
			wlr_log(WLR_ERROR, "%s",
				"wlr_scene_tree_create failed");
			return;
		}
		if (!wlr_scene_surface_create(view->content_tree,
				xsurface->surface))
		{
			wlr_log(WLR_ERROR, "%s",
				"wlr_scene_surface_create failed");
			return;
		}
		wlr_scene_node_set_position(
			&view->content_tree->node,
			is_popup ? 0 : bw,
			is_popup ? 0 : (tb > 0 ? tb : bw));
	}

	titlebar_text_update(view);
	wlr_scene_node_set_position(
		&view->scene_tree->node, xsurface->x, xsurface->y);
	wlr_scene_node_set_enabled(
		&view->scene_tree->node, true);
	if (view->server->config.stage_manager_enabled
			&& !is_popup
			&& !view_is_floating_toolbar(view))
	{
		stage_manager_new_window(view->server, view, true);
	}
	else if (!is_popup
			|| wlr_xwayland_surface_override_redirect_wants_focus(
				xsurface))
	{
		view_focus(view->server, view);
	}
}

static void xwayland_view_handle_surface_unmap(
		struct wl_listener *listener, void *data)
{
	struct hsdwl_view *view = wl_container_of(listener, view, unmap);
	(void)data;
	if (view->scene_tree)
		wlr_scene_node_set_enabled(
			&view->scene_tree->node, false);
	if (view->server->grabbed_view == view)
		view->server->grabbed_view = NULL;
	if (!view->xwayland_surface
			|| !xwayland_view_is_popup(view)
			|| wlr_xwayland_surface_override_redirect_wants_focus(
				view->xwayland_surface))
		view_focus(view->server,
			view_next(view->server, view));
}

static void xwayland_view_handle_surface_commit(
		struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, commit);
	if (view->server->config.stage_manager_enabled)
		stage_manager_notify_surface_commit(view->server, view);
}

static void xwayland_view_handle_set_geometry(
		struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(
		listener, view, set_geometry);
	if (view->scene_tree && view->xwayland_surface)
	{
		int nx = view->xwayland_surface->x;
		int ny = view->xwayland_surface->y;
		if (view_is_stage_managed(view))
			nx -= SIDEBAR_WIDTH;
		wlr_scene_node_set_position(
			&view->scene_tree->node, nx, ny);
		view_borders_update(view);
		titlebar_text_update(view);
	}
}

static void xwayland_view_handle_associate(
		struct wl_listener *listener, void *data)
{
	struct hsdwl_view *view = wl_container_of(listener, view, associate);
	(void)data;
	struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
	if (!xsurface)
		return;

	if (view->associated)
	{
		wl_list_remove(&view->map.link);
		wl_list_remove(&view->unmap.link);
		wl_list_remove(&view->commit.link);
	}
	view->map.notify = xwayland_view_handle_surface_map;
	wl_signal_add(&xsurface->surface->events.map,
		&view->map);
	view->unmap.notify = xwayland_view_handle_surface_unmap;
	wl_signal_add(&xsurface->surface->events.unmap,
		&view->unmap);
	view->commit.notify = xwayland_view_handle_surface_commit;
	wl_signal_add(&xsurface->surface->events.commit,
		&view->commit);
	view->associated = true;
}

static void xwayland_view_handle_dissociate(
		struct wl_listener *listener, void *data)
{
	struct hsdwl_view *view = wl_container_of(listener, view, dissociate);
	(void)data;
	struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
	if (view->associated)
	{
		wl_list_remove(&view->map.link);
		wl_list_remove(&view->unmap.link);
		wl_list_remove(&view->commit.link);
		view->associated = false;
	}
	if (xsurface)
		xsurface->data = NULL;
}

static void xwayland_view_handle_request_configure(
		struct wl_listener *listener, void *data)
{
	struct hsdwl_view *view = wl_container_of(
		listener, view, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;

	if (!view->xwayland_surface)
		return;

	wlr_xwayland_surface_configure(
		view->xwayland_surface, ev->x, ev->y,
		ev->width, ev->height);

	if (view->scene_tree)
	{
		int nx = ev->x;
		int ny = ev->y;
		if (view_is_stage_managed(view))
			nx -= SIDEBAR_WIDTH;
		wlr_scene_node_set_position(
			&view->scene_tree->node, nx, ny);
	}
}

static void xwayland_view_handle_destroy(
		struct wl_listener *listener, void *data)
{
	struct hsdwl_view *view = wl_container_of(listener, view, destroy);
	(void)data;
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
	if (view->server->config.stage_manager_enabled)
		stage_manager_notify_view_removed(view->server, view);
	wl_list_remove(&view->link);
	wl_list_remove(&view->associate.link);
	wl_list_remove(&view->dissociate.link);
	wl_list_remove(&view->request_configure.link);
	wl_list_remove(&view->set_geometry.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->destroy.link);
	if (view->decoration_destroy.notify)
		wl_list_remove(&view->decoration_destroy.link);
	if (view->decoration_request_mode.notify)
		wl_list_remove(&view->decoration_request_mode.link);
	if (view->associated)
	{
		wl_list_remove(&view->map.link);
		wl_list_remove(&view->unmap.link);
		wl_list_remove(&view->commit.link);
	}

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
	view->content_tree = NULL;
	view->scene_tree = NULL;
	free(view);
}

static void xwayland_view_handle_set_title(
		struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, set_title);
	if (view->xwayland_surface && view->xwayland_surface->title)
	{
		strncpy(view->cached_title,
			view->xwayland_surface->title,
			sizeof(view->cached_title) - 1);
		view->cached_title[sizeof(view->cached_title) - 1] = '\0';
	}
	titlebar_text_update(view);
}

static void xwayland_handle_new_surface(
		struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xsurface = data;

	struct hsdwl_view *view = calloc(1, sizeof(*view));
	if (!view)
	{
		return;
	}

	view->server = server;
	view->xwayland_surface = xsurface;
	view->tab_group = NULL;
	wl_list_init(&view->tab_group_link);
	xsurface->data = view;
	wl_list_insert(&server->views, &view->link);

	view->associate.notify = xwayland_view_handle_associate;
	wl_signal_add(&xsurface->events.associate,
		&view->associate);
	view->dissociate.notify = xwayland_view_handle_dissociate;
	wl_signal_add(&xsurface->events.dissociate,
		&view->dissociate);
	view->request_configure.notify =
		xwayland_view_handle_request_configure;
	wl_signal_add(&xsurface->events.request_configure,
		&view->request_configure);
	view->set_geometry.notify =
		xwayland_view_handle_set_geometry;
	wl_signal_add(&xsurface->events.set_geometry,
		&view->set_geometry);
	view->set_title.notify = xwayland_view_handle_set_title;
	wl_signal_add(&xsurface->events.set_title,
		&view->set_title);
	if (xsurface->title)
	{
		strncpy(view->cached_title, xsurface->title,
			sizeof(view->cached_title) - 1);
		view->cached_title[sizeof(view->cached_title) - 1] = '\0';
	}
	view->destroy.notify = xwayland_view_handle_destroy;
	wl_signal_add(&xsurface->events.destroy,
		&view->destroy);
}

bool hsdwl_xwayland_init(struct hsdwl_server *server)
{
	server->xwayland = wlr_xwayland_create(
		server->display, server->compositor, true);
	if (!server->xwayland)
	{
		wlr_log(WLR_ERROR, "%s",
			"wlr_xwayland_create failed");
		return false;
	}

	wlr_xwayland_set_seat(server->xwayland, server->seat);

	struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(
		server->cursor_mgr, "left_ptr", 1);
	if (xcursor && xcursor->images[0]) {
		struct wlr_xcursor_image *img = xcursor->images[0];
		struct wlr_buffer *buf = wlr_xcursor_image_get_buffer(img);
		if (buf) {
			wlr_xwayland_set_cursor(server->xwayland, buf,
				img->hotspot_x, img->hotspot_y);
		}
	}

	server->new_xwayland_surface.notify =
		xwayland_handle_new_surface;
	wl_signal_add(&server->xwayland->events.new_surface,
		&server->new_xwayland_surface);

	wlr_log(WLR_INFO, "xwayland display: %s",
		server->xwayland->display_name);
	setenv("DISPLAY", server->xwayland->display_name, true);

	return true;
}

void hsdwl_xwayland_finish(struct hsdwl_server *server)
{
	if (!server->xwayland)
		return;
	wl_list_remove(&server->new_xwayland_surface.link);
	wlr_xwayland_destroy(server->xwayland);
	server->xwayland = NULL;
}
