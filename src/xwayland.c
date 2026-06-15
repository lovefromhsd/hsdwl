#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "xwayland.h"
#include "server.h"
#include "view.h"

#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

static void xwayland_view_handle_surface_map(
		struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, map);
	struct wlr_xwayland_surface *xsurface = view->xwayland_surface;

	if (!view->scene_tree)
	{
		view->scene_tree = wlr_scene_tree_create(
			view->server->workspaces[
				view->server->current_workspace]);
		if (!view->scene_tree)
		{
			wlr_log(WLR_ERROR,
				"wlr_scene_tree_create failed");
			return;
		}

		if (!wlr_scene_surface_create(view->scene_tree,
				xsurface->surface))
		{
			wlr_log(WLR_ERROR,
				"wlr_scene_surface_create failed");
			return;
		}

		view->scene_tree->node.data = view;
	}

	wlr_scene_node_set_position(
		&view->scene_tree->node, xsurface->x, xsurface->y);
	wlr_scene_node_set_enabled(
		&view->scene_tree->node, true);
	if (!xsurface->override_redirect
			|| wlr_xwayland_surface_override_redirect_wants_focus(
				xsurface))
		view_focus(view->server, view);
}

static void xwayland_view_handle_surface_unmap(
		struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, unmap);
	if (view->scene_tree)
		wlr_scene_node_set_enabled(
			&view->scene_tree->node, false);
	if (view->server->grabbed_view == view)
		view->server->grabbed_view = NULL;
	if (!view->xwayland_surface
			|| !view->xwayland_surface->override_redirect
			|| wlr_xwayland_surface_override_redirect_wants_focus(
				view->xwayland_surface))
		view_focus(view->server,
			view_next(view->server, view));
}

static void xwayland_view_handle_surface_commit(
		struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;
}

static void xwayland_view_handle_set_geometry(
		struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(
		listener, view, set_geometry);
	if (view->scene_tree && view->xwayland_surface)
	{
		wlr_scene_node_set_position(
			&view->scene_tree->node,
			view->xwayland_surface->x,
			view->xwayland_surface->y);
	}
}

static void xwayland_view_handle_associate(
		struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, associate);
	struct wlr_xwayland_surface *xsurface = view->xwayland_surface;

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
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, dissociate);
	if (view->associated)
	{
		wl_list_remove(&view->map.link);
		wl_list_remove(&view->unmap.link);
		wl_list_remove(&view->commit.link);
		view->associated = false;
	}
	view->xwayland_surface = NULL;
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
		wlr_scene_node_set_position(
			&view->scene_tree->node, ev->x, ev->y);
}

static void xwayland_view_handle_destroy(
		struct wl_listener *listener, void *data)
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
	wl_list_remove(&view->link);
	wl_list_remove(&view->associate.link);
	wl_list_remove(&view->dissociate.link);
	wl_list_remove(&view->request_configure.link);
	wl_list_remove(&view->set_geometry.link);
	wl_list_remove(&view->destroy.link);
	if (view->associated)
	{
		wl_list_remove(&view->map.link);
		wl_list_remove(&view->unmap.link);
		wl_list_remove(&view->commit.link);
	}
	free(view);
}

static void xwayland_handle_new_surface(
		struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xsurface = data;

	struct hsdwl_view *view = calloc(1, sizeof(*view));
	if (!view)
		return;

	view->server = server;
	view->xwayland_surface = xsurface;
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
		wlr_log(WLR_ERROR,
			"wlr_xwayland_create failed");
		return false;
	}

	wlr_xwayland_set_seat(server->xwayland, server->seat);

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
