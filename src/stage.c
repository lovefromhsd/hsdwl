#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "stage.h"
#include "animation.h"
#include "output.h"
#include "server.h"
#include "view.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#define MAX_STAGE_WINDOWS 64

/* ── animation helper structs ── */

struct stage_switch_anim {
	struct hsdwl_server *server;
	struct custom_stage *old_stage;
	struct custom_stage *new_stage;
	size_t ws;
	int remaining;
	bool insert_tail;
	int n_overlays;
	struct wlr_scene_buffer *overlays[MAX_STAGE_WINDOWS];
};

struct stage_merge_anim {
	struct hsdwl_server *server;
	struct custom_stage *source;
	size_t ws;
	int remaining;
	int n_overlays;
	struct wlr_scene_buffer *overlays[MAX_STAGE_WINDOWS];
};

/* ── helpers ── */

static void stage_set_views_enabled(struct custom_stage *stage, bool enabled)
{
	struct custom_window *cw;
	wl_list_for_each(cw, &stage->windows, link)
	{
		if (cw->view && cw->view->tab_group
				&& cw->view->tab_group->scene_tree)
		{
			wlr_scene_node_set_enabled(
				&cw->view->tab_group->scene_tree->node,
				enabled);
		}
		else if (cw->view && cw->view->scene_tree)
		{
			wlr_scene_node_set_enabled(
				&cw->view->scene_tree->node, enabled);
		}
	}
}

static void stage_reparent_to_canvas(struct custom_stage *stage,
		struct hsdwl_server *server)
{
	(void)server;
	struct custom_window *cw;
	wl_list_for_each(cw, &stage->windows, link)
	{
		if (cw->view && cw->view->tab_group
				&& cw->view->tab_group->scene_tree
				&& cw->view->tab_group->scene_tree->node.parent
					== stage->tree)
		{
			wlr_scene_node_set_enabled(
				&cw->view->tab_group->scene_tree->node, true);
		}
		else if (cw->view && cw->view->scene_tree)
		{
			wlr_scene_node_reparent(
				&cw->view->scene_tree->node,
				stage->tree);
			wlr_scene_node_set_position(
				&cw->view->scene_tree->node,
				cw->x, cw->y);
			wlr_scene_node_set_enabled(
				&cw->view->scene_tree->node, true);
		}
	}
}

static void stage_safe_evict(struct custom_stage *stage,
		struct hsdwl_server *server, size_t ws)
{
	struct custom_window *cw, *tmp;
	wl_list_for_each_safe(cw, tmp, &stage->windows, link)
	{
		wl_list_remove(&cw->link);
		if (cw->view && cw->view->scene_tree)
		{
			wlr_scene_node_reparent(
				&cw->view->scene_tree->node,
				server->workspaces[ws]);
			wlr_scene_node_set_enabled(
				&cw->view->scene_tree->node, false);
		}
		free(cw);
	}
	if (stage->thumb_buf)
		wlr_scene_node_destroy(&stage->thumb_buf->node);
	if (stage->thumb_bg)
		wlr_scene_node_destroy(&stage->thumb_bg->node);
	if (stage->thumb_tree)
		wlr_scene_node_destroy(&stage->thumb_tree->node);
	if (stage->tree)
		wlr_scene_node_destroy(&stage->tree->node);
	free(stage);
}

static struct custom_window *find_custom_window(struct custom_stage *stage,
		struct hsdwl_view *view)
{
	struct custom_window *cw;
	wl_list_for_each(cw, &stage->windows, link)
	{
		if (cw->view == view)
			return cw;
	}
	return NULL;
}

static void stage_free(struct custom_stage *stage)
{
	if (!stage) return;
	struct custom_window *cw, *tmp;
	wl_list_for_each_safe(cw, tmp, &stage->windows, link)
	{
		wl_list_remove(&cw->link);
		free(cw);
	}
	if (stage->thumb_buf)
		wlr_scene_node_destroy(&stage->thumb_buf->node);
	if (stage->thumb_bg)
		wlr_scene_node_destroy(&stage->thumb_bg->node);
	if (stage->thumb_tree)
		wlr_scene_node_destroy(&stage->thumb_tree->node);
	if (stage->tree)
		wlr_scene_node_destroy(&stage->tree->node);
	free(stage);
}

/* ── thumbnail rendering ── */

static void stage_render_thumbnail(struct hsdwl_server *server,
		struct custom_stage *stage, int thumb_w, int thumb_h)
{
	if (!stage->thumb_buf || wl_list_empty(&stage->windows))
		return;

	/* compute bounding box */
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

	wlr_scene_buffer_set_buffer(stage->thumb_buf, buf);
	wlr_scene_buffer_set_dest_size(stage->thumb_buf, thumb_w, thumb_h);
	wlr_buffer_drop(buf);
}

/* ── app-name helpers for grouping ── */

static const char *view_get_app_name(struct hsdwl_view *view)
{
	if (!view) return "Unknown";
	if (view->xdg_surface && view->xdg_surface->toplevel
			&& view->xdg_surface->toplevel->app_id)
		return view->xdg_surface->toplevel->app_id;
	if (view->xwayland_surface && view->xwayland_surface->class)
		return view->xwayland_surface->class;
	return "Unknown";
}

static const char *stage_get_app_name(struct custom_stage *stage)
{
	struct custom_window *cw;
	wl_list_for_each(cw, &stage->windows, link)
	{
		if (!cw->view) continue;
		return view_get_app_name(cw->view);
	}
	return "Unknown";
}

/* ── animation completion callbacks ── */

