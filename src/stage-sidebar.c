#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "stage-3d.h"
#include "stage-sidebar.h"
#include "output.h"
#include "server.h"
#include "view.h"
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

	
	struct wlr_box bbox = {0};
	bool first = true;
	struct custom_window *cw;
	wl_list_for_each(cw, &stage->windows, link)
	{
		if (first)
		{
			bbox.x = cw->x; bbox.y = cw->y;
			bbox.width = cw->w; bbox.height = cw->h;
			first = false;
		}
		else
		{
			double x1 = fmin(bbox.x, cw->x);
			double y1 = fmin(bbox.y, cw->y);
			double x2 = fmax(bbox.x + bbox.width,
				cw->x + cw->w);
			double y2 = fmax(bbox.y + bbox.height,
				cw->y + cw->h);
			bbox.x = x1; bbox.y = y1;
			bbox.width = x2 - x1;
			bbox.height = y2 - y1;
		}
	}
	if (bbox.width < 1 || bbox.height < 1)
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

	wl_list_for_each(cw, &stage->windows, link)
	{
		float sx = (cw->x - bbox.x) * scale;
		float sy = (cw->y - bbox.y) * scale;
		float sw = cw->w * scale;
		float sh = cw->h * scale;

		if (sw < 2 || sh < 2)
			continue;

		struct wlr_surface *surface = view_get_surface(cw->view);
		if (!surface) continue;
		struct wlr_texture *texture = wlr_surface_get_texture(surface);
		if (!texture) continue;

		float tex_w = surface->current.width;
		float tex_h = surface->current.height;
		if (tex_w < 1 || tex_h < 1)
		{
			tex_w = cw->w;
			tex_h = cw->h;
		}
		float fit_scale = fmin(sw / tex_w, sh / tex_h);
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

	if (!wlr_render_pass_submit(pass))
	{
		wlr_buffer_drop(buf);
		return;
	}

	bool do_tilt = server->config.stage_3d_flip_enabled
		&& tilt_dir != 0.0f;

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



void stage_manager_render_sidebar(struct hsdwl_server *server, size_t ws)
{
	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
	int thumb_w = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;

	
	stage_hide_thumb(mgr->active_stage, true);

	
	struct custom_stage *st;
	wl_list_for_each(st, &mgr->inactive_stages, link)
		stage_hide_thumb(st, true);

	
	struct entry {
		struct custom_stage *st;
		int tw, th;
	} entries[64];
	int nentries = 0;

	wl_list_for_each(st, &mgr->inactive_stages, link)
	{
		if (wl_list_empty(&st->windows) || nentries >= 64)
			continue;

		struct wlr_box bbox = {0};
		bool first = true;
		struct custom_window *cw;
		wl_list_for_each(cw, &st->windows, link)
		{
			if (first)
			{
				bbox.x = cw->x; bbox.y = cw->y;
				bbox.width = cw->w; bbox.height = cw->h;
				first = false;
			}
			else
			{
				double x1 = fmin(bbox.x, cw->x);
				double y1 = fmin(bbox.y, cw->y);
				double x2 = fmax(bbox.x + bbox.width,
					cw->x + cw->w);
				double y2 = fmax(bbox.y + bbox.height,
					cw->y + cw->h);
				bbox.x = x1; bbox.y = y1;
				bbox.width = x2 - x1;
				bbox.height = y2 - y1;
			}
		}
		if (bbox.width < 1 || bbox.height < 1) continue;

		int tw = thumb_w;
		int th = thumb_w;

		entries[nentries].st = st;
		entries[nentries].tw = tw;
		entries[nentries].th = th;
		nentries++;
	}
	if (nentries == 0) return;

	
	int sidebar_h = 1050;
	if (!wl_list_empty(&server->outputs))
	{
		struct hsdwl_output *o = wl_container_of(
			server->outputs.next, o, link);
		if (o->wlr_output)
			sidebar_h = o->wlr_output->height;
	}

	int slot_h = sidebar_h / nentries;

	for (int i = 0; i < nentries; i++)
	{
		int x = STAGE_THUMB_PAD;

		
		int max_sz = slot_h - STAGE_THUMB_PAD * 2;
		if (max_sz < 20) max_sz = 20;
		if (entries[i].tw > max_sz)
			entries[i].tw = max_sz;
		entries[i].th = entries[i].tw;

		int y = i * slot_h + (slot_h - entries[i].th) / 2;
		if (y < STAGE_THUMB_PAD) y = STAGE_THUMB_PAD;

		entries[i].st->z_offset = (float)i * 30.0f;
		entries[i].st->thumb_x = x;
		entries[i].st->thumb_y = y;

		float td = (float)(y - sidebar_h / 2) / (float)(sidebar_h / 2);
		if (td < -1.0f) td = -1.0f;
		if (td > 1.0f) td = 1.0f;

		stage_hide_thumb(entries[i].st, false);
		stage_render_thumbnail(server, entries[i].st,
			entries[i].tw, entries[i].th, td);
		wlr_scene_node_set_position(
			&entries[i].st->thumb_tree->node, x, y);
		entries[i].st->thumb_dirty = false;
	}
}
