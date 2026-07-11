#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "stage.h"
#include "stage-sidebar.h"
#include "animation.h"
#include "layer-shell.h"
#include "output.h"
#include "server.h"
#include "deco.h"
#include "view.h"
#include "view-maximize.h"
#include "stage-util.h"
#include "tab-group-anim.h"

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


void stage_set_views_enabled(struct custom_stage *stage, bool enabled)
{
	struct custom_window *cw;
	wl_list_for_each(cw, &stage->windows, link)
	{
		struct wlr_scene_node *node = stage_window_node(cw);
		if (node)
			wlr_scene_node_set_enabled(node, enabled);
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
		server->ws_stage_mgrs[i].sidebar_hidden = false;
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
	if (view && view->anim_overlay)
	{
		wlr_scene_node_destroy(&view->anim_overlay->node);
		view->anim_overlay = NULL;
	}
	if (view && view->scene_tree)
	{
		wlr_scene_node_set_enabled(&view->scene_tree->node, true);
		view_focus(server, view);
	}
}

void stage_manager_new_window(struct hsdwl_server *server,
		struct hsdwl_view *view, bool animate)
{
	size_t ws = server->current_workspace;
	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];


	if (mgr->active_stage)
	{
		/* Keep every stage managed; the sidebar scales its thumbnails. */
		wl_list_insert(&mgr->inactive_stages,
			&mgr->active_stage->link);
		stage_set_views_enabled(mgr->active_stage, false);
		mgr->active_stage = NULL;
	}


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
	if (cw->h < 1) cw->h = 1280;
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


	if (!stage_thumb_init(stage, server, ws))
		wlr_log(WLR_ERROR, "thumb_tree create failed");


	if (view->scene_tree)
	{
		wlr_scene_node_reparent(&view->scene_tree->node,
			stage->tree);
		wlr_scene_node_set_position(&view->scene_tree->node,
			cw->x, cw->y);
		view_borders_update(view);
	}

	if (view->xwayland_surface)
	{
		int abs_x = SIDEBAR_WIDTH + (int)cw->x;
		int abs_y = (int)cw->y;
		int cfg_w = (int)cw->w;
		int cfg_h = (int)cw->h;
		if (cfg_w < 1) cfg_w = view->xwayland_surface->width;
		if (cfg_h < 1) cfg_h = view->xwayland_surface->height;
		wlr_xwayland_surface_configure(
			view->xwayland_surface, abs_x, abs_y, cfg_w, cfg_h);
	}

	mgr->active_stage = stage;


	if (animate)
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
				if (!wl_list_empty(&server->outputs)) {
					struct hsdwl_output *o = wl_container_of(
						server->outputs.next, o, link);
					if (o->wlr_output)
						wlr_output_schedule_frame(o->wlr_output);
				}
				wlr_scene_node_set_enabled(
					&view->scene_tree->node, false);
			}
		}
		else
		{
			wlr_scene_node_set_enabled(&view->scene_tree->node, true);
			int start_x = (scene_w - anim_w) / 2;
			int start_y = (scene_h - anim_h) / 2;
			wlr_scene_node_set_position(&view->scene_tree->node, start_x, start_y);
			int end_x = cw->x;
			int end_y = cw->y;
			animation_create_node_pos(server, &view->scene_tree->node,
				200, HSDWL_EASE_BEZIER,
				start_x, start_y, end_x, end_y,
				NULL, NULL);
			if (!wl_list_empty(&server->outputs)) {
				struct hsdwl_output *o = wl_container_of(
					server->outputs.next, o, link);
				if (o->wlr_output)
					wlr_output_schedule_frame(o->wlr_output);
			}
		}
	}

	view_focus(server, view);
	stage_manager_render_sidebar(server, ws);


	{
		struct custom_stage *s;

		if (mgr->active_stage)
		{
			struct custom_window *cw;
			wl_list_for_each(cw, &mgr->active_stage->windows,
					link)
			{
				if (!cw->view) continue;
				if (cw->view->tab_group
						&& cw->view->tab_group
							->maximized)
					tab_group_demaximize_to_zoomed(
						cw->view->tab_group, server);
				else if (cw->view->maximized)
					view_demaximize_to_zoomed(
						cw->view, server);
			}
		}

		wl_list_for_each(s, &mgr->inactive_stages, link)
		{
			struct custom_window *cw;
			wl_list_for_each(cw, &s->windows, link)
			{
				if (!cw->view) continue;
				if (cw->view->tab_group
						&& cw->view->tab_group
							->maximized)
					tab_group_demaximize_to_zoomed(
						cw->view->tab_group, server);
				else if (cw->view->maximized)
					view_demaximize_to_zoomed(
						cw->view, server);
			}
		}
	}
}

