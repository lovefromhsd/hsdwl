#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "layer-shell.h"
#include "pointer.h"
#include "server.h"
#include "tab-group.h"
#include "view.h"

#include <linux/input-event-codes.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xkbcommon/xkbcommon.h>

static struct hsdwl_view *view_at(struct hsdwl_server *server,
		double lx, double ly, double *sx, double *sy)
{
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (!node)
		return NULL;
	struct wlr_scene_tree *tree = node->parent;
	while (tree && tree->node.data == NULL)
		tree = tree->node.parent;
	if (tree && tree->node.data)
		return tree->node.data;
	return NULL;
}

static uint32_t determine_resize_edges(struct hsdwl_server *server,
		struct hsdwl_view *view, double cursor_x, double cursor_y)
{
	int wx = view->scene_tree->node.x;
	int wy = view->scene_tree->node.y;
	int ww, wh;
	if (view->xdg_surface)
	{
		ww = view->xdg_surface->geometry.width;
		wh = view->xdg_surface->geometry.height;
	}
	else
	{
		ww = view->xwayland_surface->width;
		wh = view->xwayland_surface->height;
	}

	if (ww < 1 || wh < 1)
		return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;

	double rx = cursor_x - wx;
	double ry = cursor_y - wy;

	int t = server->config.edge_threshold;
	uint32_t edges = XDG_TOPLEVEL_RESIZE_EDGE_NONE;
	if (rx < t)
		edges |= XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
	if (rx > ww - t)
		edges |= XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
	if (ry < t)
		edges |= XDG_TOPLEVEL_RESIZE_EDGE_TOP;
	if (ry > wh - t)
		edges |= XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;

	if (edges == XDG_TOPLEVEL_RESIZE_EDGE_NONE)
		edges = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;

	return edges;
}

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

	if (server->drag_icon_tree)
		wlr_scene_node_set_position(
			&server->drag_icon_tree->node,
			server->cursor->x, server->cursor->y);
}

static void apply_resize(struct hsdwl_server *server)
{
	if (!server->grabbed_view || !server->grabbed_view->scene_tree)
		return;
	if (!server->grabbed_view->xdg_surface
			&& !server->grabbed_view->xwayland_surface)
		return;

	double dx = server->cursor->x - server->grab_x;
	double dy = server->cursor->y - server->grab_y;

	int new_x = server->grab_view_x;
	int new_y = server->grab_view_y;
	int new_w = server->grab_geom_width;
	int new_h = server->grab_geom_height;

	if (server->resize_edges & XDG_TOPLEVEL_RESIZE_EDGE_RIGHT)
		new_w = server->grab_geom_width + (int)dx;
	if (server->resize_edges & XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM)
		new_h = server->grab_geom_height + (int)dy;
	if (server->resize_edges & XDG_TOPLEVEL_RESIZE_EDGE_LEFT)
	{
		new_w = server->grab_geom_width - (int)dx;
		if (new_w < server->config.min_window_size)
			new_w = server->config.min_window_size;
		new_x = server->grab_view_x
			+ (server->grab_geom_width - new_w);
	}
	if (server->resize_edges & XDG_TOPLEVEL_RESIZE_EDGE_TOP)
	{
		new_h = server->grab_geom_height - (int)dy;
		if (new_h < server->config.min_window_size)
			new_h = server->config.min_window_size;
		new_y = server->grab_view_y
			+ (server->grab_geom_height - new_h);
	}

	if (new_w < server->config.min_window_size)
		new_w = server->config.min_window_size;
	if (new_h < server->config.min_window_size)
		new_h = server->config.min_window_size;

	wlr_scene_node_set_position(
		&server->grabbed_view->scene_tree->node,
		new_x, new_y);
	if (server->grabbed_view->xdg_surface)
	{
		wlr_xdg_toplevel_set_size(
			server->grabbed_view->xdg_surface->toplevel,
			new_w, new_h);
	}
	else
	{
		wlr_xwayland_surface_configure(
			server->grabbed_view->xwayland_surface,
			new_x, new_y, new_w, new_h);
	}
}

