#ifndef HSDWL_STAGE_3D_H
#define HSDWL_STAGE_3D_H

#include <stdbool.h>
#include <time.h>
#include <wayland-server-core.h>

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
	float tilt_dir);

struct hsdwl_tilt_state {

	struct wl_list link;

	struct timespec start;

	int duration_ms;

	struct wlr_texture *tex;

	int tex_w, tex_h;

	struct wlr_scene_buffer *overlay;

	float start_angle, end_angle;

	float start_z, end_z;

	float start_tilt_dir, end_tilt_dir;

	void (*on_finish)(struct hsdwl_server *, void *);

	void *user_data;

};



struct hsdwl_tilt_state *stage_3d_start_tilt_anim(

	struct hsdwl_server *server,

	struct wlr_texture *tex, int tex_w, int tex_h,

	struct wlr_scene_buffer *overlay,

	int duration_ms,

	float start_angle, float end_angle,

	float start_z, float end_z,

	float start_tilt_dir, float end_tilt_dir,

	void (*on_finish)(struct hsdwl_server *, void *),

	void *user_data);


	struct hsdwl_flip_state {
	struct wl_list link;
	struct timespec start;
	int duration_ms;
	struct wlr_texture *out_tex;
	struct wlr_texture *in_tex;
	int out_w, out_h;
	int in_w, in_h;
	int out_x, out_y;
	int in_x, in_y;
	struct wlr_scene_buffer *out_overlay;
	struct wlr_scene_buffer *in_overlay;
	float tilt_angle;
	float z_offset;
	void (*on_finish)(struct hsdwl_server *server, void *user_data);
	void *user_data;
};

struct hsdwl_flip_state *stage_3d_start_flip(
	struct hsdwl_server *server,
	struct wlr_texture *out_tex, int out_w, int out_h,
	int out_x, int out_y,
	struct wlr_texture *in_tex, int in_w, int in_h,
	int in_x, int in_y,
	int duration_ms, float tilt_angle,
	float z_offset,
	void (*on_finish)(struct hsdwl_server *, void *),
	void *user_data);

void stage_3d_tick(struct hsdwl_server *server, struct timespec *now);
void stage_3d_cancel(struct hsdwl_server *server);

#endif
