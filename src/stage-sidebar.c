#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "stage-3d.h"
#include "stage-sidebar.h"
#include "output.h"
#include "server.h"
#include "view.h"
#include "stage-util.h"
#include "tab-group.h"
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>


const char *view_get_app_name(struct hsdwl_view *view)
{
	if (!view) return "Unknown";
	if (view->xdg_surface && view->xdg_surface->toplevel
			&& view->xdg_surface->toplevel->app_id)
		return view->xdg_surface->toplevel->app_id;
	if (view->xwayland_surface && view->xwayland_surface->class)
		return view->xwayland_surface->class;
	return "Unknown";
}

const char *stage_get_app_name(struct custom_stage *stage)
{
	struct custom_window *cw;
	wl_list_for_each(cw, &stage->windows, link)
	{
		if (!cw->view) continue;
		return view_get_app_name(cw->view);
	}
	return "Unknown";
}


void stage_render_thumbnail(struct hsdwl_server *server,
		struct custom_stage *stage, int thumb_w, int thumb_h,
		float tilt_dir)
{
	if (!stage->thumb_buf || wl_list_empty(&stage->windows))
		return;


	struct wlr_box bbox;
	if (!stage_compute_bbox(stage, &bbox))
		return;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format fmt = {
		.format = DRM_FORMAT_ARGB8888,
		.len = 1,
		.modifiers = mods,
	};
	struct wlr_buffer *buf = wlr_allocator_create_buffer(
		server->allocator, thumb_w, thumb_h, &fmt);
	if (!buf) return;

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, buf, NULL);
	if (!pass) { wlr_buffer_drop(buf); return; }

	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .width = thumb_w, .height = thumb_h },
		.color = { 0.0f, 0.0f, 0.0f, 0.0f },
	});

	float scale = (float)thumb_w / bbox.width;

	struct custom_window *cw;
	wl_list_for_each(cw, &stage->windows, link)
	{
		float sx = (cw->x - bbox.x) * scale;
		float sy = (cw->y - bbox.y) * scale;
		float sw = cw->w * scale;
		float sh = cw->h * scale;

		if (sw < 2 || sh < 2)
			continue;

		if (cw->view && cw->view->tab_group)
		{
			struct hsdwl_tab_group *g = cw->view->tab_group;
			int n_tabs = wl_list_length(&g->views);

			/* Count actual renderable views */
			int n_renderable = 0;
			struct hsdwl_view *vtmp;
			wl_list_for_each(vtmp, &g->views, tab_group_link)
			{
				if (view_get_surface(vtmp))
					n_renderable++;
			}
			if (n_renderable == 0) continue;

			/* Layout gap between side-by-side windows */
			float gap = sw * 0.06f;
			float avail_w = sw + gap;
			float cell_w = avail_w / (float)n_renderable;
			float cell_h = sh;

			int idx = 0;
			struct hsdwl_view *vi;
			wl_list_for_each(vi, &g->views, tab_group_link)
			{
				struct wlr_surface *surface =
					view_get_surface(vi);
				if (!surface) continue;
				struct wlr_texture *texture =
					wlr_surface_get_texture(surface);
				if (!texture) continue;

				float tx = sx + (float)idx * cell_w;
				float ty = sy ;

				float tex_w = surface->current.width;
				float tex_h = surface->current.height;
				if (tex_w < 1 || tex_h < 1)
				{
					tex_w = cw->w;
					tex_h = cw->h;
				}
				float fs = fmin(cell_w / tex_w,
					cell_h / tex_h);
				float fw = tex_w * fs;
				float fh = tex_h * fs;
				float fx = tx + (cell_w - fw) / 2;
				float fy = ty + (cell_h - fh) / 2;
				const float a = 1.0f;

				wlr_render_pass_add_texture(pass,
					&(struct wlr_render_texture_options){
					.texture = texture,
					.dst_box = {
						.x = (int)(fx + 0.5f),
						.y = (int)(fy + 0.5f),
						.width = (int)(fw + 0.5f),
						.height = (int)(fh + 0.5f),
					},
					.alpha = &a,
					.transform =
						WL_OUTPUT_TRANSFORM_NORMAL,
				});
				idx++;
			}
		}
		else
		{
			struct wlr_surface *surface =
				view_get_surface(cw->view);
			if (!surface) continue;
			struct wlr_texture *texture =
				wlr_surface_get_texture(surface);
			if (!texture) continue;

			float tex_w = surface->current.width;
			float tex_h = surface->current.height;
			if (tex_w < 1 || tex_h < 1)
			{
				tex_w = cw->w;
				tex_h = cw->h;
			}
			float fit_scale = fmin(sw / tex_w,
				sh / tex_h);
			float fit_w = tex_w * fit_scale;
			float fit_h = tex_h * fit_scale;
			float fit_x = sx + (sw - fit_w) / 2;
			float fit_y = sy + (sh - fit_h) / 2;
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
	}

	if (!wlr_render_pass_submit(pass))
	{
		wlr_buffer_drop(buf);
		return;
	}

	bool do_tilt = tilt_dir != 0.0f;

	if (do_tilt) {
		struct wlr_texture *tex = wlr_texture_from_buffer(
			server->renderer, buf);
		if (tex) {
			struct wlr_buffer *tilted = wlr_allocator_create_buffer(
				server->allocator, thumb_w, thumb_h, &fmt);
			if (tilted) {
				struct wlr_render_pass *tpass =
					wlr_renderer_begin_buffer_pass(
						server->renderer, tilted, NULL);
				if (tpass) {
					wlr_render_pass_add_rect(tpass,
						&(struct wlr_render_rect_options){
							.box = { .width = thumb_w,
								.height = thumb_h },
							.color = { 0, 0, 0, 0 },
						});
				stage_3d_render_tilted(tpass, tex,
					thumb_w, thumb_h,
					0, 0, thumb_w, thumb_h,
					stage->z_offset,
					-30.0f, 1.0f, 800.0f);
					if (wlr_render_pass_submit(tpass)) {
						wlr_scene_buffer_set_buffer(
							stage->thumb_buf, tilted);
						wlr_scene_buffer_set_dest_size(
							stage->thumb_buf,
							thumb_w, thumb_h);
						wlr_buffer_drop(tilted);
						wlr_texture_destroy(tex);
						wlr_buffer_drop(buf);
						return;
					}
					wlr_buffer_drop(tilted);
				} else {
					wlr_buffer_drop(tilted);
				}
			}
			wlr_texture_destroy(tex);
		}
	}

	wlr_scene_buffer_set_buffer(stage->thumb_buf, buf);
	wlr_scene_buffer_set_dest_size(stage->thumb_buf,
		thumb_w, thumb_h);
	wlr_buffer_drop(buf);
}