static void stage_switch_on_anim_done(struct hsdwl_server *server,
		void *user_data)
{
	struct stage_switch_anim *ssa = user_data;
	ssa->remaining--;
	if (ssa->remaining > 0) return;

	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ssa->ws];

	for (int i = 0; i < ssa->n_overlays; i++)
		wlr_scene_node_destroy(&ssa->overlays[i]->node);

	if (ssa->old_stage) {
		stage_set_views_enabled(ssa->old_stage, false);
		if (ssa->insert_tail)
			wl_list_insert(mgr->inactive_stages.prev,
				&ssa->old_stage->link);
		else
			wl_list_insert(&mgr->inactive_stages,
				&ssa->old_stage->link);
	}
	wl_list_remove(&ssa->new_stage->link);
	mgr->active_stage = ssa->new_stage;
	stage_reparent_to_canvas(ssa->new_stage, server);

	struct custom_window *cw;
	wl_list_for_each(cw, &ssa->new_stage->windows, link) {
		view_focus(server, cw->view);
		break;
	}
	stage_manager_render_sidebar(server, ssa->ws);

	free(ssa);
}

static void stage_merge_on_anim_done(struct hsdwl_server *server,
		void *user_data)
{
	struct stage_merge_anim *sma = user_data;
	sma->remaining--;
	if (sma->remaining > 0) return;

	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[sma->ws];
	struct custom_stage *source = sma->source;

	for (int i = 0; i < sma->n_overlays; i++)
		wlr_scene_node_destroy(&sma->overlays[i]->node);

	struct custom_window *cw, *tmp;
	wl_list_for_each_safe(cw, tmp, &source->windows, link)
	{
		wl_list_remove(&cw->link);
		if (cw->view && cw->view->scene_tree)
		{
			wlr_scene_node_reparent(
				&cw->view->scene_tree->node,
				mgr->active_stage->tree);
			wlr_scene_node_set_position(
				&cw->view->scene_tree->node,
				cw->x, cw->y);
			wlr_scene_node_set_enabled(
				&cw->view->scene_tree->node, true);
		}
		wl_list_insert(&mgr->active_stage->windows, &cw->link);
	}

	wl_list_remove(&source->link);
	stage_free(source);
	stage_manager_render_sidebar(server, sma->ws);
	free(sma);
}

/* ── public API ── */

void stage_manager_init(struct hsdwl_server *server)
{
	for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
	{
		server->ws_sidebar_trees[i] = wlr_scene_tree_create(
			server->workspaces[i]);
		if (!server->ws_sidebar_trees[i])
		{
			wlr_log(WLR_ERROR, "ws_sidebar_tree create failed");
			continue;
		}
		wlr_scene_node_set_position(
			&server->ws_sidebar_trees[i]->node, 0, 0);

		server->ws_sidebar_bgs[i] = wlr_scene_rect_create(
			server->ws_sidebar_trees[i],
			SIDEBAR_WIDTH, 4096,
			(float[]){0.0f, 0.0f, 0.0f, 0.0f});
		if (!server->ws_sidebar_bgs[i])
			wlr_log(WLR_ERROR, "sidebar_bg create failed");

		server->ws_stage_canvases[i] = wlr_scene_tree_create(
			server->workspaces[i]);
		if (!server->ws_stage_canvases[i])
		{
			wlr_log(WLR_ERROR, "ws_stage_canvas create failed");
			continue;
		}
		wlr_scene_node_set_position(
			&server->ws_stage_canvases[i]->node, SIDEBAR_WIDTH, 0);

		server->ws_stage_mgrs[i].active_stage = NULL;
		wl_list_init(&server->ws_stage_mgrs[i].inactive_stages);
	}
	server->drag_source_stage = NULL;
}

void stage_manager_destroy(struct hsdwl_server *server)
{
	for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
	{
		struct custom_stage *st, *stmp;
		wl_list_for_each_safe(st, stmp,
				&server->ws_stage_mgrs[i].inactive_stages, link)
		{
			wl_list_remove(&st->link);
			stage_free(st);
		}
		if (server->ws_stage_mgrs[i].active_stage)
		{
			stage_free(server->ws_stage_mgrs[i].active_stage);
			server->ws_stage_mgrs[i].active_stage = NULL;
		}
	}
}

void stage_manager_new_window(struct hsdwl_server *server,
		struct hsdwl_view *view)
{
	size_t ws = server->current_workspace;
	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];

	if (view->tab_group)
		return;

	/* free an empty active stage that was left behind */
	if (mgr->active_stage && wl_list_empty(&mgr->active_stage->windows))
	{
		stage_free(mgr->active_stage);
		mgr->active_stage = NULL;
	}

	if (mgr->active_stage && !wl_list_empty(&mgr->active_stage->windows))
	{
		/* push current active stage to inactive */
		stage_set_views_enabled(mgr->active_stage, false);

		while ((int)wl_list_length(&mgr->inactive_stages)
				>= MAX_INACTIVE_STAGES)
		{
			struct custom_stage *oldest =
				wl_container_of(mgr->inactive_stages.prev,
					oldest, link);
			wl_list_remove(&oldest->link);
			stage_safe_evict(oldest, server, ws);
		}
		wl_list_insert(&mgr->inactive_stages,
			&mgr->active_stage->link);
		mgr->active_stage = NULL;
	}

	/* create new stage */
	struct custom_stage *stage = calloc(1, sizeof(*stage));
	if (!stage) return;
	wl_list_init(&stage->windows);
	stage->thumb_dirty = true;

	stage->tree = wlr_scene_tree_create(
		server->ws_stage_canvases[ws]);
	if (!stage->tree)
	{
		free(stage);
		return;
	}

	stage->thumb_tree = wlr_scene_tree_create(
		server->ws_sidebar_trees[ws]);
	if (!stage->thumb_tree)
	{
		wlr_scene_node_destroy(&stage->tree->node);
		free(stage);
		return;
	}
	stage->thumb_tree->node.data = stage;

	stage->thumb_bg = wlr_scene_rect_create(
		stage->thumb_tree, SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD, 1,
		(float[]){0.0f, 0.0f, 0.0f, 0.0f});
	if (!stage->thumb_bg)
		wlr_log(WLR_ERROR, "thumb_bg create failed");

	stage->thumb_buf = wlr_scene_buffer_create(stage->thumb_tree, NULL);
	if (!stage->thumb_buf)
		wlr_log(WLR_ERROR, "thumb_buf create failed");

	/* position window — center of canvas area */
	double canvas_w = 1024, canvas_h = 768;
	if (!wl_list_empty(&server->outputs))
	{
		struct hsdwl_output *o = wl_container_of(
			server->outputs.next, o, link);
		if (o->wlr_output)
		{
			canvas_w = o->wlr_output->width - SIDEBAR_WIDTH;
			canvas_h = o->wlr_output->height;
		}
	}

	int vw = 800, vh = 600;
	if (view->xdg_surface && view->xdg_surface->configured)
	{
		vw = view->xdg_surface->geometry.width;
		vh = view->xdg_surface->geometry.height;
	}
	else if (view->xwayland_surface)
	{
		vw = view->xwayland_surface->width;
		vh = view->xwayland_surface->height;
	}

	struct custom_window *cw = calloc(1, sizeof(*cw));
	if (!cw)
	{
		stage_free(stage);
		return;
	}
	cw->view = view;
	double ox = fmax(0, (canvas_w - vw) / 2);
	double oy = fmax(0, (canvas_h - vh) / 2);
	cw->x = ox;
	cw->y = oy;
	cw->w = vw;
	cw->h = vh;
	wl_list_insert(&stage->windows, &cw->link);

	wlr_scene_node_reparent(&view->scene_tree->node, stage->tree);
	wlr_scene_node_set_position(&view->scene_tree->node, ox, oy);
	wlr_scene_node_set_enabled(&view->scene_tree->node, true);

	mgr->active_stage = stage;

	view_focus(server, view);
	stage_manager_render_sidebar(server, ws);
}

