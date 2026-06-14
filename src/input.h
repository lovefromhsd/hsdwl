#ifndef HSDWL_INPUT_H
#define HSDWL_INPUT_H

#include <wayland-server-core.h>

struct hsdwl_server;

struct hsdwl_keyboard
{
	struct wl_list link;
	struct hsdwl_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

void input_handle_new(struct wl_listener *listener, void *data);

#endif
