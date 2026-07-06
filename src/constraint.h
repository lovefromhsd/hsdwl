#ifndef HSDWL_CONSTRAINT_H
#define HSDWL_CONSTRAINT_H

#include <stdbool.h>

struct hsdwl_server;
struct wlr_surface;

void constraint_init(struct hsdwl_server *server);
void constraint_notify_pointer_focus_change(struct hsdwl_server *server,
	struct wlr_surface *new_surface);

#endif