void stage_manager_notify_view_removed(struct hsdwl_server *server,
		struct hsdwl_view *view)
{
	for (size_t ws = 0; ws < HSDWL_NUM_WORKSPACES; ws++)
	{
		struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];

		if (mgr->active_stage)
		{
			struct custom_window *cw = find_custom_window(
				mgr->active_stage, view);
			if (cw)
			{
				wl_list_remove(&cw->link);
				free(cw);

				if (wl_list_empty(&mgr->active_stage->windows))
				{
					if (!wl_list_empty(
							&mgr->inactive_stages))
					{
						struct custom_stage *st =
							wl_container_of(
								mgr->inactive_stages.next,
								st, link);
						wl_list_remove(&st->link);
						wl_list_insert(
							&mgr->inactive_stages,
							&mgr->active_stage->link);
						mgr->active_stage = st;
						stage_reparent_to_canvas(st,
							server);
						struct custom_window *fcw;
						wl_list_for_each(fcw,
							&st->windows, link)
						{
							view_focus(server,
								fcw->view);
							break;
						}
					}
					else
					{
						mgr->active_stage = NULL;
					}
				}
				stage_manager_render_sidebar(server, ws);
				return;
			}
		}

		struct custom_stage *st;
		wl_list_for_each(st, &mgr->inactive_stages, link)
		{
			struct custom_window *cw = find_custom_window(st, view);
			if (cw)
			{
				wl_list_remove(&cw->link);
				free(cw);
				stage_manager_render_sidebar(server, ws);
				return;
			}
		}
	}
}

struct custom_stage *stage_at(struct hsdwl_server *server,
		double lx, double ly, size_t ws)
{
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->ws_sidebar_trees[ws]->node, lx, ly, &sx, &sy);
	if (!node) return NULL;

	struct wlr_scene_tree *tree = node->parent;
	while (tree)
	{
		if (tree->node.data)
			return tree->node.data;
		if (tree == server->ws_sidebar_trees[ws])
			break;
		tree = tree->node.parent;
	}
	return NULL;
}

void stage_manager_switch(struct hsdwl_server *server,
		struct custom_stage *target, size_t ws)
{
	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
	if (!target || target == mgr->active_stage)
		return;

	struct custom_stage *old = mgr->active_stage;
	int bw = server->config.border_width;
	int tb = server->config.titlebar_height;

	struct stage_switch_anim *ssa = calloc(1, sizeof(*ssa));
	if (!ssa)
		goto instant_switch;
	ssa->server = server;
	ssa->old_stage = old;
	ssa->new_stage = target;
	ssa->ws = ws;
	ssa->remaining = 0;
	ssa->insert_tail = false;
	ssa->n_overlays = 0;

