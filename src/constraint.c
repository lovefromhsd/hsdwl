#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "constraint.h"
#include "server.h"

#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

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

static void deactivate_constraint(struct hsdwl_server *server)
{
	if (!server->active_constraint)
		return;

	wlr_log(WLR_DEBUG, "pointer constraint deactivated");

	wlr_pointer_constraint_v1_send_deactivated(
		server->active_constraint);
	server->active_constraint = NULL;
	constraint_disconnect_active(server);
}

static void handle_constraint_destroy(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, constraint_destroy);
	struct wlr_pointer_constraint_v1 *constraint = data;

	wlr_log(WLR_DEBUG, "pointer constraint destroyed");

	if (server->active_constraint == constraint)
	{
		server->active_constraint = NULL;
		constraint_disconnect_active(server);
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

	/* If the constrained surface already has pointer focus,
	 * activate the constraint immediately. */
	if (surface_has_pointer_focus(server, constraint->surface))
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
