#ifndef HSDWL_OUTPUT_H
#define HSDWL_OUTPUT_H

#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct hsdwl_server;

struct hsdwl_output
{
	struct wl_list link;
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wlr_output *wlr_output;
	struct hsdwl_server *server;
	struct wlr_box work_area;
};

void output_handle_new(struct wl_listener *listener, void *data);

#endif
