#ifndef HSDWL_CONFIG_H
#define HSDWL_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
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
	HSDWL_ACTION_CLOSE_FOCUSED,
	HSDWL_ACTION_MAXIMIZE,
	HSDWL_ACTION_CYCLE_TAB_NEXT,
	HSDWL_ACTION_CYCLE_TAB_PREV,
};

struct hsdwl_binding
{
	struct wl_list link;
	char mods[128];
	enum hsdwl_action action;
	uint32_t keysym;
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
	int border_width;
	float border_color[4];
	float border_color_focused[4];
	int titlebar_height;
	int titlebar_radius;
	float titlebar_color[4];
	float titlebar_color_focused[4];
	char title_font[128];
	int title_font_size;
	char title_font_weight[64];
	float title_text_color[4];
	float title_text_color_focused[4];
	float preview_color[4];
	float group_overlap_threshold;
	char mod_key[32];
	char kb_layout[128];
	float anim_bezier_x1;
	float anim_bezier_y1;
	float anim_bezier_x2;
	float anim_bezier_y2;
	bool smart_gaps;
	bool stage_manager_enabled;
	bool stage_3d_flip_enabled;
	struct wl_list bindings;
};

bool hsdwl_config_load(struct hsdwl_config *cfg);
void hsdwl_config_finish(struct hsdwl_config *cfg);

#endif