void stage_hide_thumb(struct custom_stage *st, bool hide)
{
	if (st && st->thumb_tree)
		wlr_scene_node_set_enabled(&st->thumb_tree->node, !hide);
}

static bool stage_has_renderable_thumbnail(struct custom_stage *stage)
{
	struct wlr_box bbox;

	return stage && stage->thumb_tree && stage->thumb_buf
		&& !wl_list_empty(&stage->windows)
		&& stage_compute_bbox(stage, &bbox);
}


void stage_manager_render_sidebar(struct hsdwl_server *server, size_t ws)
{
	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
	int max_thumb_size = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;
	if (max_thumb_size < 1)
		max_thumb_size = 1;


	stage_hide_thumb(mgr->active_stage, true);


	struct custom_stage *st;
	wl_list_for_each(st, &mgr->inactive_stages, link)
		stage_hide_thumb(st, true);

	size_t nentries = 0;
	wl_list_for_each(st, &mgr->inactive_stages, link)
	{
		if (stage_has_renderable_thumbnail(st))
			nentries++;
	}
	if (nentries == 0) return;

	int sidebar_h = output_get_height(server);
	if (sidebar_h < 1)
		sidebar_h = 1;

	size_t index = 0;
	wl_list_for_each(st, &mgr->inactive_stages, link)
	{
		if (!stage_has_renderable_thumbnail(st))
			continue;

		int slot_top = (int)((double)index * sidebar_h / nentries);
		int slot_bottom = (int)((double)(index + 1) * sidebar_h
			/ nentries);
		int slot_h = slot_bottom - slot_top;
		if (slot_h < 1)
			slot_h = 1;

		int padding = slot_h > STAGE_THUMB_PAD * 2
			? STAGE_THUMB_PAD : 0;
		int thumb_size = slot_h - padding * 2;
		if (thumb_size < 1)
			thumb_size = 1;
		if (thumb_size > max_thumb_size)
			thumb_size = max_thumb_size;

		int x = STAGE_THUMB_PAD;
		int y = slot_top + (slot_h - thumb_size) / 2;

		st->z_offset = (float)index * 30.0f;
		st->thumb_x = x;
		st->thumb_y = y;
		st->thumb_w = thumb_size;
		st->thumb_h = thumb_size;

		float sidebar_half = (float)sidebar_h / 2.0f;
		float td = ((float)y - sidebar_half) / sidebar_half;
		if (td < -1.0f) td = -1.0f;
		if (td > 1.0f) td = 1.0f;

		stage_hide_thumb(st, false);
		stage_render_thumbnail(server, st, thumb_size, thumb_size, td);
		wlr_scene_node_set_position(
			&st->thumb_tree->node, x, y);
		st->thumb_dirty = false;
		index++;
	}
}
