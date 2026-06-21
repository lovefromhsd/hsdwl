#include "view-capture.h"
#include "server.h"
#include "view.h"
#include "deco.h"
#include <drm_fourcc.h>
#include <math.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>


static void render_texture_fitted(struct wlr_render_pass *pass,
	struct wlr_texture *texture,
	int surf_w, int surf_h,
	int area_w, int area_h,
	int offset_x, int offset_y)
{
	if (surf_w < 1 || surf_h < 1)
	{
		surf_w = area_w;
		surf_h = area_h;
	}
	float scale = fmin((float)area_w / surf_w,
		(float)area_h / surf_h);
	float fit_w = surf_w * scale;
	float fit_h = surf_h * scale;
	float fit_x = offset_x + ((float)area_w - fit_w) / 2.0f;
	float fit_y = offset_y + ((float)area_h - fit_h) / 2.0f;

	const float alpha = 1.0f;
	wlr_render_pass_add_texture(pass,
		&(struct wlr_render_texture_options){
			.texture = texture,
			.dst_box = {
				.x = (int)(fit_x + 0.5f),
				.y = (int)(fit_y + 0.5f),
				.width = (int)(fit_w + 0.5f),
				.height = (int)(fit_h + 0.5f),
			},
			.alpha = &alpha,
			.transform = WL_OUTPUT_TRANSFORM_NORMAL,
		});
}


struct wlr_buffer *view_capture_content_only(
	struct hsdwl_server *server,
	struct hsdwl_view *view,
	int target_w, int target_h)
{
	struct wlr_surface *surface = NULL;
	if (view->xdg_surface)
		surface = view->xdg_surface->surface;
	else if (view->xwayland_surface)
		surface = view->xwayland_surface->surface;
	struct wlr_texture *texture = surface
		? wlr_surface_get_texture(surface) : NULL;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format fmt = {
		.format = DRM_FORMAT_ARGB8888,
		.len = 1,
		.modifiers = mods,
	};
	struct wlr_buffer *buf = wlr_allocator_create_buffer(
		server->allocator, target_w, target_h, &fmt);
	if (!buf)
		return NULL;

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, buf, NULL);
	if (!pass)
	{
		wlr_buffer_drop(buf);
		return NULL;
	}

	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .width = target_w, .height = target_h },
		.color = { 0.0f, 0.0f, 0.0f, 0.0f },
	});

	if (texture)
	{
		float tex_w = surface->current.width;
		float tex_h = surface->current.height;
		render_texture_fitted(pass, texture,
			tex_w, tex_h, target_w, target_h, 0, 0);
	}

	if (!wlr_render_pass_submit(pass))
	{
		wlr_buffer_drop(buf);
		return NULL;
	}

	return buf;
}


