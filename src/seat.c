#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "seat.h"
#include "server.h"

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>

static void seat_request_cursor(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused =
		server->seat->pointer_state.focused_client;
	if (focused == event->seat_client)
	{
		if (!event->surface
				|| !wlr_surface_has_buffer(event->surface))
		{
			wlr_cursor_set_xcursor(server->cursor,
				server->cursor_mgr, "default");
			return;
		}
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

static void seat_request_start_drag(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;
	wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
}

/*
 * Recursively clear input regions for the drag icon surface and all its
 * subsurfaces. This prevents wlr_scene_node_at from hitting the drag icon's
 * scene nodes instead of the actual window behind the cursor, which would
 * cause DnD coordinates to target the wrong surface.
 *
 * The drag icon role handler only clears the main surface's input region;
 * subsurface surfaces retain their normal input regions and would intercept
 * pointer events meant for the window beneath.
 */
static void drag_icon_clear_subsurface_input(struct wlr_surface *surface)
{
	pixman_region32_clear(&surface->input_region);

	struct wlr_subsurface *subsurface;
	wl_list_for_each(subsurface, &surface->current.subsurfaces_below,
			current.link) {
		drag_icon_clear_subsurface_input(subsurface->surface);
	}
	wl_list_for_each(subsurface, &surface->current.subsurfaces_above,
			current.link) {
		drag_icon_clear_subsurface_input(subsurface->surface);
	}
}

static void handle_drag_icon_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_server *server = wl_container_of(
		listener, server, drag_icon_destroy);
	wl_list_remove(&server->drag_icon_destroy.link);
	server->drag_icon_tree = NULL;
	server->drag_icon_destroy.notify = NULL;
}

static void seat_start_drag(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, start_drag);
	struct wlr_drag *drag = data;

	if (!drag || !drag->icon)
		return;

	server->drag_icon_tree = wlr_scene_drag_icon_create(
		&server->scene->tree, drag->icon);
	if (!server->drag_icon_tree)
		return;

	/*
	 * Clear input regions for all subsurface surfaces in the drag icon's
	 * surface tree. wlr_scene_drag_icon_create creates scene nodes for
	 * all subsurfaces via wlr_scene_subsurface_tree_create, but the drag
	 * icon role only clears the main surface's input region. Without this,
	 * wlr_scene_node_at finds subsurface buffer nodes instead of the
	 * window behind, sending DnD coordinates to the wrong surface.
	 */
	drag_icon_clear_subsurface_input(drag->icon->surface);

	wlr_scene_node_set_position(
		&server->drag_icon_tree->node,
		server->cursor->x, server->cursor->y);

	server->drag_icon_destroy.notify = handle_drag_icon_destroy;
	wl_signal_add(&drag->icon->events.destroy,
		&server->drag_icon_destroy);
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_server *server = wl_container_of(
		listener, server, pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL)
	{
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
			"default");
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void seat_request_set_primary_selection(
		struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat,
		event->source, event->serial);
}

bool seat_init(struct hsdwl_server *server)
{
	server->request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor,
		&server->request_cursor);
	server->pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(
		&server->seat->pointer_state.events.focus_change,
		&server->pointer_focus_change);
	server->request_set_selection.notify =
		seat_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection,
		&server->request_set_selection);
	server->request_set_primary_selection.notify =
		seat_request_set_primary_selection;
	wl_signal_add(
		&server->seat->events.request_set_primary_selection,
		&server->request_set_primary_selection);

	server->request_start_drag.notify = seat_request_start_drag;
	wl_signal_add(&server->seat->events.request_start_drag,
		&server->request_start_drag);
	server->start_drag.notify = seat_start_drag;
	wl_signal_add(&server->seat->events.start_drag,
		&server->start_drag);

	return true;
}
