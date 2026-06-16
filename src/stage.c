#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "stage.h"
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

/* ── helpers ── */

static void stage_set_views_enabled(struct custom_stage *stage, bool enabled)
{
	struct custom_window *cw;
	wl_list_for_each(cw, &stage->windows, link)
	{
		if (cw->view && cw->view->scene_tree)
			wlr_scene_node_set_enabled(
				&cw->view->scene_tree->node, enabled);
	}
}

static void stage_reparent_to_canvas(struct custom_stage *stage,
		struct hsdwl_server *server)
{
	(void)server;
	struct custom_window *cw;
	wl_list_for_each(cw, &stage->windows, link)
	{
		if (cw->view && cw->view->scene_tree)
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

/* ── offscreen thumbnail rendering ── */

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

	/* allocate offscreen buffer at thumbnail size */
	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format fmt = {
		.format = DRM_FORMAT_ARGB8888,
		.len = 1,
		.modifiers = mods,
	};
	struct wlr_buffer *buf = wlr_allocator_create_buffer(
		server->allocator, thumb_w, thumb_h, &fmt);
	if (!buf)
	{
		wlr_log(WLR_ERROR, "allocator create buffer failed");
		return;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, buf, NULL);
	if (!pass)
	{
		wlr_log(WLR_ERROR, "begin buffer pass failed");
		wlr_buffer_drop(buf);
		return;
	}

	/* clear with fully transparent */
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

		/* render actual window content texture, scaled down */
		struct wlr_surface *surface = view_get_surface(cw->view);
		if (surface)
		{
			struct wlr_texture *texture =
				wlr_surface_get_texture(surface);
			if (texture)
			{
				/* use the texture's actual buffer dimensions
				   so the aspect ratio is always correct even
				   when buffer != geometry (e.g. CSD windows) */
				float tex_w = surface->current.width;
				float tex_h = surface->current.height;
				if (tex_w < 1 || tex_h < 1)
				{
					tex_w = cw->w;
					tex_h = cw->h;
				}
				float fit_scale = fmin(
					sw / tex_w, sh / tex_h);
				float fit_w = tex_w * fit_scale;
				float fit_h = tex_h * fit_scale;
				float fit_x = sx + (sw - fit_w) / 2;
				float fit_y = sy + (sh - fit_h) / 2;

				const float tex_alpha = 1.0f;
				wlr_render_pass_add_texture(pass,
					&(struct wlr_render_texture_options){
					.texture = texture,
					.dst_box = {
						.x = (int)(fit_x + 0.5f),
						.y = (int)(fit_y + 0.5f),
						.width = (int)(fit_w + 0.5f),
						.height = (int)(fit_h + 0.5f),
					},
					.alpha = &tex_alpha,
					.transform = WL_OUTPUT_TRANSFORM_NORMAL,
				});
			}
		}
	}

	if (!wlr_render_pass_submit(pass))
		wlr_log(WLR_ERROR, "render pass submit failed");

	/* display the rendered thumbnail in the sidebar */
	wlr_scene_buffer_set_buffer(stage->thumb_buf, buf);
	wlr_scene_buffer_set_dest_size(stage->thumb_buf, thumb_w, thumb_h);
	wlr_buffer_drop(buf);
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
	cw->x = fmax(0, (canvas_w - vw) / 2);
	cw->y = fmax(0, (canvas_h - vh) / 2);
	cw->w = vw;
	cw->h = vh;
	wl_list_insert(&stage->windows, &cw->link);

	wlr_scene_node_reparent(&view->scene_tree->node, stage->tree);
	wlr_scene_node_set_position(&view->scene_tree->node, cw->x, cw->y);
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

	if (mgr->active_stage)
	{
		stage_set_views_enabled(mgr->active_stage, false);
		wl_list_insert(&mgr->inactive_stages,
			&mgr->active_stage->link);
	}

	wl_list_remove(&target->link);
	mgr->active_stage = target;
	stage_reparent_to_canvas(target, server);

	struct custom_window *cw;
	wl_list_for_each(cw, &target->windows, link)
	{
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

	/* active stage never shows in the sidebar */
	stage_hide_thumb(mgr->active_stage, true);

	/* hide every inactive stage thumbnail first; we'll re-enable only
	   the ones that have windows and are actually rendered below */
	struct custom_stage *st;
	wl_list_for_each(st, &mgr->inactive_stages, link)
		stage_hide_thumb(st, true);

	int thumb_w = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;

	/* first pass: compute thumbnail dimensions and total height */
	struct thumb_info {
		int h, w, gap;
		struct custom_stage *st;
	} infos[64];
	int ninfo = 0;
	int total_h = 0;

	wl_list_for_each(st, &mgr->inactive_stages, link)
	{
		if (wl_list_empty(&st->windows) || ninfo >= 64)
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
		if (bbox.width < 1 || bbox.height < 1)
			continue;

		int tw = thumb_w;
		int th = (int)(bbox.height * (float)tw / bbox.width);
		if (th > 300)
		{
			float ar = (float)bbox.width / bbox.height;
			th = 300;
			tw = (int)(300 * ar);
			if (tw < 20) tw = 20;
		}

		infos[ninfo].h = th;
		infos[ninfo].w = tw;
		infos[ninfo].gap = STAGE_THUMB_GAP + th / 8;
		infos[ninfo].st = st;
		ninfo++;
		total_h += th;
	}
	if (ninfo == 0) return;

	for (int i = 0; i < ninfo; i++)
		total_h += infos[i].gap;
	total_h -= infos[ninfo - 1].gap;

	/* get sidebar height from the first output */
	int sidebar_h = 1050; /* fallback */
	if (!wl_list_empty(&server->outputs))
	{
		struct hsdwl_output *o = wl_container_of(
			server->outputs.next, o, link);
		if (o->wlr_output)
			sidebar_h = o->wlr_output->height;
	}
	int y = (sidebar_h - total_h) / 2;
	if (y < STAGE_THUMB_PAD) y = STAGE_THUMB_PAD;

	/* second pass: render and position */
	for (int i = 0; i < ninfo; i++)
	{
		struct custom_stage *st = infos[i].st;
		int tw = infos[i].w;
		int th = infos[i].h;
		int gap = infos[i].gap;

		stage_hide_thumb(st, false);
		stage_render_thumbnail(server, st, tw, th);

		wlr_scene_node_set_position(
			&st->thumb_tree->node, STAGE_THUMB_PAD, y);

		y += th + gap;
		st->thumb_dirty = false;
	}
}

void stage_manager_notify_surface_commit(struct hsdwl_server *server,
		struct hsdwl_view *view)
{
	for (size_t ws = 0; ws < HSDWL_NUM_WORKSPACES; ws++)
	{
		struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
		if (!mgr->active_stage || wl_list_empty(&mgr->inactive_stages))
			continue;

		/* find which inactive stage this view belongs to */
		struct custom_stage *st;
		struct custom_window *cw = NULL;
		wl_list_for_each(st, &mgr->inactive_stages, link)
		{
			cw = find_custom_window(st, view);
			if (cw) break;
		}
		if (!cw) continue;

		/* update cached dimensions from the newly committed buffer */
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

		/* re-render just this stage's thumbnail */
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
