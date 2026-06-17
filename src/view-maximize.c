#define _GNU_SOURCE

#include "view-maximize.h"
#include "server.h"
#include "deco.h"
#include "tab-group-anim.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>

#include "animation.h"



static void view_do_unmaximize(struct hsdwl_view *view)
{
	struct hsdwl_config *cfg = &view->server->config;
	wlr_scene_node_set_position(&view->scene_tree->node,
		view->saved_geometry.x, view->saved_geometry.y);

	if (view->xdg_surface && view->xdg_surface->configured)
		wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel,
			view->saved_geometry.width, view->saved_geometry.height);
	else if (view->xwayland_surface)
		wlr_xwayland_surface_configure(view->xwayland_surface,
			view->saved_geometry.x, view->saved_geometry.y,
			view->saved_geometry.width, view->saved_geometry.height);

	for (int i = 0; i < 4; i++)
		if (view->border_rects[i])
			wlr_scene_node_set_enabled(&view->border_rects[i]->node, true);
	if (view->title_text_buf)
		wlr_scene_node_set_enabled(&view->title_text_buf->node, true);

	int bw = cfg->border_width;
	int tb = cfg->titlebar_height > 0 ? cfg->titlebar_height : bw;
	if (view->content_tree)
		wlr_scene_node_set_position(&view->content_tree->node, bw, tb);

	view_borders_update(view);
	titlebar_text_update(view);
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
		if (tex_w < 1 || tex_h < 1)
		{
			tex_w = target_w;
			tex_h = target_h;
		}
		float scale = fmin((float)target_w / tex_w,
			(float)target_h / tex_h);
		float fit_w = tex_w * scale;
		float fit_h = tex_h * scale;
		float fit_x = ((float)target_w - fit_w) / 2.0f;
		float fit_y = ((float)target_h - fit_h) / 2.0f;

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
		if (tex_w < 1 || tex_h < 1)
		{
			tex_w = content_w;
			tex_h = content_h;
		}
		float scale = fmin((float)content_w / tex_w,
			(float)content_h / tex_h);
		float fit_w = tex_w * scale;
		float fit_h = tex_h * scale;
		float fit_x = bw + ((float)content_w - fit_w) / 2.0f;
		float fit_y = tb + ((float)content_h - fit_h) / 2.0f;

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

	if (!wlr_render_pass_submit(pass))
	{
		wlr_buffer_drop(buf);
		return NULL;
	}

	return buf;
}



