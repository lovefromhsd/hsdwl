#ifndef HSDWL_STAGE_3D_H
#define HSDWL_STAGE_3D_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include "animation.h"


struct hsdwl_server;
struct wlr_render_pass;
struct wlr_texture;
struct wlr_scene_buffer;

void stage_3d_render_tilted(
	struct wlr_render_pass *pass,
	struct wlr_texture *texture,
	int tex_w, int tex_h,
	int dst_x, int dst_y,
	int dst_w, int dst_h,
	float z_offset,
	float angle_deg,
	float alpha,
	float focal_length);

struct hsdwl_tilt_state {

	struct wl_list link;

	struct timespec start;

	int duration_ms;

	struct wlr_texture *tex;

	int tex_w, tex_h;

	struct wlr_scene_buffer *overlay;

	float start_angle, end_angle;

	float start_z, end_z;

	enum hsdwl_easing easing;

	float focal_length;

	void (*on_finish)(struct hsdwl_server *, void *);

	void *user_data;

};


struct hsdwl_tilt_state *stage_3d_start_tilt_anim(
	struct hsdwl_server *server,
	struct wlr_texture *tex, int tex_w, int tex_h,
	struct wlr_scene_buffer *overlay,
	int duration_ms,
	enum hsdwl_easing easing,
	float start_angle, float end_angle,
	float start_z, float end_z,
	float focal_length,
	void (*on_finish)(struct hsdwl_server *, void *),
	void *user_data);

void stage_3d_tick(struct hsdwl_server *server, struct timespec *now);
void stage_3d_cancel(struct hsdwl_server *server);

#endif
