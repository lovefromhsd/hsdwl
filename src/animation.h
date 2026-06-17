#ifndef HSDWL_ANIMATION_H
#define HSDWL_ANIMATION_H

#include <time.h>
#include <wayland-server-core.h>

struct hsdwl_server;
struct wlr_scene_buffer;
struct wlr_scene_node;

enum hsdwl_easing
{
	HSDWL_EASE_LINEAR,
	HSDWL_EASE_OUT_QUAD,
	HSDWL_EASE_OUT_CUBIC,
};

struct hsdwl_animation
{
	struct wl_list link;
	struct timespec start;
	int duration_ms;
	enum hsdwl_easing easing;

	struct wlr_scene_buffer *buffer;
	struct wlr_scene_node *pos_node;

	double from_x, from_y;
	double to_x, to_y;
	int from_w, from_h;
	int to_w, to_h;

	float from_opacity;
	float to_opacity;

	void (*on_finish)(struct hsdwl_server *server, void *user_data);
	void *user_data;
};

void animation_create(struct hsdwl_server *server,
	struct wlr_scene_buffer *buffer,
	int duration_ms, enum hsdwl_easing easing,
	double from_x, double from_y, int from_w, int from_h,
	double to_x, double to_y, int to_w, int to_h,
	void (*on_finish)(struct hsdwl_server *, void *),
	void *user_data);

void animation_create_with_fade(struct hsdwl_server *server,
	struct wlr_scene_buffer *buffer,
	int duration_ms, enum hsdwl_easing easing,
	double from_x, double from_y, int from_w, int from_h,
	double to_x, double to_y, int to_w, int to_h,
	float from_opacity, float to_opacity,
	void (*on_finish)(struct hsdwl_server *, void *),
	void *user_data);

void animation_tick(struct hsdwl_server *server, struct timespec *now);

void animation_create_node_pos(struct hsdwl_server *server,
	struct wlr_scene_node *node,
	int duration_ms, enum hsdwl_easing easing,
	double from_x, double from_y,
	double to_x, double to_y,
	void (*on_finish)(struct hsdwl_server *, void *),
	void *user_data);

void animation_create_node_pos_with_fade(
	struct hsdwl_server *server,
	struct wlr_scene_node *node,
	int duration_ms, enum hsdwl_easing easing,
	double from_x, double from_y,
	double to_x, double to_y,
	float from_opacity, float to_opacity,
	void (*on_finish)(struct hsdwl_server *, void *),
	void *user_data);

void animation_cancel_all(struct hsdwl_server *server);

void animation_cancel_buffer(struct hsdwl_server *server,
	struct wlr_scene_buffer *buffer);

#endif
