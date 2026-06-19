#define _GNU_SOURCE

#include "tab-group-anim.h"
#include "tab-group-layout.h"
#include "server.h"
#include "view.h"

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
#include "stage.h"

struct tg_anim_state {
	struct hsdwl_tab_group *group;
	int cw, ch;
	struct wlr_scene_buffer *overlay;
};



static struct wlr_buffer *tab_group_capture_full(
	struct hsdwl_server *server,
	struct hsdwl_tab_group *group,
	int content_w, int content_h)
{
	int win_w = content_w;
	int win_h = content_h + group->tab_bar_thickness;
	if (win_w < 1) win_w = 1;
	if (win_h < 1) win_h = 1;

	struct wlr_surface *surface = NULL;
	if (group->active) {
		if (group->active->xdg_surface)
			surface = group->active->xdg_surface->surface;
		else if (group->active->xwayland_surface)
			surface = group->active->xwayland_surface->surface;
	}
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
	if (!buf) return NULL;

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, buf, NULL);
	if (!pass) { wlr_buffer_drop(buf); return NULL; }

	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .width = win_w, .height = win_h },
		.color = { 0.0f, 0.0f, 0.0f, 0.0f },
	});

	
	bool focused = (group->active == server->focused_views[
		server->current_workspace]);
	float *bg = focused
		? server->config.titlebar_color_focused
		: server->config.titlebar_color;
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .width = win_w, .height = group->tab_bar_thickness },
		.color = { bg[0], bg[1], bg[2], bg[3] },
	});

	
	if (texture)
	{
		float tex_w = surface->current.width;
		float tex_h = surface->current.height;
		if (tex_w < 1 || tex_h < 1) { tex_w = content_w; tex_h = content_h; }
		float scale = fmin((float)content_w / tex_w,
			(float)content_h / tex_h);
		float fit_w = tex_w * scale;
		float fit_h = tex_h * scale;
		float fit_x = ((float)content_w - fit_w) / 2.0f;
		float fit_y = group->tab_bar_thickness
			+ ((float)content_h - fit_h) / 2.0f;
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

static struct wlr_scene_buffer *tab_group_create_overlay(
	struct hsdwl_server *server,
	struct hsdwl_tab_group *group,
	int content_w, int content_h,
	int abs_x, int abs_y)
{
	int win_w = content_w;
	int win_h = content_h + group->tab_bar_thickness;
	if (win_w < 1) win_w = 1;
	if (win_h < 1) win_h = 1;

	struct wlr_buffer *cap = tab_group_capture_full(
		server, group, content_w, content_h);
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



static void destroy_tg_overlay(struct hsdwl_server *server,
	struct tg_anim_state *st)
{
	if (st->overlay)
	{
		animation_cancel_buffer(server, st->overlay);
		wlr_scene_node_destroy(&st->overlay->node);
		st->overlay = NULL;
	}
}

static void tg_anim_zoom_finish(struct hsdwl_server *server, void *user_data)
{
	struct tg_anim_state *st = user_data;
	struct hsdwl_tab_group *group = st->group;

	destroy_tg_overlay(server, st);

	group->content_area_box.width = st->cw;
	group->content_area_box.height = st->ch;

	struct hsdwl_view *vi;
	wl_list_for_each(vi, &group->views, tab_group_link)
		view_configure_size(vi, st->cw, st->ch);

	hsdwl_tab_group_update_layout(group);
	free(st);
}

static void tg_anim_full_finish(struct hsdwl_server *server, void *user_data)
{
	struct tg_anim_state *st = user_data;
	struct hsdwl_tab_group *group = st->group;

	group->content_area_box.width = st->cw;
	group->content_area_box.height = st->ch;

	struct hsdwl_view *vi;
	wl_list_for_each(vi, &group->views, tab_group_link)
		view_configure_size(vi, st->cw, st->ch);

	hsdwl_tab_group_update_layout(group);
	free(st);
}

static void tg_anim_restore_finish(struct hsdwl_server *server, void *user_data)
{
	struct tg_anim_state *st = user_data;
	struct hsdwl_tab_group *group = st->group;

	destroy_tg_overlay(server, st);

	group->content_area_box.width = st->cw;
	group->content_area_box.height = st->ch;

	struct hsdwl_view *vi;
	wl_list_for_each(vi, &group->views, tab_group_link)
		view_configure_size(vi, st->cw, st->ch);

	hsdwl_tab_group_update_layout(group);
	free(st);
}



void hsdwl_tab_group_zoom(struct hsdwl_tab_group *group,
		struct hsdwl_server *server)
{
	struct wlr_output *wlr_o = wlr_output_layout_output_at(
		server->output_layout,
		group->scene_tree->node.x +
			group->content_area_box.width / 2,
		group->scene_tree->node.y +
			group->content_area_box.height / 2);
	if (!wlr_o) return;

	struct wlr_box obox;
	wlr_output_layout_get_box(server->output_layout, wlr_o, &obox);

	group->saved_geometry.x = group->scene_tree->node.x;
	group->saved_geometry.y = group->scene_tree->node.y;
	group->saved_geometry.width = group->content_area_box.width;
	group->saved_geometry.height =
		group->content_area_box.height + group->tab_bar_thickness;

	int pad = 16;
	int zw = obox.width - SIDEBAR_WIDTH - pad;
	if (zw < 1) zw = 1;
	int zh = obox.height - group->tab_bar_thickness;
	if (zh < 1) zh = 1;

	
	int src_cw = group->content_area_box.width;
	int src_ch = group->content_area_box.height;
	int src_abs_x = SIDEBAR_WIDTH
		+ (int)group->scene_tree->node.x;
	int src_abs_y = (int)group->scene_tree->node.y;

	struct wlr_scene_buffer *ov = tab_group_create_overlay(
		server, group, src_cw, src_ch, src_abs_x, src_abs_y);
	if (!ov) ov = NULL; 

	
	group->content_area_box.width = zw;
	group->content_area_box.height = zh;

	struct hsdwl_view *vi;
	wl_list_for_each(vi, &group->views, tab_group_link)
		view_configure_size(vi, zw, zh);

	hsdwl_tab_group_update_layout(group);

	wlr_scene_node_set_position(&group->scene_tree->node,
		pad, 0);

	struct tg_anim_state *st = calloc(1, sizeof(*st));
	st->group = group;
	st->cw = zw;
	st->ch = zh;
	st->overlay = ov;

	if (ov)
	{
		int tgt_abs_x = SIDEBAR_WIDTH + pad;
		int tgt_abs_y = 0;
		int src_w = src_cw;
		int src_h = src_ch + group->tab_bar_thickness;
		int tgt_w = zw;
		int tgt_h = zh + group->tab_bar_thickness;

		animation_create_with_fade(server, ov, 200,
			HSDWL_EASE_BEZIER,
			(double)src_abs_x, (double)src_abs_y,
			src_w, src_h,
			(double)tgt_abs_x, (double)tgt_abs_y,
			tgt_w, tgt_h,
			1.0f, 0.0f,
			tg_anim_zoom_finish, st);
	}
	else
	{
		animation_create_node_pos(server,
			&group->scene_tree->node,
			200, HSDWL_EASE_BEZIER,
			(double)group->saved_geometry.x,
			(double)group->saved_geometry.y,
			(double)pad, 0.0,
			tg_anim_zoom_finish, st);
	}

	wlr_output_schedule_frame(wlr_o);
	group->zoomed = true;
	group->maximized = false;
}



void hsdwl_tab_group_maximize(struct hsdwl_tab_group *group,
		struct hsdwl_server *server)
{
	if (!group || !group->scene_tree)
		return;

	if (group->maximized)
	{
		hsdwl_tab_group_restore(group);
		return;
	}

	if (group->zoomed)
	{
		/*
		 * If the stage manager is managing windows beyond those
		 * in this tab group, skip fully maximizing and restore
		 * to normal size/position instead.
		 */
		{
			int view_count = 0;
			struct hsdwl_view *vi;
			wl_list_for_each(vi, &group->views, tab_group_link)
				view_count++;
			if (server->config.stage_manager_enabled
					&& stage_manager_window_count(server,
						server->current_workspace)
						> view_count)
			{
				hsdwl_tab_group_restore(group);
				return;
			}
		}

		struct wlr_output *wlr_o = wlr_output_layout_output_at(
			server->output_layout,
			group->scene_tree->node.x +
				group->content_area_box.width / 2,
			group->scene_tree->node.y +
				group->content_area_box.height / 2);
		if (!wlr_o) return;

		struct wlr_box obox;
		wlr_output_layout_get_box(server->output_layout, wlr_o, &obox);

		int fh = obox.height - group->tab_bar_thickness;
		if (fh < 1) fh = 1;

		group->content_area_box.width = obox.width;
		group->content_area_box.height = fh;

		struct hsdwl_view *vi;
		wl_list_for_each(vi, &group->views, tab_group_link)
			view_configure_size(vi, obox.width, fh);

		hsdwl_tab_group_update_layout(group);

		double cur_x = group->scene_tree->node.x;
		double cur_y = group->scene_tree->node.y;
		double tgt_x = -SIDEBAR_WIDTH;
		double tgt_y = 0;

		struct tg_anim_state *st = calloc(1, sizeof(*st));
		st->group = group;
		st->cw = obox.width;
		st->ch = fh;
		st->overlay = NULL;

		animation_create_node_pos(server,
			&group->scene_tree->node,
			200, HSDWL_EASE_BEZIER,
			cur_x, cur_y, tgt_x, tgt_y,
			tg_anim_full_finish, st);

		wlr_output_schedule_frame(wlr_o);
		group->zoomed = false;
		group->maximized = true;
		return;
	}

	hsdwl_tab_group_zoom(group, server);
}



void hsdwl_tab_group_restore(struct hsdwl_tab_group *group)
{
	if (!group || (!group->maximized && !group->zoomed))
		return;

	int tgt_w = group->saved_geometry.width;
	int tgt_h = group->saved_geometry.height
		- group->tab_bar_thickness;

	
	int src_cw = group->content_area_box.width;
	int src_ch = group->content_area_box.height;
	int src_abs_x = SIDEBAR_WIDTH
		+ (int)group->scene_tree->node.x;
	int src_abs_y = (int)group->scene_tree->node.y;

	struct wlr_scene_buffer *ov = tab_group_create_overlay(
		group->server, group, src_cw, src_ch,
		src_abs_x, src_abs_y);
	if (!ov) ov = NULL;

	
	group->content_area_box.width = tgt_w;
	group->content_area_box.height = tgt_h;

	struct hsdwl_view *vi;
	wl_list_for_each(vi, &group->views, tab_group_link)
		view_configure_size(vi, tgt_w, tgt_h);

	hsdwl_tab_group_update_layout(group);

	wlr_scene_node_set_position(&group->scene_tree->node,
		group->saved_geometry.x,
		group->saved_geometry.y);

	struct tg_anim_state *st = calloc(1, sizeof(*st));
	st->group = group;
	st->cw = tgt_w;
	st->ch = tgt_h;
	st->overlay = ov;

	if (ov)
	{
		int tgt_abs_x = SIDEBAR_WIDTH
			+ (int)group->saved_geometry.x;
		int tgt_abs_y = (int)group->saved_geometry.y;
		int src_w = src_cw;
		int src_h = src_ch + group->tab_bar_thickness;
		int tgt_w_ = tgt_w;
		int tgt_h_ = tgt_h + group->tab_bar_thickness;

		animation_create_with_fade(group->server, ov, 200,
			HSDWL_EASE_BEZIER,
			(double)src_abs_x, (double)src_abs_y,
			src_w, src_h,
			(double)tgt_abs_x, (double)tgt_abs_y,
			tgt_w_, tgt_h_,
			1.0f, 0.0f,
			tg_anim_restore_finish, st);
	}
	else
	{
		double cur_x = group->scene_tree->node.x;
		double cur_y = group->scene_tree->node.y;
		double tgt_x = group->saved_geometry.x;
		double tgt_y = group->saved_geometry.y;

		animation_create_node_pos(group->server,
			&group->scene_tree->node,
			200, HSDWL_EASE_BEZIER,
			cur_x, cur_y, tgt_x, tgt_y,
			tg_anim_restore_finish, st);
	}

	struct wlr_output *wlr_o = wlr_output_layout_output_at(
		group->server->output_layout,
		group->saved_geometry.x + group->saved_geometry.width / 2,
		group->saved_geometry.y + group->saved_geometry.height / 2);
	if (wlr_o)
		wlr_output_schedule_frame(wlr_o);

	group->maximized = false;
	group->zoomed = false;
}