static bool handle_grab_motion(struct hsdwl_server *server)
{
	if (!server->grabbed_view || !server->grabbed_view->scene_tree)
		return false;

	switch (server->cursor_mode)
	{
	case HSDWL_CURSOR_MOVE:
	{
		double dx = server->cursor->x - server->grab_x;
		double dy = server->cursor->y - server->grab_y;
		wlr_scene_node_set_position(
			&server->grabbed_view->scene_tree->node,
			server->grab_view_x + (int)dx,
			server->grab_view_y + (int)dy);

		if (!hsdwl_tab_group_is_member(server->grabbed_view))
		{
			bool vw =
				server->grabbed_view->scene_tree->node.enabled;
			wlr_scene_node_set_enabled(
				&server->grabbed_view->scene_tree->node, false);

			bool pvw = server->preview_tree
				&& server->preview_tree->node.enabled;
			if (server->preview_tree)
				wlr_scene_node_set_enabled(
					&server->preview_tree->node, false);

			double sx, sy;
			struct hsdwl_view *target = view_at(server,
				server->cursor->x, server->cursor->y, &sx, &sy);

			wlr_scene_node_set_enabled(
				&server->grabbed_view->scene_tree->node, vw);

			if (target && target != server->grabbed_view
					&& !hsdwl_tab_group_is_member(target))
			{
				if (server->grab_target != target)
				{
					if (server->preview_tree)
						hsdwl_tab_group_hide_preview(server);
					server->grab_target = target;
					hsdwl_tab_group_show_preview(server,
						target,
						server->cursor->x,
						server->cursor->y);
				}
				else if (server->preview_tree && pvw)
				{
					wlr_scene_node_set_enabled(
						&server->preview_tree->node, true);
				}
			}
			else
			{
				if (server->grab_target)
				{
					hsdwl_tab_group_hide_preview(server);
					server->grab_target = NULL;
				}
			}
		}
		return true;
	}
	case HSDWL_CURSOR_RESIZE:
		apply_resize(server);
		return true;
	default:
		return false;
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);

	if (handle_grab_motion(server))
		return;

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

	if (handle_grab_motion(server))
		return;

	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED)
	{
		struct hsdwl_view *tv = hsdwl_tab_group_view_at(server,
			server->cursor->x, server->cursor->y);
		if (tv && tv->tab_group)
		{
			hsdwl_tab_group_set_active(tv->tab_group, tv);
			wlr_seat_pointer_notify_button(server->seat,
				event->time_msec, event->button, event->state);
			view_focus(server, tv);
			return;
		}

		struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
		bool alt = kb && xkb_state_mod_name_is_active(
			kb->xkb_state, server->config.mod_key,
			XKB_STATE_MODS_EFFECTIVE);

		if (alt && event->button == BTN_LEFT)
		{
			double sx, sy;
			struct hsdwl_view *view = view_at(server,
				server->cursor->x, server->cursor->y, &sx, &sy);
			if (view && view->scene_tree
					&& !(view->xwayland_surface
						&& view->xwayland_surface
							->override_redirect))
			{
				if (view->maximized)
					view_maximize(server, view);

				if (hsdwl_tab_group_is_member(view))
				{
					struct hsdwl_tab_group *g = view->tab_group;
					hsdwl_tab_group_remove_view(g, view);
					server->grab_view_x =
						view->scene_tree->node.x;
					server->grab_view_y =
						view->scene_tree->node.y;
				}

				server->cursor_mode = HSDWL_CURSOR_MOVE;
				server->grabbed_view = view;
				server->grab_x = server->cursor->x;
				server->grab_y = server->cursor->y;
				server->grab_view_x =
					view->scene_tree->node.x;
				server->grab_view_y =
					view->scene_tree->node.y;
				wlr_scene_node_raise_to_top(
					&view->scene_tree->node);
				wlr_cursor_set_xcursor(server->cursor,
					server->cursor_mgr, "move");
				view_focus(server, view);
				return;
			}
		}

		if (alt && event->button == BTN_RIGHT)
		{
			double sx, sy;
			struct hsdwl_view *view = view_at(server,
				server->cursor->x, server->cursor->y, &sx, &sy);
			if (view && view->scene_tree
					&& (view->xdg_surface
						|| view->xwayland_surface)
					&& !(view->xwayland_surface
						&& view->xwayland_surface
							->override_redirect))
			{
				if (view->maximized)
					view_maximize(server, view);
				server->cursor_mode = HSDWL_CURSOR_RESIZE;
				server->grabbed_view = view;
				server->grab_x = server->cursor->x;
				server->grab_y = server->cursor->y;
				server->grab_view_x =
					view->scene_tree->node.x;
				server->grab_view_y =
					view->scene_tree->node.y;
				if (view->xdg_surface)
				{
					server->grab_geom_width =
						view->xdg_surface->geometry.width;
					server->grab_geom_height =
						view->xdg_surface->geometry.height;
				}
				else
				{
					server->grab_geom_width =
						view->xwayland_surface->width;
					server->grab_geom_height =
						view->xwayland_surface->height;
				}
				server->resize_edges = determine_resize_edges(server,
					view, server->cursor->x,
					server->cursor->y);
				wlr_scene_node_raise_to_top(
					&view->scene_tree->node);
				wlr_cursor_set_xcursor(server->cursor,
					server->cursor_mgr, "move");
				view_focus(server, view);
				return;
			}
		}

		wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);

		double sx, sy;
		struct hsdwl_view *view = view_at(server,
			server->cursor->x, server->cursor->y, &sx, &sy);
		if (view)
		{
			bool is_or = view->xwayland_surface
				&& view->xwayland_surface->override_redirect
				&& !wlr_xwayland_surface_override_redirect_wants_focus(
					view->xwayland_surface);
			if (!is_or)
				view_focus(server, view);
		}
		else
		{
			double sx2, sy2;
			struct wlr_scene_node *node =
				wlr_scene_node_at(
					&server->scene->tree.node,
					server->cursor->x,
					server->cursor->y, &sx2, &sy2);
			if (node && node->type
					== WLR_SCENE_NODE_BUFFER)
			{
				struct wlr_scene_buffer *sb =
					wlr_scene_buffer_from_node(
						node);
				struct wlr_scene_surface *ss =
					wlr_scene_surface_try_from_buffer(
						sb);
				if (ss)
				{
					struct wlr_surface *surf =
						ss->surface;
					struct hsdwl_layer_surface
						*layer;
					wl_list_for_each(layer,
						&server->layer_surfaces,
						link)
					{
						if (layer->layer_surface
								->surface != surf)
							continue;
						if (!layer->scene_tree
							|| !layer->scene_tree
								->node.enabled)
							continue;
						if (layer->layer_surface
							->current
							.keyboard_interactive
							== 0)
							continue;
						struct wlr_keyboard *kb3
							= wlr_seat_get_keyboard(
								server->seat);
						if (kb3)
							wlr_seat_keyboard_notify_enter(
								server->seat,
								surf, NULL,
								0, NULL);
						server->focused_layer
							= layer;
						break;
					}
				}
			}
		}
		return;
	}

	if (server->cursor_mode == HSDWL_CURSOR_MOVE
			|| server->cursor_mode == HSDWL_CURSOR_RESIZE)
	{
		if (server->cursor_mode == HSDWL_CURSOR_MOVE
				&& server->grab_target
				&& server->grabbed_view
				&& !hsdwl_tab_group_is_member(server->grabbed_view))
		{
			hsdwl_tab_group_create(server,
				server->grabbed_view, server->grab_target,
				HSDWL_TAB_HORIZONTAL);
		}

		server->cursor_mode = HSDWL_CURSOR_PASSTHROUGH;
		server->grabbed_view = NULL;
		server->grab_target = NULL;
		hsdwl_tab_group_hide_preview(server);
		wlr_cursor_set_xcursor(server->cursor,
			server->cursor_mgr, "default");
		return;
	}

	wlr_seat_pointer_notify_button(server->seat,
		event->time_msec, event->button, event->state);
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

void pointer_handle_new(struct hsdwl_server *server,
		struct wlr_input_device *device)
{
	wlr_cursor_attach_input_device(server->cursor, device);
}
