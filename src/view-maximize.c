#define _GNU_SOURCE

#include "view-capture.h"
#include "view-maximize.h"
#include "server.h"
#include "deco.h"
#include "tab-group-anim.h"
#include "stage.h"

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>

#include "animation.h"


static void view_set_surface_size(struct hsdwl_view *view,
	int x, int y, int w, int h)
{
	if (view->xdg_surface && view->xdg_surface->configured)
		wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel, w, h);
	else if (view->xwayland_surface)
		wlr_xwayland_surface_configure(view->xwayland_surface, x, y, w, h);
}


static void view_set_deco_visible(struct hsdwl_view *view, bool visible)
{
	for (int i = 0; i < 4; i++)
		if (view->border_rects[i])
			wlr_scene_node_set_enabled(&view->border_rects[i]->node, visible);
	if (view->title_text_buf)
		wlr_scene_node_set_enabled(&view->title_text_buf->node, visible);
	if (view->shadow_rect)
		wlr_scene_node_set_enabled(
			&view->shadow_rect->node, visible);
}


static void window_full_size(int content_w, int content_h, int bw, int tb,
	int *out_w, int *out_h)
{
	int tb_cap = tb > 0 ? tb : 0;
	*out_w = content_w + 2 * bw;
	*out_h = content_h + tb_cap + bw;
}


