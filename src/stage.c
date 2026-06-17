#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "stage.h"
#include "stage-sidebar.h"
#include "animation.h"
#include "output.h"
#include "server.h"
#include "view.h"
#include "view-maximize.h"

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

void stage_set_views_enabled(struct custom_stage *stage, bool enabled)
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

void stage_reparent_to_canvas(struct custom_stage *stage,
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

void stage_safe_evict(struct custom_stage *stage,
		struct hsdwl_server *server, size_t ws)
{
	if (!stage) return;

	wl_list_remove(&stage->link);

	struct custom_window *cw, *tmp;
	wl_list_for_each_safe(cw, tmp, &stage->windows, link)
	{
		wl_list_remove(&cw->link);
		if (cw->view && cw->view->scene_tree)
		{
			wlr_scene_node_reparent(
				&cw->view->scene_tree->node,
				server->workspaces[ws]);
			wlr_scene_node_set_position(
				&cw->view->scene_tree->node,
				cw->x, cw->y);
			wlr_scene_node_set_enabled(
				&cw->view->scene_tree->node, true);
		}
		free(cw);
	}

	if (stage->thumb_tree)
		wlr_scene_node_destroy(&stage->thumb_tree->node);
	else if (stage->tree)
		wlr_scene_node_destroy(&stage->tree->node);

	free(stage);
}

struct custom_window *find_custom_window(struct custom_stage *stage,
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

void stage_free(struct custom_stage *stage)
{
	if (!stage) return;

	struct custom_window *cw, *tmp;
	wl_list_for_each_safe(cw, tmp, &stage->windows, link)
	{
		wl_list_remove(&cw->link);
		free(cw);
	}

	if (stage->thumb_tree)
		wlr_scene_node_destroy(&stage->thumb_tree->node);
	else if (stage->tree)
		wlr_scene_node_destroy(&stage->tree->node);

	free(stage);
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
			&server->ws_stage_canvases[i]->node,
			SIDEBAR_WIDTH, 0);

		server->ws_stage_mgrs[i].active_stage = NULL;
		wl_list_init(&server->ws_stage_mgrs[i].inactive_stages);
	}
}

void stage_manager_destroy(struct hsdwl_server *server)
{
	for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
	{
		struct custom_stage *st, *tmp;
		wl_list_for_each_safe(st, tmp,
			&server->ws_stage_mgrs[i].inactive_stages, link)
		{
			wl_list_remove(&st->link);
			stage_free(st);
		}
		if (server->ws_stage_mgrs[i].active_stage)
			stage_free(
				server->ws_stage_mgrs[i].active_stage);
	}
}

static void stage_new_window_anim_done(struct hsdwl_server *server,
		void *user_data)
{
	struct hsdwl_view *view = user_data;
	(void)server;
	if (view && view->anim_overlay)
	{
		wlr_scene_node_destroy(&view->anim_overlay->node);
		view->anim_overlay = NULL;
	}
	if (view && view->scene_tree)
		wlr_scene_node_set_enabled(&view->scene_tree->node, true);
}

