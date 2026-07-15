#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "stage-3d.h"
#include "server.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#define STRIPS 192

void stage_3d_render_tilted(
	struct wlr_render_pass *pass,
	struct wlr_texture *texture,
	int tex_w, int tex_h,
	int dst_x, int dst_y,
	int dst_w, int dst_h,
	float z_offset,
	float angle_deg,
	float alpha,
	float focal_length)
{
	if (tex_w < 1 || tex_h < 1 || dst_w < 1 || dst_h < 1)
		return;

	float rad = angle_deg * (float)(M_PI / 180.0);
	float cos_a = cosf(rad);
	float sin_a = sinf(rad);
	float f = focal_length;
	if (f < 1.0f) f = 1.0f;

	float src_strip_w = (float)tex_w / (float)STRIPS;

	int start, end, step;
	if (sin_a >= 0.0f) {
		start = STRIPS - 1; end = -1; step = -1;
	} else {
		start = 0; end = STRIPS; step = 1;
	}

	float max_strip_h = 0.0f;
	for (int i = 0; i < STRIPS; i++) {
		float pc = ((float)i + 0.5f) / (float)STRIPS;
		float xc = (pc - 0.5f) * (float)tex_w;
		float zc = -xc * sin_a + z_offset;
		float wc = f / (f + zc);
		if (wc < 0.01f) wc = 0.01f;
		float sh = (float)tex_h * wc;
		if (sh > max_strip_h) max_strip_h = sh;
	}

	float vscale = 1.0f;
	if (max_strip_h > (float)dst_h && max_strip_h > 0.0f) {
		vscale = (float)dst_h / max_strip_h;
	}

	for (int idx = start; idx != end; idx += step)
	{
		float p_left = (float)idx / (float)STRIPS;
		float p_right = (float)(idx + 1) / (float)STRIPS;

		float x_left = (p_left - 0.5f) * (float)tex_w;
		float x_right = (p_right - 0.5f) * (float)tex_w;

		float xl_rot = x_left * cos_a;
		float zl_rot = -x_left * sin_a + z_offset;
		float xr_rot = x_right * cos_a;
		float zr_rot = -x_right * sin_a + z_offset;

		float wl = f / (f + zl_rot);
		float wr = f / (f + zr_rot);
		if (wl < 0.01f) wl = 0.01f;
		if (wr < 0.01f) wr = 0.01f;

		float screen_left = (float)dst_w / 2.0f + xl_rot * wl * vscale;
		float screen_right = (float)dst_w / 2.0f + xr_rot * wr * vscale;

		int bx = dst_x + (int)(screen_left + 0.5f);
		int bw = (int)(screen_right + 0.5f) - bx;
		if (bw < 1) bw = 1;

		float pc = (p_left + p_right) / 2.0f;
		float xc = (pc - 0.5f) * (float)tex_w;
		float zc = -xc * sin_a + z_offset;
		float wc = f / (f + zc);
		if (wc < 0.01f) wc = 0.01f;

		float strip_h = (float)tex_h * wc * vscale;
		if (strip_h < 1.0f) strip_h = 1.0f;

		float strip_y = ((float)dst_h - strip_h) * 0.5f;

		float src_x = p_left * (float)tex_w;
		float src_w = src_strip_w;

		wlr_render_pass_add_texture(pass,
			&(struct wlr_render_texture_options){
				.texture = texture,
				.src_box = {
					.x = src_x,
					.y = 0,
					.width = src_w,
					.height = (float)tex_h,
				},
				.dst_box = {
					.x = bx,
					.y = dst_y + (int)(strip_y + 0.5f),
					.width = bw,
					.height = (int)(strip_h + 0.5f),
				},
				.alpha = &alpha,
				.transform = WL_OUTPUT_TRANSFORM_NORMAL,
			});
	}
}

static struct wlr_buffer *render_tilt_frame(

	struct hsdwl_server *server,

	struct wlr_texture *tex,

	int tex_w, int tex_h,

	int dst_w, int dst_h,

	float z_offset,

	float angle_deg,

	float focal_length)