bool stage_manager_remove_view(struct hsdwl_server *server,
		struct hsdwl_view *view, size_t ws)
{
	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
	if (!mgr->active_stage) return false;

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
		if (!cw) return false;

		wl_list_remove(&cw->link);
		if (view->scene_tree)
		{
			double ox = view->scene_tree->node.x;
			double oy = view->scene_tree->node.y;
			wlr_scene_node_reparent(
				&view->scene_tree->node,
				server->workspaces[ws]);
			wlr_scene_node_set_position(
				&view->scene_tree->node,
				ox + SIDEBAR_WIDTH, oy);
		}
		free(cw);

		if (wl_list_empty(&st->windows))
		{
			wl_list_remove(&st->link);
			stage_reparent_to_canvas(st, server);
			stage_free(st);
		}
		return true;
	}

	wl_list_remove(&cw->link);
	if (view->scene_tree)
	{
		double ox = view->scene_tree->node.x;
		double oy = view->scene_tree->node.y;
		wlr_scene_node_reparent(
			&view->scene_tree->node,
			server->workspaces[ws]);
		wlr_scene_node_set_position(
			&view->scene_tree->node,
			ox + SIDEBAR_WIDTH, oy);
	}
	free(cw);

	if (wl_list_empty(&mgr->active_stage->windows))
	{
		if (!wl_list_empty(&mgr->inactive_stages))
		{
			struct custom_stage *promote = wl_container_of(
				mgr->inactive_stages.next, promote, link);
			wl_list_remove(&promote->link);
			stage_free(mgr->active_stage);
			mgr->active_stage = promote;
			stage_reparent_to_canvas(promote, server);
			stage_focus_first(promote, server);
		}
		else
		{
			stage_free(mgr->active_stage);
			mgr->active_stage = NULL;
		}
	}
	return true;
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

			if (!wl_list_empty(&mgr->inactive_stages))
			{
				struct custom_stage *promote = wl_container_of(
					mgr->inactive_stages.next, promote, link);
				wl_list_remove(&promote->link);
				stage_free(mgr->active_stage);
				mgr->active_stage = promote;
				stage_reparent_to_canvas(promote, server);
				stage_focus_first(promote, server);
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
	if (view->tab_group && view->tab_group->scene_tree)
	{
		cw->x = view->tab_group->scene_tree->node.x;
		cw->y = view->tab_group->scene_tree->node.y;
		cw->w = view->tab_group->content_area_box.width;
		cw->h = view->tab_group->content_area_box.height;
	}
	else
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
				if (st->thumb_w < 1 || st->thumb_h < 1)
				{
					stage_manager_render_sidebar(server, ws);
					return;
				}

				int sidebar_h = output_get_height(server);
				if (sidebar_h < 1)
					sidebar_h = 1;
				float sidebar_half = (float)sidebar_h / 2.0f;
				float td = ((float)st->thumb_y - sidebar_half)
					/ sidebar_half;
				if (td < -1.0f) td = -1.0f;
				if (td > 1.0f) td = 1.0f;
				stage_render_thumbnail(server, st,
					st->thumb_w, st->thumb_h, td);
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


		struct custom_stage *active = calloc(1, sizeof(*active));
		if (!active) continue;
		wl_list_init(&active->windows);

		active->tree = wlr_scene_tree_create(
			server->ws_stage_canvases[ws]);
		if (!active->tree) { stage_free(active); continue; }
		active->tree->node.data = active;

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
			if (first_view->xwayland_surface)
			{
				wlr_xwayland_surface_configure(
					first_view->xwayland_surface,
					SIDEBAR_WIDTH + (int)acw->x, (int)acw->y,
					(int)acw->w > 0 ? (int)acw->w
						: first_view->xwayland_surface->width,
					(int)acw->h > 0 ? (int)acw->h
						: first_view->xwayland_surface->height);
			}
			wl_list_insert(&active->windows, &acw->link);
		}

		stage_thumb_init(active, server, ws);

		mgr->active_stage = active;


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

			stage_thumb_init(st, server, ws);

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
			if (view->xwayland_surface)
			{
				wlr_xwayland_surface_configure(
					view->xwayland_surface,
					SIDEBAR_WIDTH + (int)ocw->x, (int)ocw->y,
					(int)ocw->w > 0 ? (int)ocw->w
						: view->xwayland_surface->width,
					(int)ocw->h > 0 ? (int)ocw->h
						: view->xwayland_surface->height);
			}
			wl_list_insert(&st->windows, &ocw->link);

			wlr_scene_node_set_enabled(
				&view->scene_tree->node, false);

			wl_list_insert(&mgr->inactive_stages, &st->link);
		}

		stage_manager_render_sidebar(server, ws);
	}
}

