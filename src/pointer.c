#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "layer-shell.h"
#include "pointer.h"
#include "server.h"
#include "stage.h"
#include "tab-group.h"
#include "tab-group-layout.h"
#include "view.h"
#include "view-maximize.h"

#include <linux/input-event-codes.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
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
	{
		struct hsdwl_view *v;
		wl_list_for_each(v, &server->views, link)
			if (v == tree->node.data) return v;
	}
	return NULL;
}

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

static void get_view_bounds(struct hsdwl_view *view, struct wlr_box *box)
{
	if (!view || !view->scene_tree)
	{
		box->x = box->y = box->width = box->height = 0;
		return;
	}
	if (hsdwl_tab_group_is_member(view))
	{
		struct hsdwl_tab_group *g = view->tab_group;
		box->x = g->scene_tree->node.x;
		box->y = g->scene_tree->node.y;
		box->width = g->content_area_box.width;
		box->height = g->content_area_box.height
			+ g->tab_bar_thickness;
	}
	else
	{
		box->x = view->scene_tree->node.x;
		box->y = view->scene_tree->node.y;
		int cw = 0, ch = 0;
		if (view->xdg_surface
				&& view->xdg_surface->configured)
		{
			cw = view->xdg_surface->geometry.width;
			ch = view->xdg_surface->geometry.height;
		}
		else if (view->xwayland_surface)
		{
			cw = view->xwayland_surface->width;
			ch = view->xwayland_surface->height;
		}
		int bw = view->server->config.border_width;
		int tb = view->server->config.titlebar_height;
		if (tb < 0) tb = 0;
		box->width = cw + 2 * bw;
		box->height = ch + (tb > 0 ? tb : bw) + bw;
	}
}

static bool overlap_meets_threshold(struct hsdwl_view *dragged,
		struct hsdwl_view *target, float threshold)
{
	if (threshold <= 0.0f)
		return true;
	struct wlr_box bd, bt;
	get_view_bounds(dragged, &bd);
	get_view_bounds(target, &bt);
	int ix = MAX(bd.x, bt.x);
	int iy = MAX(bd.y, bt.y);
	int ix2 = MIN(bd.x + bd.width, bt.x + bt.width);
	int iy2 = MIN(bd.y + bd.height, bt.y + bt.height);
	int iw = ix2 > ix ? ix2 - ix : 0;
	int ih = iy2 > iy ? iy2 - iy : 0;
	int ia = iw * ih;
	if (!ia)
		return false;
	int da = bd.width * bd.height;
	if (!da)
		return false;
	return (float)ia / da >= threshold;
}

