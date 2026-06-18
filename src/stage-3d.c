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

#define STRIPS 48

void stage_3d_render_tilted(
	struct wlr_render_pass *pass,
	struct wlr_texture *texture,
	int tex_w, int tex_h,
	int dst_x, int dst_y,
	int dst_w, int dst_h,
	float z_offset,
	float angle_deg,
	float alpha,
	float tilt_dir)
{
	if (tex_w < 1 || tex_h < 1 || dst_w < 1 || dst_h < 1)
		return;

	float rad = angle_deg * (float)(M_PI / 180.0);
	float cos_a = cosf(rad);
	float sin_a = sinf(rad);
	float focal = (float)tex_w * 1.5f;

	float max_persp = focal / (focal - (float)tex_w / 2.0f * fabsf(sin_a) + z_offset);
	if (max_persp < 1.0f) max_persp = 1.0f;
	float min_persp = focal / (focal + (float)tex_w / 2.0f * fabsf(sin_a) + z_offset);
	if (min_persp > 1.0f) min_persp = 1.0f;

	float ramp_amp = (float)dst_h * 0.18f;
	float total_span = ((max_persp + min_persp) * 0.5f) * (float)tex_h + ramp_amp;
	float vscale = 1.0f;
	if (total_span > (float)dst_h && total_span > 0.0f) {
		vscale = (float)dst_h / total_span * 0.97f;
	}
	ramp_amp *= vscale;

	float near_h = (float)tex_h * max_persp * vscale;
	float near_top = (float)dst_h * 0.5f - near_h * 0.5f - ramp_amp * 0.5f;
	float far_h = (float)tex_h * min_persp * vscale;
	float far_bot = (float)dst_h * 0.5f + far_h * 0.5f + ramp_amp * 0.5f;
	float global_shift = ((float)dst_h - (near_top + far_bot)) * 0.5f;

	for (int i = 0; i < STRIPS; i++)
	{
		float p0 = (float)i / (float)STRIPS;
		float p1 = (float)(i + 1) / (float)STRIPS;
		float pc = (p0 + p1) * 0.5f;

		float xc = (pc - 0.5f) * (float)tex_w;

		float zc = xc * sin_a + z_offset;

		float perspc = focal / (focal + zc);
		if (perspc < 0.05f) perspc = 0.05f;

		float x0 = (p0 - 0.5f) * (float)tex_w;
		float x1 = (p1 - 0.5f) * (float)tex_w;

		float z0 = x0 * sin_a + z_offset;
		float z1 = x1 * sin_a + z_offset;

		float x0p = x0 * cos_a;
		float x1p = x1 * cos_a;

		float sx0 = x0p * (focal / (focal + z0));
		float sx1 = x1p * (focal / (focal + z1));

		float strip_x = (float)dst_w / 2.0f + sx0 * vscale;
		float strip_w = (sx1 - sx0) * vscale;

		if (strip_w < 1.0f) strip_w = 1.0f;

		float strip_h = (float)tex_h * perspc * vscale;
		float ramp = -(pc - 0.5f) * ramp_amp * tilt_dir;
		float strip_y = (float)dst_h * 0.5f - strip_h * 0.5f + ramp + global_shift;

		float src_x = p0 * (float)tex_w;
		float src_w = (p1 - p0) * (float)tex_w;

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
					.x = dst_x + (int)(strip_x + 0.5f),
					.y = dst_y + (int)(strip_y + 0.5f),
					.width = (int)(strip_w + 1.5f),
					.height = (int)(strip_h + 0.5f),
				},
				.alpha = &alpha,
				.transform = WL_OUTPUT_TRANSFORM_NORMAL,
			});
	}
}

static struct wlr_buffer *render_flip_frame(
	struct hsdwl_server *server,
	struct wlr_texture *tex,
	int tex_w, int tex_h,
	int dst_w, int dst_h,
	float z_offset,
	float angle_deg)
{
	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format fmt = {
		.format = DRM_FORMAT_ARGB8888,
		.len = 1,
		.modifiers = mods,
	};

	struct wlr_buffer *buf = wlr_allocator_create_buffer(
		server->allocator, dst_w, dst_h, &fmt);
	if (!buf)
		return NULL;

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
		z_offset,
		angle_deg, 1.0f, 0.0f);

	if (!wlr_render_pass_submit(pass)) {
		wlr_buffer_drop(buf);
		return NULL;
	}

	return buf;
}