	/* Old stage windows: animate from canvas position to thumbnail */
	if (old) {
		struct custom_window *cw;
		struct hsdwl_tab_group *seen_tg[64];
		int n_seen_tg = 0;
		wl_list_for_each(cw, &old->windows, link)
		{
			if (ssa->n_overlays >= MAX_STAGE_WINDOWS) break;

			int fw, fh, fx, fy, tw, th;

			if (cw->view && cw->view->tab_group) {
				struct hsdwl_tab_group *g = cw->view->tab_group;
				/* skip if we already made an overlay for this group */
				bool skip = false;
				for (int i = 0; i < n_seen_tg; i++)
					if (seen_tg[i] == g) { skip = true; break; }
				if (skip) continue;
				seen_tg[n_seen_tg++] = g;

				struct hsdwl_view *av = g->active;
				if (!av) continue;
				int cw_ = g->content_area_box.width;
				int ch_ = g->content_area_box.height;
				struct wlr_buffer *buf = view_capture_full_window(
					server, av, cw_, ch_, 0, 0);
				if (!buf) continue;

				fw = cw_;  fh = ch_;
				fx = SIDEBAR_WIDTH + (int)g->scene_tree->node.x;
				fy = (int)g->scene_tree->node.y
					+ g->tab_bar_thickness;

				tw = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;
				th = (int)((float)ch_ * tw / cw_);
				if (th > 300) {
					float ar = (float)cw_ / ch_;
					th = 300;
					tw = (int)(300 * ar);
				}

				struct wlr_scene_buffer *ov =
					wlr_scene_buffer_create(
						server->animation_tree, buf);
				wlr_buffer_drop(buf);
				wlr_scene_node_set_position(&ov->node,fx,fy);
				wlr_scene_buffer_set_dest_size(ov,fw,fh);
				wlr_scene_node_raise_to_top(&ov->node);
				ssa->overlays[ssa->n_overlays++] = ov;
				ssa->remaining++;
				animation_create(server, ov,200,HSDWL_EASE_OUT_QUAD,
					fx,fy,fw,fh,
					target->thumb_x,target->thumb_y,tw,th,
					stage_switch_on_anim_done,ssa);
				continue;
			}

			struct wlr_buffer *buf = view_capture_full_window(
				server, cw->view, (int)cw->w, (int)cw->h,
				bw, tb);
			if (!buf) continue;

			fw = (int)cw->w + 2 * bw;
			fh = (int)cw->h + tb + bw;
			fx = SIDEBAR_WIDTH + (int)cw->x;
			fy = (int)cw->y;

			tw = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;
			th = (int)((float)cw->h * tw / cw->w);
			if (th > 300) {
				float ar = (float)cw->w / cw->h;
				th = 300;
				tw = (int)(300 * ar);
			}

			struct wlr_scene_buffer *ov = wlr_scene_buffer_create(
				server->animation_tree, buf);
			wlr_buffer_drop(buf);
			wlr_scene_node_set_position(&ov->node, fx, fy);
			wlr_scene_buffer_set_dest_size(ov, fw, fh);
			wlr_scene_node_raise_to_top(&ov->node);

			ssa->overlays[ssa->n_overlays++] = ov;
			ssa->remaining++;

			animation_create(server, ov, 200, HSDWL_EASE_OUT_QUAD,
				fx, fy, fw, fh,
				target->thumb_x, target->thumb_y, tw, th,
				stage_switch_on_anim_done, ssa);
		}
	}

	/* Target stage windows: animate from thumbnail to canvas position */
	{
		struct custom_window *cw;
		struct hsdwl_tab_group *seen_tg[64];
		int n_seen_tg = 0;
		wl_list_for_each(cw, &target->windows, link)
		{
			if (ssa->n_overlays >= MAX_STAGE_WINDOWS) break;

			int tw, th, ttx, tty, tx, ty;

			if (cw->view && cw->view->tab_group) {
				struct hsdwl_tab_group *g = cw->view->tab_group;
				bool skip = false;
				for (int i = 0; i < n_seen_tg; i++)
					if (seen_tg[i] == g) { skip = true; break; }
				if (skip) continue;
				seen_tg[n_seen_tg++] = g;

				struct hsdwl_view *av = g->active;
				if (!av) continue;
				int cw_ = g->content_area_box.width;
				int ch_ = g->content_area_box.height;
				struct wlr_buffer *buf = view_capture_full_window(
					server, av, cw_, ch_, 0, 0);
				if (!buf) continue;

				tw = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;
				th = (int)((float)ch_ * tw / cw_);
				if (th > 300) {
					float ar = (float)cw_ / ch_;
					th = 300;
					tw = (int)(300 * ar);
				}
				ttx = SIDEBAR_WIDTH + (int)g->scene_tree->node.x;
				tty = (int)g->scene_tree->node.y
					+ g->tab_bar_thickness;
				tx = cw_;  ty = ch_;

				struct wlr_scene_buffer *ov =
					wlr_scene_buffer_create(
						server->animation_tree, buf);
				wlr_buffer_drop(buf);
				wlr_scene_node_set_position(&ov->node,
					target->thumb_x, target->thumb_y);
				wlr_scene_buffer_set_dest_size(ov, tw, th);
				wlr_scene_node_raise_to_top(&ov->node);
				ssa->overlays[ssa->n_overlays++] = ov;
				ssa->remaining++;
				animation_create(server, ov,200,HSDWL_EASE_OUT_QUAD,
					target->thumb_x,target->thumb_y,tw,th,
					ttx,tty,tx,ty,
					stage_switch_on_anim_done,ssa);
				continue;
			}

			struct wlr_buffer *buf = view_capture_full_window(
				server, cw->view, (int)cw->w, (int)cw->h,
				bw, tb);
			if (!buf) continue;

			tx = (int)cw->w + 2 * bw;
			ty = (int)cw->h + tb + bw;
			ttx = SIDEBAR_WIDTH + (int)cw->x;
			tty = (int)cw->y;

			tw = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;
			th = (int)((float)cw->h * tw / cw->w);
			if (th > 300) {
				float ar = (float)cw->w / cw->h;
				th = 300;
				tw = (int)(300 * ar);
			}

			struct wlr_scene_buffer *ov = wlr_scene_buffer_create(
				server->animation_tree, buf);
			wlr_buffer_drop(buf);
			wlr_scene_node_set_position(&ov->node,
				target->thumb_x, target->thumb_y);
			wlr_scene_buffer_set_dest_size(ov, tw, th);
			wlr_scene_node_raise_to_top(&ov->node);

			ssa->overlays[ssa->n_overlays++] = ov;
			ssa->remaining++;

			animation_create(server, ov, 200, HSDWL_EASE_OUT_QUAD,
				target->thumb_x, target->thumb_y, tw, th,
				ttx, tty, tx, ty,
				stage_switch_on_anim_done, ssa);
		}
	}

	/* hide real views (captured before hiding) */
	if (old) stage_set_views_enabled(old, false);
	stage_set_views_enabled(target, false);

	if (ssa->remaining > 0) return;

	free(ssa);

instant_switch:
	if (old) {
		stage_set_views_enabled(old, false);
		wl_list_insert(&mgr->inactive_stages, &old->link);
	}
	wl_list_remove(&target->link);
	mgr->active_stage = target;
	stage_reparent_to_canvas(target, server);
	struct custom_window *cw;
	wl_list_for_each(cw, &target->windows, link) {
		view_focus(server, cw->view);
		break;
	}
	stage_manager_render_sidebar(server, ws);
}

