#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "binding.h"
#include "input.h"
#include "pointer.h"
#include "server.h"
#include "view.h"

#include <stdlib.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_keyboard *keyboard = wl_container_of(
		listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data)
{
	struct hsdwl_keyboard *keyboard = wl_container_of(
		listener, keyboard, key);
	struct hsdwl_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_keyboard *wlr_keyboard = keyboard->wlr_keyboard;
	struct wlr_seat *seat = server->seat;

	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
	{
		if (binding_dispatch(server, wlr_keyboard, event))
			return;
	}

	wlr_seat_set_keyboard(seat, wlr_keyboard);
	wlr_seat_keyboard_notify_key(seat,
		event->time_msec, event->keycode, event->state);
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_keyboard *keyboard = wl_container_of(
		listener, keyboard, destroy);
	wl_list_remove(&keyboard->link);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	free(keyboard);
}

static void input_keyboard_create(struct hsdwl_server *server,
		struct wlr_input_device *device)
{
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct hsdwl_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (!keyboard)
		return;

	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_rule_names rules = {
		.layout = server->config.kb_layout[0]
			? server->config.kb_layout : NULL,
	};
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context)
	{
		free(keyboard);
		return;
	}
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(
		context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap)
	{
		xkb_context_unref(context);
		free(keyboard);
		return;
	}
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard,
		server->config.keyboard_repeat_rate,
		server->config.keyboard_repeat_delay);

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wl_list_insert(&server->keyboards, &keyboard->link);

	wlr_seat_set_keyboard(server->seat, wlr_keyboard);
	wlr_seat_set_capabilities(server->seat,
		WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);
}

void input_handle_new(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type)
	{
	case WLR_INPUT_DEVICE_KEYBOARD:
		input_keyboard_create(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		pointer_handle_new(server, device);
		break;
	default:
		break;
	}
}