static void destroy_anim_overlay(struct hsdwl_server *server,
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

static void view_anim_zoom_finish(struct hsdwl_server *server, void *user_data)
{
	struct hsdwl_view *view = user_data;
	destroy_anim_overlay(server, view);
	view_borders_update(view);
	titlebar_text_update(view);
}

static void view_anim_unmaximize_finish(struct hsdwl_server *server,
		void *user_data)
{
	struct hsdwl_view *view = user_data;
	destroy_anim_overlay(server, view);
	view_do_unmaximize(view);
}


static struct wlr_scene_buffer *create_window_overlay(
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

void view_maximize(struct hsdwl_server *server, struct hsdwl_view *view)
{
	if (!view || !view->scene_tree)
		return;

	if (hsdwl_tab_group_is_member(view))
	{
		hsdwl_tab_group_maximize(view->tab_group, server);
		return;
	}

	destroy_anim_overlay(server, view);

	struct hsdwl_config *cfg = &server->config;
	int bw = cfg->border_width;
	int tb = cfg->titlebar_height;

	
	if (view->maximized)
	{
		struct wlr_output *wlr_o = wlr_output_layout_output_at(
			server->output_layout,
			view->saved_geometry.x + view->saved_geometry.width / 2,
			view->saved_geometry.y + view->saved_geometry.height / 2);
		if (!wlr_o)
		{
			view_do_unmaximize(view);
			view->maximized = false;
			view->zoomed = false;
			return;
		}

		int cur_cw = 1, cur_ch = 1;
		if (view->xdg_surface && view->xdg_surface->configured)
		{
			cur_cw = view->xdg_surface->geometry.width;
			cur_ch = view->xdg_surface->geometry.height;
		}
		else if (view->xwayland_surface)
		{
			cur_cw = view->xwayland_surface->width;
			cur_ch = view->xwayland_surface->height;
		}

		int tb_cap = tb > 0 ? tb : 0;
		int src_full_w = cur_cw + 2 * bw;
		int src_full_h = cur_ch + tb_cap + bw;
		
		int src_abs_x = (int)view->scene_tree->node.x;
		int src_abs_y = (int)view->scene_tree->node.y;

		int tgt_full_w = view->saved_geometry.width + 2 * bw;
		int tgt_full_h = view->saved_geometry.height + tb_cap + bw;
		
		int tgt_abs_x = SIDEBAR_WIDTH + (int)view->saved_geometry.x;
		int tgt_abs_y = (int)view->saved_geometry.y;

		struct wlr_scene_buffer *ov = create_window_overlay(
			server, view, cur_cw, cur_ch, bw, tb_cap,
			src_abs_x, src_abs_y);
		if (!ov) return;

		for (int i = 0; i < 4; i++)
			if (view->border_rects[i])
				wlr_scene_node_set_enabled(
					&view->border_rects[i]->node, true);
		if (view->title_text_buf)
			wlr_scene_node_set_enabled(
				&view->title_text_buf->node, true);

		if (view->saved_parent)
		{
			wlr_scene_node_reparent(&view->scene_tree->node,
				view->saved_parent);
			view->saved_parent = NULL;
		}

		int ct_off_x = bw;
		int ct_off_y = tb > 0 ? tb : bw;
		if (view->content_tree)
			wlr_scene_node_set_position(
				&view->content_tree->node, ct_off_x, ct_off_y);

		wlr_scene_node_set_position(&view->scene_tree->node,
			view->saved_geometry.x, view->saved_geometry.y);

		if (view->xdg_surface && view->xdg_surface->configured)
			wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel,
				view->saved_geometry.width, view->saved_geometry.height);
		else if (view->xwayland_surface)
			wlr_xwayland_surface_configure(view->xwayland_surface,
				view->saved_geometry.x, view->saved_geometry.y,
				view->saved_geometry.width, view->saved_geometry.height);

		view_borders_update(view);
		titlebar_text_update(view);

		view->anim_overlay = ov;

		animation_create_with_fade(server, ov, 200, HSDWL_EASE_BEZIER,
			(double)src_abs_x, (double)src_abs_y,
			src_full_w, src_full_h,
			(double)tgt_abs_x, (double)tgt_abs_y,
			tgt_full_w, tgt_full_h,
			1.0f, 0.0f,
			view_anim_unmaximize_finish, view);

		view->maximized = false;
		view->zoomed = false;
		return;
	}

	
	if (view->zoomed)
	{
		struct wlr_output *wlr_o = wlr_output_layout_output_at(
			server->output_layout,
			view->saved_geometry.x + view->saved_geometry.width / 2,
			view->saved_geometry.y + view->saved_geometry.height / 2);
		if (!wlr_o) return;

		struct wlr_box obox;
		wlr_output_layout_get_box(server->output_layout, wlr_o, &obox);

		int fw = obox.width;
		if (fw < 1) fw = 1;

		for (int i = 0; i < 4; i++)
			if (view->border_rects[i])
				wlr_scene_node_set_enabled(
					&view->border_rects[i]->node, false);
		if (view->title_text_buf)
			wlr_scene_node_set_enabled(
				&view->title_text_buf->node, false);

		if (view->content_tree)
			wlr_scene_node_set_position(
				&view->content_tree->node, 0, 0);

		
		view->saved_parent = view->scene_tree->node.parent;
		wlr_scene_node_reparent(&view->scene_tree->node,
			server->workspaces[server->current_workspace]);
		wlr_scene_node_set_position(&view->scene_tree->node, 0, 0);

		if (view->xdg_surface && view->xdg_surface->configured)
			wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel,
				fw, obox.height);
		else if (view->xwayland_surface)
			wlr_xwayland_surface_configure(view->xwayland_surface,
				0, 0, fw, obox.height);

		view->zoomed = false;
		view->maximized = true;
		return;
	}

	
	view->saved_geometry.x = view->scene_tree->node.x;
	view->saved_geometry.y = view->scene_tree->node.y;
	if (view->xdg_surface && view->xdg_surface->configured)
	{
		view->saved_geometry.width = view->xdg_surface->geometry.width;
		view->saved_geometry.height = view->xdg_surface->geometry.height;
	}
	else if (view->xwayland_surface)
	{
		view->saved_geometry.width = view->xwayland_surface->width;
		view->saved_geometry.height = view->xwayland_surface->height;
	}
	else return;

	struct wlr_output *wlr_o = wlr_output_layout_output_at(
		server->output_layout,
		view->saved_geometry.x + view->saved_geometry.width / 2,
		view->saved_geometry.y + view->saved_geometry.height / 2);
	if (!wlr_o) return;

	struct wlr_box obox;
	wlr_output_layout_get_box(server->output_layout, wlr_o, &obox);

	int pad = 16;
	int zw = obox.width - SIDEBAR_WIDTH - pad;
	int zh = obox.height;
	if (zw < 1) zw = 1;
	int cw = zw - 2 * bw;
	int ch = zh - (tb > 0 ? tb : 0) - bw;
	if (cw < 1) cw = 1;
	if (ch < 1) ch = 1;

	int src_cw = view->saved_geometry.width;
	int src_ch = view->saved_geometry.height;
	int tb_cap = tb > 0 ? tb : 0;
	int src_full_w = src_cw + 2 * bw;
	int src_full_h = src_ch + tb_cap + bw;
	int src_abs_x = SIDEBAR_WIDTH + (int)view->saved_geometry.x;
	int src_abs_y = (int)view->saved_geometry.y;

	int tgt_full_w = cw + 2 * bw;
	int tgt_full_h = ch + tb_cap + bw;
	int tgt_abs_x = SIDEBAR_WIDTH + pad;
	int tgt_abs_y = 0;

	struct wlr_scene_buffer *ov = create_window_overlay(
		server, view, src_cw, src_ch, bw, tb_cap,
		src_abs_x, src_abs_y);
	if (!ov) return;

	wlr_scene_node_set_position(&view->scene_tree->node, pad, 0);

	if (view->xdg_surface && view->xdg_surface->configured)
		wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel, cw, ch);
	else if (view->xwayland_surface)
		wlr_xwayland_surface_configure(view->xwayland_surface,
			pad, 0, cw, ch);

	view->anim_overlay = ov;

	animation_create_with_fade(server, ov, 200, HSDWL_EASE_BEZIER,
		(double)src_abs_x, (double)src_abs_y,
		src_full_w, src_full_h,
		(double)tgt_abs_x, (double)tgt_abs_y,
		tgt_full_w, tgt_full_h,
		1.0f, 0.0f,
		view_anim_zoom_finish, view);

	view->zoomed = true;
	view->maximized = false;
}