struct hsdwl_flip_state *stage_3d_start_flip(
	struct hsdwl_server *server,
	struct wlr_texture *out_tex, int out_w, int out_h,
	int out_x, int out_y,
	struct wlr_texture *in_tex, int in_w, int in_h,
	int in_x, int in_y,
	int duration_ms, float tilt_angle,
	float z_offset,
	void (*on_finish)(struct hsdwl_server *, void *),
	void *user_data)
{
	struct hsdwl_flip_state *fs = calloc(1, sizeof(*fs));
	if (!fs)
		return NULL;

	fs->out_tex = out_tex;
	fs->out_w = out_w;
	fs->out_h = out_h;
	fs->out_x = out_x;
	fs->out_y = out_y;
	fs->in_tex = in_tex;
	fs->in_w = in_w;
	fs->in_h = in_h;
	fs->in_x = in_x;
	fs->in_y = in_y;
	fs->duration_ms = duration_ms;
	fs->tilt_angle = tilt_angle;
	fs->z_offset = z_offset;
	fs->on_finish = on_finish;
	fs->user_data = user_data;

	fs->out_overlay = wlr_scene_buffer_create(
		server->animation_tree, NULL);
	fs->in_overlay = wlr_scene_buffer_create(
		server->animation_tree, NULL);

	if (!fs->out_overlay || !fs->in_overlay) {
		if (fs->out_overlay)
			wlr_scene_node_destroy(&fs->out_overlay->node);
		if (fs->in_overlay)
			wlr_scene_node_destroy(&fs->in_overlay->node);
		free(fs);
		return NULL;
	}

	wlr_scene_node_raise_to_top(&fs->out_overlay->node);
	wlr_scene_node_raise_to_top(&fs->in_overlay->node);

	clock_gettime(CLOCK_MONOTONIC, &fs->start);
	wl_list_insert(&server->flip_animations, &fs->link);
	return fs;
}

static void flip_cleanup(struct hsdwl_server *server,
	struct hsdwl_flip_state *fs)
{
	if (fs->out_overlay)
		wlr_scene_node_destroy(&fs->out_overlay->node);
	if (fs->in_overlay)
		wlr_scene_node_destroy(&fs->in_overlay->node);
	if (fs->out_tex)
		wlr_texture_destroy(fs->out_tex);
	if (fs->in_tex)
		wlr_texture_destroy(fs->in_tex);
	wl_list_remove(&fs->link);
	if (fs->on_finish)
		fs->on_finish(server, fs->user_data);
	free(fs);
}

void stage_3d_tick(struct hsdwl_server *server, struct timespec *now)
{
	struct hsdwl_flip_state *fs, *tmp;
	wl_list_for_each_safe(fs, tmp, &server->flip_animations, link)
	{
		double elapsed_ms =
			(double)(now->tv_sec - fs->start.tv_sec) * 1000.0 +
			(double)(now->tv_nsec - fs->start.tv_nsec) / 1000000.0;

		double t = elapsed_ms / (double)fs->duration_ms;
		if (t < 0.0) t = 0.0;
		if (t > 1.0) t = 1.0;

		double out_angle = -90.0 + t * 90.0;
		double in_angle = 90.0 - t * 90.0;
		float out_opacity = (float)(1.0 - t);
		float in_opacity = (float)t;

		bool out_hide = (out_angle <= -89.0 || out_angle >= 89.0);
		bool in_hide = (in_angle <= -89.0 || in_angle >= 89.0);

		if (fs->out_tex && !out_hide) {
			struct wlr_buffer *buf = render_flip_frame(
				server, fs->out_tex,
				fs->out_w, fs->out_h,
				fs->out_w, fs->out_h,
				fs->z_offset,
				(float)out_angle);
			if (buf) {
				wlr_scene_buffer_set_buffer(
					fs->out_overlay, buf);
				wlr_scene_node_set_position(
					&fs->out_overlay->node,
					fs->out_x, fs->out_y);
				wlr_scene_buffer_set_dest_size(
					fs->out_overlay,
					fs->out_w, fs->out_h);
				wlr_scene_buffer_set_opacity(
					fs->out_overlay, out_opacity);
				wlr_buffer_drop(buf);
			}
		} else if (fs->out_overlay) {
			wlr_scene_node_set_enabled(
				&fs->out_overlay->node, false);
		}

		if (fs->in_tex && !in_hide) {
			struct wlr_buffer *buf = render_flip_frame(
				server, fs->in_tex,
				fs->in_w, fs->in_h,
				fs->in_w, fs->in_h,
				fs->z_offset,
				(float)in_angle);
			if (buf) {
				wlr_scene_buffer_set_buffer(
					fs->in_overlay, buf);
				wlr_scene_node_set_position(
					&fs->in_overlay->node,
					fs->in_x, fs->in_y);
				wlr_scene_buffer_set_dest_size(
					fs->in_overlay,
					fs->in_w, fs->in_h);
				wlr_scene_buffer_set_opacity(
					fs->in_overlay, in_opacity);
				wlr_buffer_drop(buf);
			}
		} else if (fs->in_overlay) {
			wlr_scene_node_set_enabled(
				&fs->in_overlay->node, false);
		}

		if (t >= 1.0) {
			flip_cleanup(server, fs);
			return;
		}
	}
}

void stage_3d_cancel(struct hsdwl_server *server)
{
	struct hsdwl_flip_state *fs, *tmp;
	wl_list_for_each_safe(fs, tmp, &server->flip_animations, link)
	{
		flip_cleanup(server, fs);
	}
}
