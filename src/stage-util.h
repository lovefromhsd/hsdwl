#ifndef HSDWL_STAGE_UTIL_H
#define HSDWL_STAGE_UTIL_H

#include <stdbool.h>
#include <stddef.h>

struct hsdwl_server;
struct custom_stage;
struct custom_window;
struct wlr_box;
struct wlr_scene_node;

void stage_focus_first(struct custom_stage *stage, struct hsdwl_server *server);


bool stage_focus_next(struct custom_stage *stage,
		struct hsdwl_server *server, bool reverse);

bool stage_compute_bbox(struct custom_stage *stage, struct wlr_box *out_bbox);

bool stage_thumb_init(struct custom_stage *stage, struct hsdwl_server *server, size_t ws);

struct wlr_scene_node *stage_window_node(struct custom_window *cw);

int output_get_width(struct hsdwl_server *server);

int output_get_height(struct hsdwl_server *server);

#endif
