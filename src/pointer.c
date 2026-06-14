#define _GNU_SOURCE

#include "pointer.h"
#include "server.h"
#include "view.h"

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

static void process_cursor_motion(struct hsdwl_server *server, uint32_t time)
{
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, server->cursor->x, server->cursor->y,
		&sx, &sy);

	struct wlr_surface *surface = NULL;
	if (node && node->type == WLR_SCENE_NODE_BUFFER)
	{
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *ss =
			wlr_scene_surface_try_from_buffer(sb);
		if (ss)
			surface = ss->surface;
	}

	if (surface)
	{
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
	}
	else
	{
		wlr_seat_pointer_clear_focus(server->seat);
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
			"default");
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	wlr_seat_pointer_notify_button(server->seat,
		event->time_msec, event->button, event->state);

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED)
	{
		double sx, sy;
		struct wlr_scene_node *node = wlr_scene_node_at(
			&server->scene->tree.node,
			server->cursor->x, server->cursor->y, &sx, &sy);
		if (node)
		{
			struct wlr_scene_tree *tree = node->parent;
			while (tree && tree->node.data == NULL)
				tree = tree->node.parent;
			if (tree && tree->node.data)
			{
				struct hsdwl_view *view = tree->node.data;
				wlr_xdg_toplevel_set_activated(
					view->xdg_surface->toplevel, true);
				wlr_xdg_surface_schedule_configure(
					view->xdg_surface);
				struct wlr_keyboard *kb =
					wlr_seat_get_keyboard(server->seat);
				if (kb)
				{
					wlr_seat_keyboard_notify_enter(
						server->seat,
						view->xdg_surface->surface,
						NULL, 0, NULL);
				}
			}
		}
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete,
		event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_server *server = wl_container_of(
		listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void seat_request_cursor(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused =
		server->seat->pointer_state.focused_client;
	if (focused == event->seat_client)
	{
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
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

bool pointer_init(struct hsdwl_server *server)
{
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
		"default");

	server->cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server->cursor->events.motion,
		&server->cursor_motion);
	server->cursor_motion_absolute.notify =
		server_cursor_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute,
		&server->cursor_motion_absolute);
	server->cursor_button.notify = server_cursor_button;
	wl_signal_add(&server->cursor->events.button,
		&server->cursor_button);
	server->cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server->cursor->events.axis,
		&server->cursor_axis);
	server->cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server->cursor->events.frame,
		&server->cursor_frame);

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

	return true;
}

void pointer_handle_new(struct hsdwl_server *server,
		struct wlr_input_device *device)
{
	wlr_cursor_attach_input_device(server->cursor, device);
}