{

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };

	struct wlr_drm_format fmt = {

		.format = DRM_FORMAT_ARGB8888,

		.len = 1,

		.modifiers = mods,

	};


	struct wlr_buffer *buf = wlr_allocator_create_buffer(

		server->allocator, dst_w, dst_h, &fmt);

	if (!buf) return NULL;


	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(

		server->renderer, buf, NULL);

	if (!pass) {

		wlr_buffer_drop(buf);

		return NULL;

	}


	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){

		.box = { .width = dst_w, .height = dst_h },

		.color = { 0.0f, 0.0f, 0.0f, 0.0f },

	});


	stage_3d_render_tilted(pass, tex, tex_w, tex_h,

		0, 0, dst_w, dst_h,

		z_offset, angle_deg, 1.0f, focal_length);


	if (!wlr_render_pass_submit(pass)) {

		wlr_buffer_drop(buf);

		return NULL;

	}

	return buf;

}


static void tilt_cleanup(struct hsdwl_server *server, struct hsdwl_tilt_state *ts)
{
	wl_list_remove(&ts->link);
	if (ts->tex)
		wlr_texture_destroy(ts->tex);
	if (ts->on_finish)
		ts->on_finish(server, ts->user_data);
	free(ts);
}

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
	void *user_data)
{
	struct hsdwl_tilt_state *ts = calloc(1, sizeof(*ts));
	if (!ts) return NULL;

	ts->duration_ms = duration_ms;
	ts->tex = tex;
	ts->tex_w = tex_w;
	ts->tex_h = tex_h;
	ts->overlay = overlay;
	ts->easing = easing;
	ts->start_angle = start_angle;
	ts->end_angle = end_angle;
	ts->start_z = start_z;
	ts->end_z = end_z;
	ts->focal_length = focal_length;
	ts->on_finish = on_finish;
	ts->user_data = user_data;

	clock_gettime(CLOCK_MONOTONIC, &ts->start);
	wl_list_insert(&server->tilt_animations, &ts->link);

	return ts;
}

void stage_3d_tick(struct hsdwl_server *server, struct timespec *now)
{
	struct hsdwl_tilt_state *ts, *tmp_ts;
	wl_list_for_each_safe(ts, tmp_ts, &server->tilt_animations, link) {
		double elapsed_ms =
			(double)(now->tv_sec - ts->start.tv_sec) * 1000.0 +
			(double)(now->tv_nsec - ts->start.tv_nsec) / 1000000.0;

		double t = elapsed_ms / (double)ts->duration_ms;
		if (t < 0.0) t = 0.0;
		if (t > 1.0) t = 1.0;

		double eased_t;
		switch (ts->easing) {
		case HSDWL_EASE_LINEAR:
			eased_t = t;
			break;
		case HSDWL_EASE_OUT_QUAD:
			eased_t = t * (2.0 - t);
			break;
		case HSDWL_EASE_OUT_CUBIC: {
			double t1 = t - 1.0;
			eased_t = t1 * t1 * t1 + 1.0;
			break;
		}
		case HSDWL_EASE_BEZIER:
			eased_t = ease_bezier(t,
				server->config.anim_bezier_x1,
				server->config.anim_bezier_y1,
				server->config.anim_bezier_x2,
				server->config.anim_bezier_y2);
			break;
		}

		float angle = ts->start_angle + (ts->end_angle - ts->start_angle) * (float)eased_t;
		float z = ts->start_z + (ts->end_z - ts->start_z) * (float)eased_t;

		struct wlr_buffer *buf = render_tilt_frame(
			server, ts->tex,
			ts->tex_w, ts->tex_h,
			ts->tex_w, ts->tex_h,
			z, angle, ts->focal_length);
		if (buf) {
			wlr_scene_buffer_set_buffer(ts->overlay, buf);
			wlr_buffer_drop(buf);
		}

		if (t >= 1.0) {
			tilt_cleanup(server, ts);
		}
	}
}

void stage_3d_cancel(struct hsdwl_server *server)
{
	struct hsdwl_tilt_state *ts, *tmp_ts;
	wl_list_for_each_safe(ts, tmp_ts, &server->tilt_animations, link) {
		tilt_cleanup(server, ts);
	}
}