void stage_manager_new_window(struct hsdwl_server *server,
		struct hsdwl_view *view)
{
	size_t ws = server->current_workspace;
	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];

	/* If there is an existing active stage, push it to inactive */
	if (mgr->active_stage)
	{
		/* evict oldest if we exceed the maximum */
		if (wl_list_length(&mgr->inactive_stages) >= MAX_INACTIVE_STAGES)
		{
			struct custom_stage *oldest = wl_container_of(
				mgr->inactive_stages.next, oldest, link);
			stage_safe_evict(oldest, server, ws);
		}

		wl_list_insert(&mgr->inactive_stages,
			&mgr->active_stage->link);
		stage_set_views_enabled(mgr->active_stage, false);
		mgr->active_stage = NULL;
	}

	/* Create a new stage for this view */
	struct custom_stage *stage = calloc(1, sizeof(*stage));
	if (!stage) return;

	wl_list_init(&stage->windows);

	struct custom_window *cw = calloc(1, sizeof(*cw));
	if (!cw) { free(stage); return; }

	cw->view = view;
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
	if (cw->w < 1) cw->w = 800;
	if (cw->h < 1) cw->h = 600;
	wl_list_insert(&stage->windows, &cw->link);

	stage->tree = wlr_scene_tree_create(
		server->ws_stage_canvases[ws]);
	if (!stage->tree)
	{
		free(cw);
		free(stage);
		return;
	}
	stage->tree->node.data = stage;

	/* center the view on the canvas */
	int bw = server->config.border_width;
	int tb = server->config.titlebar_height;
	int scene_w = 1920, scene_h = 1080;
	if (!wl_list_empty(&server->outputs)) {
		struct hsdwl_output *o = wl_container_of(
			server->outputs.next, o, link);
		if (o->wlr_output) {
			scene_w = o->wlr_output->width;
			scene_h = o->wlr_output->height;
		}
	}
	double canvas_x = (scene_w - SIDEBAR_WIDTH
		- cw->w - 2 * bw) / 2.0;
	double canvas_y = (scene_h - cw->h - tb - bw) / 2.0;
	if (canvas_x < 0) canvas_x = 0;
	if (canvas_y < 0) canvas_y = 0;
	cw->x = canvas_x;
	cw->y = canvas_y;

	/* thumbnail tree */
	stage->thumb_tree = wlr_scene_tree_create(
		server->ws_sidebar_trees[ws]);
	if (!stage->thumb_tree)
	{
		wlr_log(WLR_ERROR, "thumb_bg create failed");
	}
	else
	{
		stage->thumb_tree->node.data = stage;
		stage->thumb_bg = wlr_scene_rect_create(
			stage->thumb_tree, 40, 40,
			(float[]){0.0f, 0.0f, 0.0f, 0.3f});
		stage->thumb_buf = wlr_scene_buffer_create(
			stage->thumb_tree, NULL);
		if (!stage->thumb_buf)
			wlr_log(WLR_ERROR, "thumb_buf create failed");
	}

	/* Make sure view's scene_tree is reparented into our canvas */
	if (view->scene_tree)
	{
		wlr_scene_node_reparent(&view->scene_tree->node,
			stage->tree);
		wlr_scene_node_set_position(&view->scene_tree->node,
			cw->x, cw->y);
	}

	mgr->active_stage = stage;

	/* Pop-in animation: tiny dot at center of screen flies to final pos and grows */
	{
		int cap_h = cw->h;
		int cap_w = cw->w;
		int anim_w = cap_w + 2 * bw;
		int anim_h = cap_h + tb + bw;
		if (anim_w < 1) anim_w = 1;
		if (anim_h < 1) anim_h = 1;
		int abs_x = SIDEBAR_WIDTH + (int)cw->x;
		int abs_y = (int)cw->y;

		struct wlr_buffer *cap = view_capture_full_window(
			server, view, cap_w, cap_h, bw, tb);
		if (cap)
		{
			int sx = (scene_w - 10) / 2;
			int sy = (scene_h - 10) / 2;
			view->anim_overlay = wlr_scene_buffer_create(
				server->animation_tree, cap);
			wlr_buffer_drop(cap);
			if (view->anim_overlay)
			{
				wlr_scene_node_set_position(
					&view->anim_overlay->node, sx, sy);
				wlr_scene_buffer_set_dest_size(
					view->anim_overlay, 10, 10);
				wlr_scene_node_raise_to_top(
					&view->anim_overlay->node);
				animation_create(server, view->anim_overlay,
					200, HSDWL_EASE_BEZIER,
					sx, sy, 10, 10,
					abs_x, abs_y, anim_w, anim_h,
					stage_new_window_anim_done, view);
				wlr_scene_node_set_enabled(
					&view->scene_tree->node, false);
			}
		}
	}

	view_focus(server, view);
	stage_manager_render_sidebar(server, ws);
}

void stage_manager_notify_view_removed(struct hsdwl_server *server,
		struct hsdwl_view *view)
{
	for (size_t ws = 0; ws < HSDWL_NUM_WORKSPACES; ws++)
	{
		struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
		if (!mgr->active_stage) continue;

		struct custom_window *cw = find_custom_window(
			mgr->active_stage, view);
		if (!cw)
		{
			struct custom_stage *st;
			wl_list_for_each(st, &mgr->inactive_stages, link)
			{
				cw = find_custom_window(st, view);
				if (cw) break;
			}
			if (cw)
			{
				wl_list_remove(&cw->link);
				free(cw);
				if (find_custom_window(st, NULL) == NULL
						&& wl_list_empty(&st->windows))
				{
					/* Stage now empty, evict it */
					wl_list_remove(&st->link);
					stage_reparent_to_canvas(st, server);
					stage_free(st);
				}
			}
			continue;
		}

		wl_list_remove(&cw->link);
		free(cw);

		if (wl_list_empty(&mgr->active_stage->windows))
		{
			/* Active stage is now empty; promote an inactive stage */
			if (!wl_list_empty(&mgr->inactive_stages))
			{
				struct custom_stage *promote = wl_container_of(
					mgr->inactive_stages.next, promote, link);
				wl_list_remove(&promote->link);
				stage_free(mgr->active_stage);
				mgr->active_stage = promote;
				stage_reparent_to_canvas(promote, server);
				struct custom_window *cw2;
				wl_list_for_each(cw2, &promote->windows, link)
				{
					view_focus(server, cw2->view);
					break;
				}
			}
			else
			{
				stage_free(mgr->active_stage);
				mgr->active_stage = NULL;
			}
		}
		stage_manager_render_sidebar(server, ws);
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
		{
			struct custom_stage *s = tree->node.data;
			return s;
		}
		if (!tree->node.parent) break;
		tree = tree->node.parent;
	}
	return NULL;
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

