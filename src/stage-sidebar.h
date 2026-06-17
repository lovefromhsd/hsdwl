#ifndef HSDWL_STAGE_SIDEBAR_H
#define HSDWL_STAGE_SIDEBAR_H

#include <stdbool.h>
#include <stddef.h>

struct hsdwl_server;
struct hsdwl_view;
struct custom_stage;

void stage_hide_thumb(struct custom_stage *stage, bool hidden);
void stage_render_thumbnail(struct hsdwl_server *server,
	struct custom_stage *stage, int thumb_w, int thumb_h);
const char *view_get_app_name(struct hsdwl_view *view);
const char *stage_get_app_name(struct custom_stage *stage);
void stage_manager_render_sidebar(struct hsdwl_server *server, size_t ws);

#endif