static void view_do_unmaximize(struct hsdwl_view *view)
{
	struct hsdwl_config *cfg = &view->server->config;
	wlr_scene_node_set_position(&view->scene_tree->node,
		view->saved_geometry.x, view->saved_geometry.y);

	view_set_surface_size(view,
		view->saved_geometry.x, view->saved_geometry.y,
		view->saved_geometry.width, view->saved_geometry.height);

	view_set_deco_visible(view, true);

	int bw = cfg->border_width;
	int tb = cfg->titlebar_height > 0 ? cfg->titlebar_height : bw;
	if (view->content_tree)
		wlr_scene_node_set_position(&view->content_tree->node, bw, tb);

	view_borders_update(view);
	titlebar_text_update(view);
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
		int src_full_w, src_full_h, tgt_full_w, tgt_full_h;
		window_full_size(cur_cw, cur_ch, bw, tb,
			&src_full_w, &src_full_h);
		window_full_size(view->saved_geometry.width,
			view->saved_geometry.height, bw, tb,
			&tgt_full_w, &tgt_full_h);

		
		int src_abs_x = (int)view->scene_tree->node.x;
		int src_abs_y = (int)view->scene_tree->node.y;

		int tgt_abs_x = SIDEBAR_WIDTH + (int)view->saved_geometry.x;
		int tgt_abs_y = (int)view->saved_geometry.y;

		struct wlr_scene_buffer *ov = create_window_overlay(
			server, view, cur_cw, cur_ch, bw, tb_cap,
			src_abs_x, src_abs_y);
		if (!ov) return;

		view_do_unmaximize(view);

		if (view->saved_parent)
		{
			wlr_scene_node_reparent(&view->scene_tree->node,
				view->saved_parent);
			view->saved_parent = NULL;
		}

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
		/*
		 * If the stage manager is managing windows, skip fully
		 * maximizing (stage 2) and unmaximize back to normal instead.
		 */
		if (server->config.stage_manager_enabled
				&& stage_manager_window_count(server,
					server->current_workspace) > 1)
		{
			struct wlr_output *wlr_o = wlr_output_layout_output_at(
				server->output_layout,
				view->saved_geometry.x
					+ view->saved_geometry.width / 2,
				view->saved_geometry.y
					+ view->saved_geometry.height / 2);
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
			int src_full_w, src_full_h, tgt_full_w, tgt_full_h;
			window_full_size(cur_cw, cur_ch, bw, tb,
				&src_full_w, &src_full_h);
			window_full_size(view->saved_geometry.width,
				view->saved_geometry.height, bw, tb,
				&tgt_full_w, &tgt_full_h);

			int src_abs_x = SIDEBAR_WIDTH
				+ (int)view->scene_tree->node.x;
			int src_abs_y = (int)view->scene_tree->node.y;

			int tgt_abs_x = SIDEBAR_WIDTH
				+ (int)view->saved_geometry.x;
			int tgt_abs_y = (int)view->saved_geometry.y;

			struct wlr_scene_buffer *ov = create_window_overlay(
				server, view, cur_cw, cur_ch, bw, tb_cap,
				src_abs_x, src_abs_y);
			if (!ov) return;

			view_do_unmaximize(view);

			view->anim_overlay = ov;

			animation_create_with_fade(server, ov, 200,
				HSDWL_EASE_BEZIER,
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

		struct wlr_output *wlr_o = wlr_output_layout_output_at(
			server->output_layout,
			view->saved_geometry.x + view->saved_geometry.width / 2,
			view->saved_geometry.y + view->saved_geometry.height / 2);
		if (!wlr_o) return;

		struct wlr_box obox;
		wlr_output_layout_get_box(server->output_layout, wlr_o, &obox);

		int fw = obox.width;
		if (fw < 1) fw = 1;

		view_set_deco_visible(view, false);

		if (view->content_tree)
			wlr_scene_node_set_position(
				&view->content_tree->node, 0, 0);

		view->saved_parent = view->scene_tree->node.parent;
		wlr_scene_node_reparent(&view->scene_tree->node,
			server->workspaces[server->current_workspace]);
		wlr_scene_node_set_position(&view->scene_tree->node, 0, 0);

		view_set_surface_size(view, 0, 0, fw, obox.height);

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
	int src_full_w, src_full_h, tgt_full_w, tgt_full_h;
	window_full_size(src_cw, src_ch, bw, tb,
		&src_full_w, &src_full_h);
	window_full_size(cw, ch, bw, tb,
		&tgt_full_w, &tgt_full_h);
	int src_abs_x = SIDEBAR_WIDTH + (int)view->saved_geometry.x;
	int src_abs_y = (int)view->saved_geometry.y;

	int tgt_abs_x = SIDEBAR_WIDTH + pad;
	int tgt_abs_y = 0;

	struct wlr_scene_buffer *ov = create_window_overlay(
		server, view, src_cw, src_ch, bw, tb_cap,
		src_abs_x, src_abs_y);
	if (!ov) return;

	wlr_scene_node_set_position(&view->scene_tree->node, pad, 0);

	view_set_surface_size(view, pad, 0, cw, ch);

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


void view_demaximize_to_zoomed(struct hsdwl_view *view,
		struct hsdwl_server *server)
{
	if (!view || !view->maximized)
		return;

	struct hsdwl_config *cfg = &server->config;
	int bw = cfg->border_width;
	int tb = cfg->titlebar_height;

	struct wlr_output *wlr_o = wlr_output_layout_output_at(
		server->output_layout,
		view->saved_geometry.x + view->saved_geometry.width / 2,
		view->saved_geometry.y + view->saved_geometry.height / 2);
	if (!wlr_o) return;

	struct wlr_box obox;
	wlr_output_layout_get_box(server->output_layout, wlr_o, &obox);

	int pad = 16;
	int zw = obox.width - SIDEBAR_WIDTH - pad;
	if (zw < 1) zw = 1;
	int cw = zw - 2 * bw;
	int ch = obox.height - (tb > 0 ? tb : 0) - bw;
	if (cw < 1) cw = 1;
	if (ch < 1) ch = 1;

	view_set_deco_visible(view, true);

	if (view->content_tree)
		wlr_scene_node_set_position(&view->content_tree->node, bw, tb);

	if (view->saved_parent)
	{
		wlr_scene_node_reparent(&view->scene_tree->node,
			view->saved_parent);
		view->saved_parent = NULL;
	}

	wlr_scene_node_set_position(&view->scene_tree->node, pad, 0);

	view_set_surface_size(view, pad, 0, cw, ch);

	view->maximized = false;
	view->zoomed = true;

	view_borders_update(view);
	titlebar_text_update(view);
}


void view_unmaximize_instant(struct hsdwl_view *view,
		struct hsdwl_server *server)
{
	if (!view || !(view->maximized || view->zoomed))
		return;

	destroy_anim_overlay(server, view);
	view_do_unmaximize(view);

	if (view->saved_parent)
	{
		wlr_scene_node_reparent(&view->scene_tree->node,
			view->saved_parent);
		view->saved_parent = NULL;
	}

	view->maximized = false;
	view->zoomed = false;
}
