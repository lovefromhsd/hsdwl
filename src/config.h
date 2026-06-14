#ifndef HSDWL_CONFIG_H
#define HSDWL_CONFIG_H

#include <stdbool.h>
#include <wayland-server-core.h>

enum hsdwl_action
{
	HSDWL_ACTION_NONE,
	HSDWL_ACTION_SWITCH_WORKSPACE,
	HSDWL_ACTION_MOVE_TO_WORKSPACE,
	HSDWL_ACTION_CYCLE_FOCUS,
	HSDWL_ACTION_CYCLE_FOCUS_REVERSE,
	HSDWL_ACTION_SPAWN,
	HSDWL_ACTION_QUIT,
};

struct hsdwl_binding
{
	struct wl_list link;
	char mods[128];
	enum hsdwl_action action;
	int arg;
	char command[1024];
};

struct hsdwl_config
{
	int cursor_size;
	int keyboard_repeat_rate;
	int keyboard_repeat_delay;
	int edge_threshold;
	int min_window_size;
	char mod_key[32];
	struct wl_list bindings;
};

bool hsdwl_config_load(struct hsdwl_config *cfg);
void hsdwl_config_finish(struct hsdwl_config *cfg);

#endif