void stage_manager_cycle(struct hsdwl_server *server, size_t ws, bool reverse)
{
	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
	if (wl_list_empty(&mgr->inactive_stages))
		return;

	struct custom_stage *target = reverse
		? wl_container_of(mgr->inactive_stages.prev, target, link)
		: wl_container_of(mgr->inactive_stages.next,  target, link);

	if (!target || target == mgr->active_stage)
		return;

	struct custom_stage *old = mgr->active_stage;
	int bw = server->config.border_width;
	int tb = server->config.titlebar_height;

	struct stage_switch_anim *ssa = calloc(1, sizeof(*ssa));
	if (!ssa)
		goto instant_cycle;
	ssa->server = server;
	ssa->old_stage = old;
	ssa->new_stage = target;
	ssa->ws = ws;
	ssa->remaining = 0;
	ssa->insert_tail = !reverse;
	ssa->n_overlays = 0;

	/* Old stage windows: animate from canvas position to thumbnail */
	if (old) {
		struct custom_window *cw;
		struct hsdwl_tab_group *seen_tg[64];
		int n_seen_tg = 0;
		wl_list_for_each(cw, &old->windows, link)
		{
			if (ssa->n_overlays >= MAX_STAGE_WINDOWS) break;

			int fw, fh, fx, fy, tw, th;

			if (cw->view && cw->view->tab_group) {
				struct hsdwl_tab_group *g = cw->view->tab_group;
				bool skip = false;
				for (int i = 0; i < n_seen_tg; i++)
					if (seen_tg[i] == g) { skip = true; break; }
				if (skip) continue;
				seen_tg[n_seen_tg++] = g;

				struct hsdwl_view *av = g->active;
				if (!av) continue;
				int cw_ = g->content_area_box.width;
				int ch_ = g->content_area_box.height;
				struct wlr_buffer *buf = view_capture_full_window(
					server, av, cw_, ch_, 0, 0);
				if (!buf) continue;

				fw = cw_;  fh = ch_;
				fx = SIDEBAR_WIDTH + (int)g->scene_tree->node.x;
				fy = (int)g->scene_tree->node.y
					+ g->tab_bar_thickness;

				tw = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;
				th = (int)((float)ch_ * tw / cw_);
				if (th > 300) {
					float ar = (float)cw_ / ch_;
					th = 300;
					tw = (int)(300 * ar);
				}

				struct wlr_scene_buffer *ov =
					wlr_scene_buffer_create(
						server->animation_tree, buf);
				wlr_buffer_drop(buf);
				wlr_scene_node_set_position(&ov->node,fx,fy);
				wlr_scene_buffer_set_dest_size(ov,fw,fh);
				wlr_scene_node_raise_to_top(&ov->node);
				ssa->overlays[ssa->n_overlays++] = ov;
				ssa->remaining++;
				animation_create(server, ov,200,HSDWL_EASE_OUT_QUAD,
					fx,fy,fw,fh,
					target->thumb_x,target->thumb_y,tw,th,
					stage_switch_on_anim_done,ssa);
				continue;
			}

			struct wlr_buffer *buf = view_capture_full_window(
				server, cw->view, (int)cw->w, (int)cw->h,
				bw, tb);
			if (!buf) continue;

			fw = (int)cw->w + 2 * bw;
			fh = (int)cw->h + tb + bw;
			fx = SIDEBAR_WIDTH + (int)cw->x;
			fy = (int)cw->y;

			tw = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;
			th = (int)((float)cw->h * tw / cw->w);
			if (th > 300) {
				float ar = (float)cw->w / cw->h;
				th = 300;
				tw = (int)(300 * ar);
			}

			struct wlr_scene_buffer *ov = wlr_scene_buffer_create(
				server->animation_tree, buf);
			wlr_buffer_drop(buf);
			wlr_scene_node_set_position(&ov->node, fx, fy);
			wlr_scene_buffer_set_dest_size(ov, fw, fh);
			wlr_scene_node_raise_to_top(&ov->node);

			ssa->overlays[ssa->n_overlays++] = ov;
			ssa->remaining++;

			animation_create(server, ov, 200, HSDWL_EASE_OUT_QUAD,
				fx, fy, fw, fh,
				target->thumb_x, target->thumb_y, tw, th,
				stage_switch_on_anim_done, ssa);
		}
	}

	/* Target stage windows: animate from thumbnail to canvas position */
	{
		struct custom_window *cw;
		struct hsdwl_tab_group *seen_tg[64];
		int n_seen_tg = 0;
		wl_list_for_each(cw, &target->windows, link)
		{
			if (ssa->n_overlays >= MAX_STAGE_WINDOWS) break;

			int tw, th, ttx, tty, tx, ty;

			if (cw->view && cw->view->tab_group) {
				struct hsdwl_tab_group *g = cw->view->tab_group;
				bool skip = false;
				for (int i = 0; i < n_seen_tg; i++)
					if (seen_tg[i] == g) { skip = true; break; }
				if (skip) continue;
				seen_tg[n_seen_tg++] = g;

				struct hsdwl_view *av = g->active;
				if (!av) continue;
				int cw_ = g->content_area_box.width;
				int ch_ = g->content_area_box.height;
				struct wlr_buffer *buf = view_capture_full_window(
					server, av, cw_, ch_, 0, 0);
				if (!buf) continue;

				tw = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;
				th = (int)((float)ch_ * tw / cw_);
				if (th > 300) {
					float ar = (float)cw_ / ch_;
					th = 300;
					tw = (int)(300 * ar);
				}
				ttx = SIDEBAR_WIDTH + (int)g->scene_tree->node.x;
				tty = (int)g->scene_tree->node.y
					+ g->tab_bar_thickness;
				tx = cw_;  ty = ch_;

				struct wlr_scene_buffer *ov =
					wlr_scene_buffer_create(
						server->animation_tree, buf);
				wlr_buffer_drop(buf);
				wlr_scene_node_set_position(&ov->node,
					target->thumb_x, target->thumb_y);
				wlr_scene_buffer_set_dest_size(ov, tw, th);
				wlr_scene_node_raise_to_top(&ov->node);
				ssa->overlays[ssa->n_overlays++] = ov;
				ssa->remaining++;
				animation_create(server, ov,200,HSDWL_EASE_OUT_QUAD,
					target->thumb_x,target->thumb_y,tw,th,
					ttx,tty,tx,ty,
					stage_switch_on_anim_done,ssa);
				continue;
			}

			struct wlr_buffer *buf = view_capture_full_window(
				server, cw->view, (int)cw->w, (int)cw->h,
				bw, tb);
			if (!buf) continue;

			tx = (int)cw->w + 2 * bw;
			ty = (int)cw->h + tb + bw;
			ttx = SIDEBAR_WIDTH + (int)cw->x;
			tty = (int)cw->y;

			tw = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;
			th = (int)((float)cw->h * tw / cw->w);
			if (th > 300) {
				float ar = (float)cw->w / cw->h;
				th = 300;
				tw = (int)(300 * ar);
			}

			struct wlr_scene_buffer *ov = wlr_scene_buffer_create(
				server->animation_tree, buf);
			wlr_buffer_drop(buf);
			wlr_scene_node_set_position(&ov->node,
				target->thumb_x, target->thumb_y);
			wlr_scene_buffer_set_dest_size(ov, tw, th);
			wlr_scene_node_raise_to_top(&ov->node);

			ssa->overlays[ssa->n_overlays++] = ov;
			ssa->remaining++;

			animation_create(server, ov, 200, HSDWL_EASE_OUT_QUAD,
				target->thumb_x, target->thumb_y, tw, th,
				ttx, tty, tx, ty,
				stage_switch_on_anim_done, ssa);
		}
	}

	/* hide real views */
	if (old) stage_set_views_enabled(old, false);
	stage_set_views_enabled(target, false);

	if (ssa->remaining > 0) return;

	free(ssa);

instant_cycle:
	if (old) stage_set_views_enabled(old, false);
	wl_list_remove(&target->link);
	if (reverse)
		wl_list_insert(&mgr->inactive_stages,
			&old->link);
	else
		wl_list_insert(mgr->inactive_stages.prev,
			&old->link);
	mgr->active_stage = target;
	stage_reparent_to_canvas(target, server);
	struct custom_window *cw;
	wl_list_for_each(cw, &target->windows, link) {
		view_focus(server, cw->view);
		break;
	}
	stage_manager_render_sidebar(server, ws);
}