		struct custom_window *cw = find_custom_window(
			mgr->active_stage, view);
		if (cw)
		{
			cw_update_geometry(cw, view);
			return;
		}

		struct custom_stage *st;
		wl_list_for_each(st, &mgr->inactive_stages, link)
		{
			struct custom_window *cw2 = find_custom_window(st, view);
			if (cw2)
			{
				cw_update_geometry(cw2, view);
				int thumb_w = SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD;
				int thumb_h = (int)(cw2->h * (float)thumb_w / cw2->w);
				if (thumb_h > 300) {
					float ar = (float)cw2->w / cw2->h;
					thumb_h = 300;
					thumb_w = (int)(300 * ar);
				}
				stage_render_thumbnail(server, st,
					thumb_w, thumb_h);
				return;
			}
		}
	}
}

void stage_manager_migrate_existing(struct hsdwl_server *server)
{
	for (size_t ws = 0; ws < HSDWL_NUM_WORKSPACES; ws++)
	{
		struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
		mgr->active_stage = NULL;
		wl_list_init(&mgr->inactive_stages);

		/* Collect all non-tab-group views under this workspace */
		struct hsdwl_view *view;
		struct hsdwl_view *first_view = NULL;
		int nviews = 0;
		wl_list_for_each(view, &server->views, link)
		{
			if (view->tab_group) continue;
			if (!view->scene_tree) continue;
			if (view->scene_tree->node.parent
				!= server->workspaces[ws]) continue;

			if (!first_view)
				first_view = view;
			nviews++;
		}
		if (nviews == 0) continue;

		/* Build first stage from the focused view */
		struct custom_stage *active = calloc(1, sizeof(*active));
		if (!active) continue;
		wl_list_init(&active->windows);

		if (first_view)
		{
			struct custom_window *acw = calloc(1, sizeof(*acw));
			if (!acw) { stage_free(active); continue; }
			acw->view = first_view;
			cw_update_geometry(acw, first_view);
			if (first_view->scene_tree)
			{
				wlr_scene_node_reparent(
					&first_view->scene_tree->node,
					active->tree);
				wlr_scene_node_set_position(
					&first_view->scene_tree->node,
					acw->x, acw->y);
			}
			wl_list_insert(&active->windows, &acw->link);
		}

		active->tree = wlr_scene_tree_create(
			server->ws_stage_canvases[ws]);
		if (!active->tree) { stage_free(active); continue; }
		active->tree->node.data = active;

		active->thumb_tree = wlr_scene_tree_create(
			server->ws_sidebar_trees[ws]);
		if (active->thumb_tree)
			active->thumb_tree->node.data = active;

		active->thumb_bg = wlr_scene_rect_create(
			active->thumb_tree, 40, 40,
			(float[]){0.0f, 0.0f, 0.0f, 0.3f});
		active->thumb_buf = wlr_scene_buffer_create(
			active->thumb_tree, NULL);

		mgr->active_stage = active;

		/* Remaining views become inactive stages */
		wl_list_for_each(view, &server->views, link)
		{
			if (view == first_view) continue;
			if (view->tab_group) continue;
			if (!view->scene_tree) continue;
			if (view->scene_tree->node.parent
				!= server->workspaces[ws]) continue;

			struct custom_stage *st = calloc(1, sizeof(*st));
			if (!st) break;
			wl_list_init(&st->windows);

			st->tree = wlr_scene_tree_create(
				server->ws_stage_canvases[ws]);
			if (!st->tree) { free(st); break; }
			st->tree->node.data = st;

			st->thumb_tree = wlr_scene_tree_create(
				server->ws_sidebar_trees[ws]);
			if (st->thumb_tree)
			{
				st->thumb_tree->node.data = st;
				st->thumb_bg = wlr_scene_rect_create(
					st->thumb_tree, 40, 40,
					(float[]){0.0f, 0.0f, 0.0f, 0.3f});
				st->thumb_buf = wlr_scene_buffer_create(
					st->thumb_tree, NULL);
			}

			struct custom_window *ocw = calloc(1, sizeof(*ocw));
			if (!ocw) { stage_free(st); break; }
			ocw->view = view;
			cw_update_geometry(ocw, view);
			if (view->scene_tree)
			{
				wlr_scene_node_reparent(
					&view->scene_tree->node, st->tree);
				wlr_scene_node_set_position(
					&view->scene_tree->node,
					ocw->x, ocw->y);
			}
			wl_list_insert(&st->windows, &ocw->link);

			wlr_scene_node_set_enabled(
				&view->scene_tree->node, false);

			wl_list_insert(&mgr->inactive_stages, &st->link);
		}

		stage_manager_render_sidebar(server, ws);
	}
}