struct wlr_buffer *view_capture_full_window(
	struct hsdwl_server *server,
	struct hsdwl_view *view,
	int content_w, int content_h,
	int bw, int tb)
{
	int win_w = content_w + 2 * bw;
	int win_h = content_h + tb + bw;
	if (win_w < 1) win_w = 1;
	if (win_h < 1) win_h = 1;

	struct wlr_surface *surface = NULL;
	if (view->xdg_surface)
		surface = view->xdg_surface->surface;
	else if (view->xwayland_surface)
		surface = view->xwayland_surface->surface;
	struct wlr_texture *texture = surface
		? wlr_surface_get_texture(surface) : NULL;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format fmt = {
		.format = DRM_FORMAT_ARGB8888,
		.len = 1,
		.modifiers = mods,
	};
	struct wlr_buffer *buf = wlr_allocator_create_buffer(
		server->allocator, win_w, win_h, &fmt);
	if (!buf)
		return NULL;

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, buf, NULL);
	if (!pass)
	{
		wlr_buffer_drop(buf);
		return NULL;
	}

	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .width = win_w, .height = win_h },
		.color = { 0.0f, 0.0f, 0.0f, 0.0f },
	});

	bool focused = (view == server->focused_views[
		server->current_workspace]);
	float *bcol = focused
		? server->config.border_color_focused
		: server->config.border_color;
	float *tcol = focused
		? server->config.titlebar_color_focused
		: server->config.titlebar_color;


	if (tb > 0)
	{
		wlr_render_pass_add_rect(pass,
			&(struct wlr_render_rect_options){
				.box = { .width = content_w + 2 * bw, .height = tb },
				.color = { tcol[0], tcol[1], tcol[2], tcol[3] },
			});


		if (view->title_text_buf)
		{
			titlebar_text_update(view);
			if (view->title_text_buf->buffer)
			{
				struct wlr_texture *text_texture =
					wlr_texture_from_buffer(
						server->renderer,
						view->title_text_buf->buffer);
				if (text_texture)
				{
					const float ta = 1.0f;
					wlr_render_pass_add_texture(pass,
						&(struct wlr_render_texture_options){
							.texture = text_texture,
							.dst_box = {
								.x = bw,
								.y = 0,
								.width = content_w,
								.height = tb,
							},
							.alpha = &ta,
							.transform = WL_OUTPUT_TRANSFORM_NORMAL,
						});
					wlr_texture_destroy(text_texture);
				}
			}
		}
	}


	if (bw > 0)
	{
		int side_y = tb > 0 ? tb : bw;
		wlr_render_pass_add_rect(pass,
			&(struct wlr_render_rect_options){
				.box = { .x = 0, .y = side_y,
					.width = bw, .height = content_h },
				.color = { bcol[0], bcol[1], bcol[2], bcol[3] },
			});
		wlr_render_pass_add_rect(pass,
			&(struct wlr_render_rect_options){
				.box = { .x = content_w + bw, .y = side_y,
					.width = bw, .height = content_h },
				.color = { bcol[0], bcol[1], bcol[2], bcol[3] },
			});
		if (tb == 0)
		{
			wlr_render_pass_add_rect(pass,
				&(struct wlr_render_rect_options){
					.box = { .x = bw, .y = 0,
						.width = content_w, .height = bw },
					.color = { bcol[0], bcol[1], bcol[2], bcol[3] },
				});
		}
		int bot_y = tb > 0 ? tb + content_h : bw + content_h;
		wlr_render_pass_add_rect(pass,
			&(struct wlr_render_rect_options){
				.box = { .x = bw, .y = bot_y,
					.width = content_w, .height = bw },
				.color = { bcol[0], bcol[1], bcol[2], bcol[3] },
			});
	}


	if (texture)
	{
		float tex_w = surface->current.width;
		float tex_h = surface->current.height;
		render_texture_fitted(pass, texture,
			tex_w, tex_h, content_w, content_h, bw, tb);
	}

	if (!wlr_render_pass_submit(pass))
	{
		wlr_buffer_drop(buf);
		return NULL;
	}

	return buf;
}


void destroy_anim_overlay(struct hsdwl_server *server,
	struct hsdwl_view *view)
{
	if (view->anim_overlay)
	{
		animation_cancel_buffer(server, view->anim_overlay);
		wlr_scene_node_destroy(&view->anim_overlay->node);
		view->anim_overlay = NULL;
	}
	if (view->content_tree)
		wlr_scene_node_set_enabled(&view->content_tree->node, true);
}


struct wlr_scene_buffer *create_window_overlay(
	struct hsdwl_server *server, struct hsdwl_view *view,
	int content_w, int content_h, int bw, int tb,
	int abs_x, int abs_y)
{
	int win_w = content_w + 2 * bw;
	int win_h = content_h + tb + bw;
	if (win_w < 1) win_w = 1;
	if (win_h < 1) win_h = 1;

	struct wlr_buffer *cap = view_capture_full_window(
		server, view, content_w, content_h, bw, tb);
	if (!cap) return NULL;

	wlr_scene_node_raise_to_top(&server->animation_tree->node);

	struct wlr_scene_buffer *ov = wlr_scene_buffer_create(
		server->animation_tree, cap);
	wlr_buffer_drop(cap);
	wlr_scene_node_set_position(&ov->node, abs_x, abs_y);
	wlr_scene_buffer_set_dest_size(ov, win_w, win_h);
	wlr_scene_node_raise_to_top(&ov->node);
	return ov;
}
