#ifndef HSDWL_POINTER_H
#define HSDWL_POINTER_H

#include <wayland-server-core.h>

struct hsdwl_server;
struct wlr_input_device;

bool pointer_init(struct hsdwl_server *server);
void pointer_handle_new(struct hsdwl_server *server,
	struct wlr_input_device *device);

#endif