void stage_manager_merge(struct hsdwl_server *server,
		struct custom_stage *source, size_t ws)
{
	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
	if (!source || !mgr->active_stage || source == mgr->active_stage)
		return;

	int bw = server->config.border_width;
	int tb = server->config.titlebar_height;

	struct stage_merge_anim *sma = calloc(1, sizeof(*sma));
	if (!sma)
		goto instant_merge;
	sma->server = server;
	sma->source = source;
	sma->ws = ws;
	sma->remaining = 0;
	sma->n_overlays = 0;

	/* Source stage windows: animate from thumbnail to canvas */
	{
		struct custom_window *scw;
		struct hsdwl_tab_group *seen_tg[64];
		int n_seen_tg = 0;
		wl_list_for_each(scw, &source->windows, link)
		{
			if (sma->n_overlays >= MAX_STAGE_WINDOWS) break;

			int tx, ty, ttx, tty, tw, th;

			if (scw->view && scw->view->tab_group) {
				struct hsdwl_tab_group *g = scw->view->tab_group;
				bool skip = false;
				for (int i = 0; i < n_seen_tg; i++)
					if (seen_tg[i] == g) { skip = true; break; }
				if (skip) continue;
				seen_tg[n_seen_tg++] = g;

				struct hsdwl_view *av = g->active;
				if (!av) continue;
				int cw_ = g->content_area_box.width;
				int ch_ = g->content_area_box.height;
				struct wlr_buffer *buf = view_capture_full_window(
					server, av, cw_, ch_, 0, 0);
				if (!buf) continue;

				tw = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;
				th = (int)((float)ch_ * tw / cw_);
				if (th > 300) {
					float ar = (float)cw_ / ch_;
					th = 300;
					tw = (int)(300 * ar);
				}
				ttx = SIDEBAR_WIDTH + (int)g->scene_tree->node.x;
				tty = (int)g->scene_tree->node.y
					+ g->tab_bar_thickness;
				tx = cw_;  ty = ch_;

				struct wlr_scene_buffer *ov =
					wlr_scene_buffer_create(
						server->animation_tree, buf);
				wlr_buffer_drop(buf);
				wlr_scene_node_set_position(&ov->node,
					source->thumb_x, source->thumb_y);
				wlr_scene_buffer_set_dest_size(ov, tw, th);
				wlr_scene_node_raise_to_top(&ov->node);
				sma->overlays[sma->n_overlays++] = ov;
				sma->remaining++;
				animation_create(server, ov,200,HSDWL_EASE_OUT_QUAD,
					source->thumb_x,source->thumb_y,tw,th,
					ttx,tty,tx,ty,
					stage_merge_on_anim_done,sma);
				continue;
			}

			struct wlr_buffer *buf = view_capture_full_window(
				server, scw->view, (int)scw->w, (int)scw->h,
				bw, tb);
			if (!buf) continue;

			tx = (int)scw->w + 2 * bw;
			ty = (int)scw->h + tb + bw;
			ttx = SIDEBAR_WIDTH + (int)scw->x;
			tty = (int)scw->y;

			tw = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;
			th = (int)((float)scw->h * tw / scw->w);
			if (th > 300) {
				float ar = (float)scw->w / scw->h;
				th = 300;
				tw = (int)(300 * ar);
			}

			struct wlr_scene_buffer *ov = wlr_scene_buffer_create(
				server->animation_tree, buf);
			wlr_buffer_drop(buf);
			wlr_scene_node_set_position(&ov->node,
				source->thumb_x, source->thumb_y);
			wlr_scene_buffer_set_dest_size(ov, tw, th);
			wlr_scene_node_raise_to_top(&ov->node);

			sma->overlays[sma->n_overlays++] = ov;
			sma->remaining++;

			animation_create(server, ov, 200, HSDWL_EASE_OUT_QUAD,
				source->thumb_x, source->thumb_y, tw, th,
				ttx, tty, tx, ty,
				stage_merge_on_anim_done, sma);
		}
	}