static uint32_t determine_resize_edges(struct hsdwl_server *server,
		struct hsdwl_view *view, double cursor_x, double cursor_y)
{
	int wx, wy, ww, wh;
	if (hsdwl_tab_group_is_member(view))
	{
		struct hsdwl_tab_group *g = view->tab_group;
		wx = g->scene_tree->node.x;
		wy = g->scene_tree->node.y + g->tab_bar_thickness;
		ww = g->content_area_box.width;
		wh = g->content_area_box.height;
	}
	else
	{
		wx = view->scene_tree->node.x;
		wy = view->scene_tree->node.y;
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


	struct hsdwl_view *v = server->grabbed_view;
	for (int i = 0; i < 4; i++)
		if (v->border_rects[i])
			wlr_scene_node_set_enabled(
				&v->border_rects[i]->node, false);
	if (v->shadow_rect)
		wlr_scene_node_set_enabled(
			&v->shadow_rect->node, false);

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

	int bw = server->config.border_width;
	int tb = server->config.titlebar_height;
	if (tb < 0) tb = 0;


	int pw, ph;
	if (tb > 0)
	{
		pw = new_w + 2 * bw;
		ph = new_h + tb + bw;
	}
	else
	{
		pw = new_w + 2 * bw;
		ph = new_h + 2 * bw;
	}

	if (hsdwl_tab_group_is_member(server->grabbed_view))
	{
		struct hsdwl_tab_group *g = server->grabbed_view->tab_group;
		if (g && g->scene_tree)
			new_y -= g->tab_bar_thickness;
		pw = new_w;
		ph = new_h;
	}

	server->resize_preview_x = new_x;
	server->resize_preview_y = new_y;
	server->resize_preview_w = new_w;
	server->resize_preview_h = new_h;

	int preview_x = new_x;
	int preview_y = new_y;
	if (view_is_stage_managed(server->grabbed_view))
		preview_x += SIDEBAR_WIDTH;
	if (hsdwl_tab_group_is_member(server->grabbed_view))
	{
		struct hsdwl_tab_group *g = server->grabbed_view->tab_group;
		if (g && g->scene_tree)
			preview_y += 2 * g->tab_bar_thickness;
	}


	float *col = server->config.border_color_focused;
	for (int i = 0; i < 4; i++)
	{
		if (!server->resize_preview[i])
		{
			server->resize_preview[i] = wlr_scene_rect_create(
				&server->scene->tree, 1, 1, col);
			if (server->resize_preview[i])
				wlr_scene_node_raise_to_top(
					&server->resize_preview[i]->node);
		}
	}


	if (server->resize_preview[0]) {
		wlr_scene_rect_set_size(
			server->resize_preview[0], pw, bw);
		wlr_scene_node_set_position(
			&server->resize_preview[0]->node, preview_x, preview_y);
		wlr_scene_node_set_enabled(
			&server->resize_preview[0]->node, true);
	}

	if (server->resize_preview[1]) {
		wlr_scene_rect_set_size(
			server->resize_preview[1], pw, bw);
		wlr_scene_node_set_position(
			&server->resize_preview[1]->node,
			preview_x, preview_y + ph - bw);
		wlr_scene_node_set_enabled(
			&server->resize_preview[1]->node, true);
	}

	if (server->resize_preview[2]) {
		wlr_scene_rect_set_size(
			server->resize_preview[2], bw, ph);
		wlr_scene_node_set_position(
			&server->resize_preview[2]->node, preview_x, preview_y);
		wlr_scene_node_set_enabled(
			&server->resize_preview[2]->node, true);
	}

	if (server->resize_preview[3]) {
		wlr_scene_rect_set_size(
			server->resize_preview[3], bw, ph);
		wlr_scene_node_set_position(
			&server->resize_preview[3]->node,
			preview_x + pw - bw, preview_y);
		wlr_scene_node_set_enabled(
			&server->resize_preview[3]->node, true);
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

		if (hsdwl_tab_group_is_member(server->grabbed_view))
		{
			struct hsdwl_tab_group *g =
				server->grabbed_view->tab_group;
			if (g && g->scene_tree)
				wlr_scene_node_set_position(
					&g->scene_tree->node,
					server->grab_view_x + (int)dx,
					server->grab_view_y + (int)dy);

			double sx, sy;
			struct hsdwl_view *target = view_at(server,
				server->cursor->x, server->cursor->y,
				&sx, &sy);

			if (target && target != server->grabbed_view
					&& target->tab_group != g
					&& overlap_meets_threshold(
						server->grabbed_view, target,
						server->config.group_overlap_threshold))
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
				else if (server->preview_tree
						&& server->preview_tree->node.enabled)
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
		else
		{
			wlr_scene_node_set_position(
				&server->grabbed_view->scene_tree->node,
				server->grab_view_x + (int)dx,
				server->grab_view_y + (int)dy);

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
				&& overlap_meets_threshold(
					server->grabbed_view, target,
					server->config.group_overlap_threshold))
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
	case HSDWL_CURSOR_STAGE_DRAG:
		return true;
	case HSDWL_CURSOR_TAB_REORDER:
	{
		struct hsdwl_view *v = server->grabbed_view;
		if (!v || !v->tab_group || !v->tab_group->scene_tree)
		{
			server->cursor_mode = HSDWL_CURSOR_PASSTHROUGH;
			server->grabbed_view = NULL;
			return true;
		}
		struct hsdwl_tab_group *g = v->tab_group;
		int num = wl_list_length(&g->tab_buttons);
		if (num < 2)
		{
			server->cursor_mode = HSDWL_CURSOR_PASSTHROUGH;
			server->grabbed_view = NULL;
			return true;
		}

		double local_y = server->cursor->y
			- g->scene_tree->node.y;
		if (local_y < 0 || local_y >= g->tab_bar_thickness)
			return true;
		double local_x = server->cursor->x
			- g->scene_tree->node.x;
		int seg = g->content_area_box.width / num;
		int idx = (int)(local_x / seg);
		if (idx < 0) idx = 0;
		if (idx >= num) idx = num - 1;
		int old_idx = 0;
		struct hsdwl_tab_button *btn;
		wl_list_for_each(btn, &g->tab_buttons, link)
		{
			if (btn->view == v)
				break;
			old_idx++;
		}
		if (idx != old_idx)
			hsdwl_tab_group_reorder(g, v, idx);
		return true;
	}
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

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED
			&& server->cursor_mode == HSDWL_CURSOR_STAGE_DRAG)
		return;

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED)
	{

		if (server->config.stage_manager_enabled
				&& server->cursor->x < SIDEBAR_WIDTH
				&& event->button == BTN_LEFT)
		{
			double sx, sy;
			struct wlr_scene_node *n = wlr_scene_node_at(
				&server->ws_sidebar_trees[
					server->current_workspace]->node,
				server->cursor->x, server->cursor->y, &sx, &sy);
			if (n)
			{
				struct custom_stage *stage = stage_at(server,
					server->cursor->x, server->cursor->y,
					server->current_workspace);
				if (stage)
				{
					server->cursor_mode =
						HSDWL_CURSOR_STAGE_DRAG;
					server->drag_source_stage = stage;
					wlr_cursor_set_xcursor(
						server->cursor,
						server->cursor_mgr,
						"grabbing");
					return;
				}
			}
		}

		struct hsdwl_view *tv = hsdwl_tab_group_view_at(server,
			server->cursor->x, server->cursor->y);
		if (tv && tv->tab_group)
		{
			struct hsdwl_tab_group *g = tv->tab_group;
			double ly = server->cursor->y - g->scene_tree->node.y;
			bool on_tab_bar = ly >= 0
				&& ly < g->tab_bar_thickness;

			if (event->button == BTN_RIGHT)
			{
				if (on_tab_bar)
				{
					hsdwl_tab_group_remove_view(g, tv);
					return;
				}
			}
			else if (on_tab_bar)
			{
				hsdwl_tab_group_set_active(g, tv);
				server->cursor_mode =
					HSDWL_CURSOR_TAB_REORDER;
				server->grabbed_view = tv;
				server->grab_x = server->cursor->x;
				server->grab_y = server->cursor->y;
				wlr_cursor_set_xcursor(
					server->cursor,
					server->cursor_mgr,
					"grabbing");
				return;
			}
			else
			{
				hsdwl_tab_group_set_active(g, tv);

			}
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

				server->cursor_mode = HSDWL_CURSOR_MOVE;
				server->grabbed_view = view;
				server->grab_x = server->cursor->x;
				server->grab_y = server->cursor->y;

				if (hsdwl_tab_group_is_member(view))
				{
					struct hsdwl_tab_group *g =
						view->tab_group;
					server->grab_view_x =
						g->scene_tree->node.x;
					server->grab_view_y =
						g->scene_tree->node.y;
					wlr_scene_node_raise_to_top(
						&g->scene_tree->node);
				}
				else
				{
					server->grab_view_x =
						view->scene_tree->node.x;
					server->grab_view_y =
						view->scene_tree->node.y;
					wlr_scene_node_raise_to_top(
						&view->scene_tree->node);
				}

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
				if (hsdwl_tab_group_is_member(view))
				{
					struct hsdwl_tab_group *g = view->tab_group;
					server->grab_view_x =
						g->scene_tree->node.x;
					server->grab_view_y =
						g->scene_tree->node.y;
					wlr_scene_node_raise_to_top(
						&g->scene_tree->node);
				}
				else
				{
					server->grab_view_x =
						view->scene_tree->node.x;
					server->grab_view_y =
						view->scene_tree->node.y;
					wlr_scene_node_raise_to_top(
						&view->scene_tree->node);
				}
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
				if (!hsdwl_tab_group_is_member(view))
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

		/* Don't steal focus when a popup grab is active */
		if (!wlr_seat_keyboard_has_grab(server->seat))
		{
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

	if (server->cursor_mode == HSDWL_CURSOR_STAGE_DRAG)
	{
		if (server->drag_source_stage)
		{
			size_t ws = server->current_workspace;
			if (server->cursor->x > SIDEBAR_WIDTH)
				stage_manager_merge(server,
					server->drag_source_stage, ws);
			else
				stage_manager_switch(server,
					server->drag_source_stage, ws);
		}
		server->cursor_mode = HSDWL_CURSOR_PASSTHROUGH;
		server->drag_source_stage = NULL;
		wlr_cursor_set_xcursor(server->cursor,
			server->cursor_mgr, "default");
		return;
	}

	if (server->cursor_mode == HSDWL_CURSOR_MOVE
			|| server->cursor_mode == HSDWL_CURSOR_RESIZE
			|| server->cursor_mode == HSDWL_CURSOR_TAB_REORDER)
	{
		if (server->cursor_mode == HSDWL_CURSOR_MOVE
				&& server->grab_target
				&& server->grabbed_view)
		{
			if (hsdwl_tab_group_is_member(server->grab_target))
			{
				hsdwl_tab_group_add_view(
					server->grab_target->tab_group,
					server->grabbed_view);
			}
			else if (hsdwl_tab_group_is_member(
					server->grabbed_view))
			{
				hsdwl_tab_group_add_view(
					server->grabbed_view->tab_group,
					server->grab_target);
			}
			else
			{
				hsdwl_tab_group_create(server,
					server->grabbed_view,
					server->grab_target,
					HSDWL_TAB_HORIZONTAL);
			}
		}

		if (server->cursor_mode == HSDWL_CURSOR_RESIZE)
		{
			int fx = server->resize_preview_x;
			int fy = server->resize_preview_y;
			int fw = server->resize_preview_w;
			int fh = server->resize_preview_h;

			if (hsdwl_tab_group_is_member(server->grabbed_view))
			{
				struct hsdwl_tab_group *g =
					server->grabbed_view->tab_group;
				if (g && g->scene_tree)
				{
					fy += g->tab_bar_thickness;
					wlr_scene_node_set_position(
						&g->scene_tree->node, fx, fy);
					g->content_area_box.width = fw;
					g->content_area_box.height = fh;
					struct hsdwl_view *vi;
					wl_list_for_each(vi, &g->views,
							tab_group_link)
						view_configure_size(vi, fw, fh);
				}
			}
			else if (server->grabbed_view)
			{
				wlr_scene_node_set_position(
					&server->grabbed_view->scene_tree->node,
					fx, fy);
				if (server->grabbed_view->xdg_surface)
				{
					wlr_xdg_toplevel_set_size(
						server->grabbed_view
							->xdg_surface->toplevel,
						fw, fh);
				}
				else if (server->grabbed_view
						->xwayland_surface)
				{
					wlr_xwayland_surface_configure(
						server->grabbed_view
							->xwayland_surface,
						fx, fy, fw, fh);
				}
			}

			for (int i = 0; i < 4; i++)
			{
				if (server->resize_preview[i])
				{
					wlr_scene_node_set_enabled(
						&server->resize_preview[i]
							->node, false);
					wlr_scene_node_destroy(
						&server->resize_preview[i]
							->node);
					server->resize_preview[i] = NULL;
				}
			}
		}

		struct hsdwl_tab_group *__tg;
		wl_list_for_each(__tg, &server->tab_groups, link)
			hsdwl_tab_group_update_layout(__tg);

		if (server->config.stage_manager_enabled)
			stage_manager_check_sidebar_overlap(server,
				server->current_workspace);

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

	return true;
}

void pointer_handle_new(struct hsdwl_server *server,
		struct wlr_input_device *device)
{
	wlr_cursor_attach_input_device(server->cursor, device);
}
