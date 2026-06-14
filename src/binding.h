#ifndef HSDWL_BINDING_H
#define HSDWL_BINDING_H

#include <stdbool.h>

struct hsdwl_server;
struct wlr_keyboard;
struct wlr_keyboard_key_event;

bool binding_dispatch(struct hsdwl_server *server,
	struct wlr_keyboard *kb, struct wlr_keyboard_key_event *event);

#endif