	/* hide source views (captured before hiding) */
	stage_set_views_enabled(source, false);

	if (sma->remaining > 0) return;

	/* no overlays created */
	free(sma);

instant_merge:
	;
	struct custom_window *cw, *tmp;
	wl_list_for_each_safe(cw, tmp, &source->windows, link)
	{
		wl_list_remove(&cw->link);
		if (cw->view && cw->view->scene_tree)
		{
			wlr_scene_node_reparent(
				&cw->view->scene_tree->node,
				mgr->active_stage->tree);
			wlr_scene_node_set_position(
				&cw->view->scene_tree->node,
				cw->x, cw->y);
			wlr_scene_node_set_enabled(
				&cw->view->scene_tree->node, true);
		}
		wl_list_insert(&mgr->active_stage->windows, &cw->link);
	}

	wl_list_remove(&source->link);
	stage_free(source);

	stage_manager_render_sidebar(server, ws);
}

static void stage_hide_thumb(struct custom_stage *st, bool hide)
{
	if (st && st->thumb_tree)
		wlr_scene_node_set_enabled(&st->thumb_tree->node, !hide);
}

void stage_manager_render_sidebar(struct hsdwl_server *server, size_t ws)
{
	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
	int thumb_w = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;

	/* active stage never shows in the sidebar */
	stage_hide_thumb(mgr->active_stage, true);

	/* hide every inactive stage thumbnail first */
	struct custom_stage *st;
	wl_list_for_each(st, &mgr->inactive_stages, link)
		stage_hide_thumb(st, true);

	/* collect stages with their app name and dimensions */
	struct entry {
		struct custom_stage *st;
		const char *app;
		int tw, th, gap;
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
		int th = (int)(bbox.height * (float)tw / bbox.width);
		if (th > 300)
		{
			float ar = (float)bbox.width / bbox.height;
			th = 300;
			tw = (int)(300 * ar);
			if (tw < 20) tw = 20;
		}
		int gap = STAGE_THUMB_GAP + th / 8;

		entries[nentries].st = st;
		entries[nentries].app = stage_get_app_name(st);
		entries[nentries].tw = tw;
		entries[nentries].th = th;
		entries[nentries].gap = gap;
		nentries++;
	}
	if (nentries == 0) return;

	/* total height — each entry at its own natural height */
	int total_h = 0;
	for (int i = 0; i < nentries; i++)
	{
		total_h += entries[i].th;
		if (i > 0) total_h += entries[i].gap;
	}

	int sidebar_h = 1050;
	if (!wl_list_empty(&server->outputs))
	{
		struct hsdwl_output *o = wl_container_of(
			server->outputs.next, o, link);
		if (o->wlr_output)
			sidebar_h = o->wlr_output->height;
	}
	int y = (sidebar_h - total_h) / 2;
	if (y < STAGE_THUMB_PAD) y = STAGE_THUMB_PAD;

	/* render: same-app stages get a horizontal offset */
	for (int i = 0; i < nentries; i++)
	{
		bool same_as_prev = (i > 0
			&& strcmp(entries[i].app, entries[i-1].app) == 0);
		int x = STAGE_THUMB_PAD
			+ (same_as_prev ? STAGE_GROUP_OFFSET : 0);

		stage_hide_thumb(entries[i].st, false);
		stage_render_thumbnail(server, entries[i].st,
			entries[i].tw, entries[i].th);
		wlr_scene_node_set_position(
			&entries[i].st->thumb_tree->node, x, y);
		entries[i].st->thumb_x = x;
		entries[i].st->thumb_y = y;
		y += entries[i].th + entries[i].gap;
		entries[i].st->thumb_dirty = false;
	}
}

static void cw_update_geometry(struct custom_window *cw,
		struct hsdwl_view *view)
{
	if (view->xdg_surface && view->xdg_surface->configured)
	{
		cw->w = view->xdg_surface->geometry.width;
		cw->h = view->xdg_surface->geometry.height;
	}
	else if (view->xwayland_surface)
	{
		cw->w = view->xwayland_surface->width;
		cw->h = view->xwayland_surface->height;
	}
	if (view->scene_tree)
	{
		cw->x = view->scene_tree->node.x;
		cw->y = view->scene_tree->node.y;
	}
}

