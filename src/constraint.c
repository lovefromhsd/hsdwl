#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "constraint.h"
#include "server.h"

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include "view.h"
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>

static void constraint_disconnect_active(struct hsdwl_server *server)
{
	wl_list_remove(&server->constraint_destroy.link);
	wl_list_init(&server->constraint_destroy.link);
	wl_list_remove(&server->constraint_set_region.link);
	wl_list_init(&server->constraint_set_region.link);
}

static struct wlr_pointer_constraint_v1 *
constraint_for_surface(struct hsdwl_server *server,
	struct wlr_surface *surface)
{
	if (!server->pointer_constraints || !surface)
		return NULL;

	/* Prefer the newest constraint for a surface */
	struct wlr_pointer_constraint_v1 *constraint;
	struct wlr_pointer_constraint_v1 *found = NULL;
	wl_list_for_each(constraint,
		&server->pointer_constraints->constraints, link)
	{
		if (constraint->surface == surface)
			found = constraint;
	}
	return found;
}

static void cursor_center_on_surface(struct hsdwl_server *server,
	struct wlr_surface *surface)
{
	double cx = 0, cy = 0;
	bool found = false;

	struct hsdwl_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view_get_surface(view) == surface)
		{
			double vx = view->scene_tree->node.x;
			double vy = view->scene_tree->node.y;
			int cw = 0, ch = 0;

			if (hsdwl_tab_group_is_member(view))
			{
				struct hsdwl_tab_group *g = view->tab_group;
				vx = g->scene_tree->node.x;
				vy = g->scene_tree->node.y;
				cw = g->content_area_box.width;
				ch = g->content_area_box.height;
			}
			else if (view->xdg_surface
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

			if (cw > 0 && ch > 0)
			{
				if (!hsdwl_tab_group_is_member(view))
				{
					int bw = server->config.border_width;
					int tb = server->config.titlebar_height;
					if (tb < 0) tb = 0;
					vx += bw;
					vy += (tb > 0 ? tb : bw);
				}
				cx = vx + cw / 2.0;
				cy = vy + ch / 2.0;
			}
			found = true;
			break;
		}
	}

	if (!found)
	{
		struct wlr_box box;
		wlr_output_layout_get_box(
			server->output_layout, NULL, &box);
		if (box.width > 0 && box.height > 0)
		{
			cx = box.x + box.width / 2.0;
			cy = box.y + box.height / 2.0;
			found = true;
		}
	}

	if (found)
		wlr_cursor_warp(server->cursor, NULL, cx, cy);
}

static void deactivate_constraint(struct hsdwl_server *server)
{
	if (!server->active_constraint)
		return;

	wlr_log(WLR_DEBUG, "pointer constraint deactivated");

	struct wlr_surface *surface = server->active_constraint->surface;

	wlr_pointer_constraint_v1_send_deactivated(
		server->active_constraint);
	server->active_constraint = NULL;
	constraint_disconnect_active(server);

	/* Warp cursor to center of the constrained surface and restore
	 * cursor image hidden by activate_constraint() */
	cursor_center_on_surface(server, surface);
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
		"default");
}

static void handle_constraint_destroy(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, constraint_destroy);
	struct wlr_pointer_constraint_v1 *constraint = data;

	wlr_log(WLR_DEBUG, "pointer constraint destroyed");

	if (server->active_constraint == constraint)
	{
		struct wlr_surface *surface = constraint->surface;
		server->active_constraint = NULL;
		constraint_disconnect_active(server);

		cursor_center_on_surface(server, surface);
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
			"default");
	}
}

static void handle_constraint_set_region(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_server *server = wl_container_of(
		listener, server, constraint_set_region);
	(void)server;
}

static void activate_constraint(struct hsdwl_server *server,
	struct wlr_pointer_constraint_v1 *constraint)
{
	if (server->active_constraint == constraint)
		return;

	/* Deactivate previous constraint */
	if (server->active_constraint)
		wlr_pointer_constraint_v1_send_deactivated(
			server->active_constraint);

	constraint_disconnect_active(server);

	server->active_constraint = constraint;
	wlr_pointer_constraint_v1_send_activated(constraint);

	wlr_log(WLR_DEBUG, "pointer constraint activated: type=%d",
		constraint->type);

	/* For locked constraints: center cursor on the surface
	 * before hiding the hardware cursor */
	if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
	{
		cursor_center_on_surface(server, constraint->surface);
		wlr_cursor_unset_image(server->cursor);
	}

	server->constraint_destroy.notify = handle_constraint_destroy;
	wl_signal_add(&constraint->events.destroy,
		&server->constraint_destroy);
	server->constraint_set_region.notify = handle_constraint_set_region;
	wl_signal_add(&constraint->events.set_region,
		&server->constraint_set_region);
}

static bool surface_has_pointer_focus(struct hsdwl_server *server,
	struct wlr_surface *surface)
{
	return server->seat->pointer_state.focused_surface == surface;
}

static void handle_new_constraint(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, new_constraint);
	struct wlr_pointer_constraint_v1 *constraint = data;

	wlr_log(WLR_DEBUG,
		"new pointer constraint: type=%d surface=%p focused=%d",
		constraint->type,
		(void *)constraint->surface,
		surface_has_pointer_focus(server, constraint->surface));

	/* Activate immediately if surface has pointer focus.
	 * For locked constraints also activate without focus —
	 * the game expects to take over the cursor right when
	 * (re)creating a lock (e.g. closing an inventory). */
	if (surface_has_pointer_focus(server, constraint->surface)
			|| constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
		activate_constraint(server, constraint);
}

void constraint_notify_pointer_focus_change(
	struct hsdwl_server *server, struct wlr_surface *new_surface)
{
	wlr_log(WLR_DEBUG, "pointer focus change: new_surface=%p",
		(void *)new_surface);

	if (new_surface) {
		struct wlr_pointer_constraint_v1 *c =
			constraint_for_surface(server, new_surface);
		if (c)
			activate_constraint(server, c);
		else
			deactivate_constraint(server);
	} else {
		deactivate_constraint(server);
	}
}

void constraint_init(struct hsdwl_server *server)
{
	server->pointer_constraints =
		wlr_pointer_constraints_v1_create(server->display);
	if (!server->pointer_constraints) {
		wlr_log(WLR_ERROR,
			"wlr_pointer_constraints_v1_create failed");
		return;
	}

	server->relative_pointer_manager =
		wlr_relative_pointer_manager_v1_create(server->display);
	if (!server->relative_pointer_manager) {
		wlr_log(WLR_ERROR,
			"wlr_relative_pointer_manager_v1_create failed");
		return;
	}

	server->active_constraint = NULL;

	server->new_constraint.notify = handle_new_constraint;
	wl_signal_add(&server->pointer_constraints->events.new_constraint,
		&server->new_constraint);

	wl_list_init(&server->constraint_destroy.link);
	wl_list_init(&server->constraint_set_region.link);

	wlr_log(WLR_INFO,
		"pointer constraints and relative pointer initialized");
}
