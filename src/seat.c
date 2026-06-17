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
	wlr_seat_start_drag(server->seat, event->drag, event->serial);
}

static void handle_drag_icon_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_server *server = wl_container_of(
		listener, server, drag_icon_destroy);
	wlr_scene_node_destroy(&server->drag_icon_tree->node);
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

	server->drag_icon_tree = wlr_scene_tree_create(
		&server->scene->tree);
	if (!server->drag_icon_tree)
		return;

	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_create(server->drag_icon_tree,
			drag->icon->surface);
	if (!scene_surface)
	{
		wlr_scene_node_destroy(&server->drag_icon_tree->node);
		server->drag_icon_tree = NULL;
		return;
	}

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
