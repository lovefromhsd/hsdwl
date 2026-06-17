#ifndef HSDWL_VIEW_MAXIMIZE_H
#define HSDWL_VIEW_MAXIMIZE_H

#include "view.h"

struct hsdwl_server;
struct wlr_buffer;

void view_maximize(struct hsdwl_server *server, struct hsdwl_view *view);
struct wlr_buffer *view_capture_full_window(struct hsdwl_server *server,
	struct hsdwl_view *view, int content_w, int content_h,
	int bw, int tb);
struct wlr_buffer *view_capture_content_only(struct hsdwl_server *server,
	struct hsdwl_view *view, int target_w, int target_h);

#endif