void stage_manager_check_sidebar_overlap(struct hsdwl_server *server,
		size_t ws)
{
	if (!server->config.stage_manager_enabled)
		return;

	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
	if (!mgr->active_stage)
		return;

	int output_h = 1080;
	if (!wl_list_empty(&server->outputs)) {
		struct hsdwl_output *o = wl_container_of(
			server->outputs.next, o, link);
		if (o->wlr_output)
			output_h = o->wlr_output->height;
	}

	struct wlr_box sidebar = {
		.x = 0, .y = 0,
		.width = SIDEBAR_WIDTH,
		.height = output_h,
	};

	bool overlap = false;
	struct custom_window *cw;
	wl_list_for_each(cw, &mgr->active_stage->windows, link)
	{
		if (!cw->view || !cw->view->scene_tree)
			continue;

		double abs_x, abs_y, abs_w, abs_h;

		if (cw->view->tab_group
				&& cw->view->tab_group->scene_tree)
		{
			struct hsdwl_tab_group *g = cw->view->tab_group;
			abs_x = SIDEBAR_WIDTH + g->scene_tree->node.x;
			abs_y = g->scene_tree->node.y;
			abs_w = g->content_area_box.width;
			abs_h = g->content_area_box.height
				+ g->tab_bar_thickness;
		}
		else
		{
			int bw = server->config.border_width;
			int tb = server->config.titlebar_height;
			if (tb < 0) tb = 0;
			abs_x = SIDEBAR_WIDTH + cw->view->scene_tree->node.x;
			abs_y = cw->view->scene_tree->node.y;
			abs_w = cw->w + 2 * bw;
			abs_h = cw->h + (tb > 0 ? tb + bw : bw);
		}

		struct wlr_box win = {
			.x = (int)abs_x, .y = (int)abs_y,
			.width = (int)abs_w, .height = (int)abs_h,
		};

		int ix = win.x > sidebar.x ? win.x : sidebar.x;
		int iy = win.y > sidebar.y ? win.y : sidebar.y;
		int ix2 = (win.x + win.width) < (sidebar.x + sidebar.width)
			? (win.x + win.width)
			: (sidebar.x + sidebar.width);
		int iy2 = (win.y + win.height) < (sidebar.y + sidebar.height)
			? (win.y + win.height)
			: (sidebar.y + sidebar.height);
		if (ix < ix2 && iy < iy2) {
			overlap = true;
			break;
		}
	}

	bool should_hide = overlap;
	if (should_hide == mgr->sidebar_hidden)
		return;


	struct hsdwl_animation *anim, *tmp;
	wl_list_for_each_safe(anim, tmp, &server->animations, link) {
		if (anim->pos_node
				== &server->ws_sidebar_trees[ws]->node)
		{
			wl_list_remove(&anim->link);
			free(anim);
		}
	}

	double cur = server->ws_sidebar_trees[ws]->node.x;
	double to = should_hide ? -SIDEBAR_WIDTH : 0;

	animation_create_node_pos(server,
		&server->ws_sidebar_trees[ws]->node,
		200, HSDWL_EASE_BEZIER,
		cur, 0, to, 0,
		NULL, NULL);

	if (!wl_list_empty(&server->outputs)) {
		struct hsdwl_output *o = wl_container_of(
			server->outputs.next, o, link);
		if (o->wlr_output)
			wlr_output_schedule_frame(o->wlr_output);
	}

	mgr->sidebar_hidden = should_hide;
	layer_shell_rearrange(server);
}

int stage_manager_window_count(struct hsdwl_server *server, size_t ws)
{
	if (ws >= HSDWL_NUM_WORKSPACES)
		return 0;
	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
	int count = 0;
	struct custom_window *cw;
	if (mgr->active_stage)
		wl_list_for_each(cw, &mgr->active_stage->windows, link)
			count++;
	struct custom_stage *stage;
	wl_list_for_each(stage, &mgr->inactive_stages, link)
		wl_list_for_each(cw, &stage->windows, link)
			count++;
	return count;
}