void stage_manager_notify_surface_commit(struct hsdwl_server *server,
		struct hsdwl_view *view)
{
	for (size_t ws = 0; ws < HSDWL_NUM_WORKSPACES; ws++)
	{
		struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
		if (!mgr->active_stage) continue;

		/* always sync dimensions — even for the active stage — so
		   the thumbnail later uses the correct size */
		struct custom_window *cw = NULL;
		struct custom_stage *st = NULL;

		if ((cw = find_custom_window(mgr->active_stage, view)))
			cw_update_geometry(cw, view);
		else
		{
			wl_list_for_each(st, &mgr->inactive_stages, link)
			{
				if ((cw = find_custom_window(st, view)))
				{
					cw_update_geometry(cw, view);
					break;
				}
			}
		}
		/* no match → view isn't managed by stage manager */
		if (!cw) continue;

		/* if the view is in an inactive stage, re-render thumbnail */
		if (!st) return; /* active stage — no thumbnail to update */
		if (!wl_list_empty(&st->windows))
		{
			int thumb_w = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;
			struct wlr_box bbox = {0};
			bool first = true;
			struct custom_window *w;
			wl_list_for_each(w, &st->windows, link)
			{
				if (first)
				{
					bbox.x = w->x; bbox.y = w->y;
					bbox.width = w->w; bbox.height = w->h;
					first = false;
				}
				else
				{
					double x1 = fmin(bbox.x, w->x);
					double y1 = fmin(bbox.y, w->y);
					double x2 = fmax(bbox.x + bbox.width,
						w->x + w->w);
					double y2 = fmax(bbox.y + bbox.height,
						w->y + w->h);
					bbox.x = x1; bbox.y = y1;
					bbox.width = x2 - x1;
					bbox.height = y2 - y1;
				}
			}
			if (bbox.width < 1 || bbox.height < 1) return;

			int thumb_h = (int)(bbox.height * (float)thumb_w / bbox.width);
			if (thumb_h > 300)
			{
				float ar = (float)bbox.width / bbox.height;
				thumb_h = 300;
				thumb_w = (int)(300 * ar);
				if (thumb_w < 20) thumb_w = 20;
			}

			stage_render_thumbnail(server, st, thumb_w, thumb_h);
		}
		return;
	}
}

void stage_manager_migrate_existing(struct hsdwl_server *server)
{
	for (size_t ws = 0; ws < HSDWL_NUM_WORKSPACES; ws++)
	{
		struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];

		struct hsdwl_view *views[128];
		int nviews = 0;
		struct hsdwl_view *v;
		wl_list_for_each(v, &server->views, link)
		{
			if (nviews >= 128) break;
			if (!v->scene_tree || !v->scene_tree->node.enabled)
				continue;
			if (v->tab_group) continue;
			if (v->scene_tree->node.parent != server->workspaces[ws])
				continue;
			views[nviews++] = v;
		}

		if (nviews == 0) continue;

		struct hsdwl_view *focused = server->focused_views[ws];
		bool found_focused = false;
		int focused_idx = -1;
		for (int i = 0; i < nviews; i++)
		{
			if (views[i] == focused)
			{
				found_focused = true;
				focused_idx = i;
				break;
			}
		}
		if (!found_focused && nviews > 0)
		{
			focused = views[0];
			focused_idx = 0;
		}

		struct custom_stage *active = calloc(1, sizeof(*active));
		if (!active) return;
		wl_list_init(&active->windows);
		active->thumb_dirty = true;

		active->tree = wlr_scene_tree_create(
			server->ws_stage_canvases[ws]);
		if (!active->tree) { free(active); return; }

		active->thumb_tree = wlr_scene_tree_create(
			server->ws_sidebar_trees[ws]);
		if (!active->thumb_tree)
		{
			wlr_scene_node_destroy(&active->tree->node);
			free(active);
			return;
		}
		active->thumb_tree->node.data = active;

		active->thumb_bg = wlr_scene_rect_create(active->thumb_tree,
			SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD, 1,
			(float[]){0.0f, 0.0f, 0.0f, 0.0f});
		active->thumb_buf = wlr_scene_buffer_create(
			active->thumb_tree, NULL);

		struct custom_window *acw = calloc(1, sizeof(*acw));
		if (!acw) { stage_free(active); return; }
		acw->view = focused;
		if (focused->xdg_surface)
		{
			acw->w = focused->xdg_surface->geometry.width;
			acw->h = focused->xdg_surface->geometry.height;
		}
		else if (focused->xwayland_surface)
		{
			acw->w = focused->xwayland_surface->width;
			acw->h = focused->xwayland_surface->height;
		}
		acw->x = focused->scene_tree->node.x;
		acw->y = focused->scene_tree->node.y;
		wl_list_insert(&active->windows, &acw->link);

		wlr_scene_node_reparent(&focused->scene_tree->node,
			active->tree);
		wlr_scene_node_set_enabled(
			&focused->scene_tree->node, true);

		mgr->active_stage = active;

		int inactive_count = 0;
		for (int i = 0; i < nviews; i++)
		{
			if (i == focused_idx) continue;
			if (inactive_count >= MAX_INACTIVE_STAGES) break;

			struct hsdwl_view *ov = views[i];
			struct custom_stage *st = calloc(1, sizeof(*st));
			if (!st) break;
			wl_list_init(&st->windows);
			st->thumb_dirty = true;

			st->tree = wlr_scene_tree_create(
				server->ws_stage_canvases[ws]);
			if (!st->tree) { free(st); break; }

			st->thumb_tree = wlr_scene_tree_create(
				server->ws_sidebar_trees[ws]);
			if (!st->thumb_tree)
			{
				wlr_scene_node_destroy(&st->tree->node);
				free(st);
				break;
			}
			st->thumb_tree->node.data = st;

			st->thumb_bg = wlr_scene_rect_create(
				st->thumb_tree,
				SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD,
				1, (float[]){0.0f, 0.0f, 0.0f, 0.0f});
			st->thumb_buf = wlr_scene_buffer_create(
				st->thumb_tree, NULL);

			struct custom_window *ocw = calloc(1, sizeof(*ocw));
			if (!ocw) { stage_free(st); break; }
			ocw->view = ov;
			if (ov->xdg_surface)
			{
				ocw->w = ov->xdg_surface->geometry.width;
				ocw->h = ov->xdg_surface->geometry.height;
			}
			else if (ov->xwayland_surface)
			{
				ocw->w = ov->xwayland_surface->width;
				ocw->h = ov->xwayland_surface->height;
			}
			ocw->x = ov->scene_tree->node.x;
			ocw->y = ov->scene_tree->node.y;
			wl_list_insert(&st->windows, &ocw->link);

			wlr_scene_node_reparent(&ov->scene_tree->node,
				st->tree);
			wlr_scene_node_set_enabled(
				&ov->scene_tree->node, false);

			wl_list_insert(&mgr->inactive_stages, &st->link);
			inactive_count++;
		}

		stage_manager_render_sidebar(server, ws);
	}
}
