#ifndef HSDWL_CONFIG_H
#define HSDWL_CONFIG_H

#include <stdbool.h>

struct hsdwl_config
{
	char terminal[256];
	int cursor_size;
	int keyboard_repeat_rate;
	int keyboard_repeat_delay;
	int edge_threshold;
	int min_window_size;
	char mod_key[32];
};

bool hsdwl_config_load(struct hsdwl_config *cfg);

#endif
