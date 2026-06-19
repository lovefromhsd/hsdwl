#ifndef HSDWL_VIEW_CAPTURE_H
#define HSDWL_VIEW_CAPTURE_H

#include <stdbool.h>

struct hsdwl_server;
struct hsdwl_view;
struct wlr_buffer;
struct wlr_scene_buffer;

struct wlr_buffer *view_capture_content_only(
	struct hsdwl_server *server,
	struct hsdwl_view *view,
	int target_w, int target_h);

struct wlr_buffer *view_capture_full_window(
	struct hsdwl_server *server,
	struct hsdwl_view *view,
	int content_w, int content_h,
	int bw, int tb);

struct wlr_scene_buffer *create_window_overlay(
	struct hsdwl_server *server,
	struct hsdwl_view *view,
	int content_w, int content_h,
	int bw, int tb,
	int abs_x, int abs_y);

void destroy_anim_overlay(struct hsdwl_server *server,
	struct hsdwl_view *view);

#endif
