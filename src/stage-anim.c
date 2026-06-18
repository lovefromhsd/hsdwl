#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "stage.h"
#include "stage-3d.h"
#include "stage-sidebar.h"
#include "server.h"
#include "output.h"
#include "view.h"
#include "view-maximize.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include "animation.h"

#define MAX_STAGE_WINDOWS 64

static inline float pos_tilt_angle(float win_cx, float screen_cx)
{
	float a = -atan2f(win_cx - screen_cx, 800.0f) * (180.0f / (float)M_PI);
	if (a > 30.0f) a = 30.0f;
	if (a < -30.0f) a = -30.0f;
	return a;
}

static inline void get_thumb_size(int src_w, int src_h, int max_sz,
		int *out_w, int *out_h)
{
	float a = (float)src_w / (float)src_h;
	if (a >= 1.0f) {
		*out_w = max_sz;
		*out_h = (int)(max_sz / a + 0.5f);
	} else {
		*out_w = (int)(max_sz * a + 0.5f);
		*out_h = max_sz;
	}
	if (*out_w < 1) *out_w = 1;
	if (*out_h < 1) *out_h = 1;
}



struct stage_switch_anim {
	struct hsdwl_server *server;
	struct custom_stage *old_stage;
	struct custom_stage *new_stage;
	size_t ws;
	int remaining;
	bool insert_tail;
	bool use_3d;
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



static void stage_switch_on_anim_done(struct hsdwl_server *server,
		void *user_data)
{
	struct stage_switch_anim *ssa = user_data;
	ssa->remaining--;
	if (ssa->remaining > 0) return;

	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ssa->ws];

	if (!ssa->use_3d) {
		for (int i = 0; i < ssa->n_overlays; i++)
			wlr_scene_node_destroy(&ssa->overlays[i]->node);
	}

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



void stage_manager_switch(struct hsdwl_server *server,
		struct custom_stage *target, size_t ws)
{
	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
	if (!target || target == mgr->active_stage)
		return;

	struct custom_stage *old = mgr->active_stage;
	int bw = server->config.border_width;
	int tb = server->config.titlebar_height;

	int scene_w = 1920;
	if (!wl_list_empty(&server->outputs)) {
		struct hsdwl_output *o = wl_container_of(
			server->outputs.next, o, link);
		if (o->wlr_output)
			scene_w = o->wlr_output->width;
	}
	float scene_cx = (float)scene_w / 2.0f;

	stage_3d_cancel(server);
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
	ssa->use_3d = false;

	if (ssa->use_3d) {
		stage_3d_cancel(server);
		if (old) {
			struct custom_window *cw;
			wl_list_for_each(cw, &old->windows, link)
			{
				if (ssa->n_overlays >= MAX_STAGE_WINDOWS) break;

				struct wlr_buffer *buf = view_capture_full_window(
					server, cw->view, (int)cw->w, (int)cw->h,
					bw, tb);
				if (!buf) continue;

				struct wlr_texture *tex = wlr_texture_from_buffer(
					server->renderer, buf);
				wlr_buffer_drop(buf);
				if (!tex) continue;

				int fw = (int)cw->w + 2 * bw;
				int fh = (int)cw->h + tb + bw;
				int fx = SIDEBAR_WIDTH + (int)cw->x;
				int fy = (int)cw->y;

				stage_3d_start_flip(server,
					tex, fw, fh, fx, fy,
					NULL, 0, 0, fx, fy,
					400, 0.0f, 0.0f, 800.0f,
					stage_switch_on_anim_done, ssa);
				ssa->n_overlays++;
				ssa->remaining++;
			}
		}

		{
			struct custom_window *cw;
			wl_list_for_each(cw, &target->windows, link)
			{
				if (ssa->n_overlays >= MAX_STAGE_WINDOWS) break;

				struct wlr_buffer *buf = view_capture_full_window(
					server, cw->view, (int)cw->w, (int)cw->h,
					bw, tb);
				if (!buf) continue;

				struct wlr_texture *tex = wlr_texture_from_buffer(
					server->renderer, buf);
				wlr_buffer_drop(buf);
				if (!tex) continue;

				int tx = (int)cw->w + 2 * bw;
				int ty = (int)cw->h + tb + bw;
				int ttx = SIDEBAR_WIDTH + (int)cw->x;
				int tty = (int)cw->y;

				stage_3d_start_flip(server,
					NULL, 0, 0, ttx, tty,
					tex, tx, ty, ttx, tty,
					400, 0.0f, 0.0f, 800.0f,
					stage_switch_on_anim_done, ssa);
				ssa->n_overlays++;
				ssa->remaining++;
			}
		}

		if (old) stage_set_views_enabled(old, false);
		stage_set_views_enabled(target, false);
		goto after_captures;
	}

	
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

				get_thumb_size(fw, fh, SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD, &tw, &th);

				struct wlr_scene_buffer *ov =
					wlr_scene_buffer_create(
						server->animation_tree, buf);
				
					struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buf);
					wlr_buffer_drop(buf);
				wlr_scene_node_set_position(&ov->node,fx,fy);
				wlr_scene_buffer_set_dest_size(ov,fw,fh);
				wlr_scene_node_raise_to_top(&ov->node);
				ssa->overlays[ssa->n_overlays++] = ov;
				ssa->remaining++;
				animation_create(server, ov,400,HSDWL_EASE_BEZIER,
					fx,fy,fw,fh,
					target->thumb_x,target->thumb_y,tw,th,
					stage_switch_on_anim_done,ssa);
    float out_angle = pos_tilt_angle(fx + fw / 2.0f, scene_cx);
    if (tex) {
        if (stage_3d_start_tilt_anim(server, tex, fw, fh, ov, 400, 0.0f, out_angle, 0.0f, old->z_offset, 800.0f, stage_switch_on_anim_done, ssa))
            ssa->remaining++;
    }

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

			get_thumb_size(fw, fh, SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD, &tw, &th);

			struct wlr_scene_buffer *ov = wlr_scene_buffer_create(
				server->animation_tree, buf);
			
				struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buf);
				wlr_buffer_drop(buf);
			wlr_scene_node_set_position(&ov->node, fx, fy);
			wlr_scene_buffer_set_dest_size(ov, fw, fh);
			wlr_scene_node_raise_to_top(&ov->node);

			ssa->overlays[ssa->n_overlays++] = ov;
			ssa->remaining++;

			animation_create(server, ov, 400, HSDWL_EASE_BEZIER,
				fx, fy, fw, fh,
				target->thumb_x, target->thumb_y, tw, th,
				stage_switch_on_anim_done, ssa);
				float out_angle = pos_tilt_angle(fx + fw / 2.0f, scene_cx);
				if (tex) {
					if (stage_3d_start_tilt_anim(server, tex, fw, fh, ov, 400, 0.0f, out_angle, 0.0f, old->z_offset, 800.0f, stage_switch_on_anim_done, ssa))
						ssa->remaining++;
				}

		}
	}

	
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

				get_thumb_size((int)cw_, (int)ch_, SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD, &tw, &th);
				ttx = SIDEBAR_WIDTH + (int)g->scene_tree->node.x;
				tty = (int)g->scene_tree->node.y
					+ g->tab_bar_thickness;
				tx = cw_;  ty = ch_;

				struct wlr_scene_buffer *ov =
					wlr_scene_buffer_create(
						server->animation_tree, buf);
				
					struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buf);
					wlr_buffer_drop(buf);
				wlr_scene_node_set_position(&ov->node,
					target->thumb_x, target->thumb_y);
				wlr_scene_buffer_set_dest_size(ov, tw, th);
				wlr_scene_node_raise_to_top(&ov->node);
				ssa->overlays[ssa->n_overlays++] = ov;
				ssa->remaining++;
				animation_create(server, ov,400,HSDWL_EASE_BEZIER,
					target->thumb_x,target->thumb_y,tw,th,
					ttx,tty,tx,ty,
					stage_switch_on_anim_done,ssa);
    float in_angle = pos_tilt_angle(ttx + tx / 2.0f, scene_cx);
    if (tex) {
        if (stage_3d_start_tilt_anim(server, tex, tx, ty, ov, 400, in_angle, 0.0f, target->z_offset, 0.0f, 800.0f, stage_switch_on_anim_done, ssa))
            ssa->remaining++;
    }

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

			get_thumb_size(tx, ty, SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD, &tw, &th);

			struct wlr_scene_buffer *ov = wlr_scene_buffer_create(
				server->animation_tree, buf);
			
				struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buf);
				wlr_buffer_drop(buf);
			wlr_scene_node_set_position(&ov->node,
				target->thumb_x, target->thumb_y);
			wlr_scene_buffer_set_dest_size(ov, tw, th);
			wlr_scene_node_raise_to_top(&ov->node);

			ssa->overlays[ssa->n_overlays++] = ov;
			ssa->remaining++;

			animation_create(server, ov, 400, HSDWL_EASE_BEZIER,
				target->thumb_x, target->thumb_y, tw, th,
				ttx, tty, tx, ty,
				stage_switch_on_anim_done, ssa);
				float in_angle = pos_tilt_angle(ttx + tx / 2.0f, scene_cx);
				if (tex) {
					if (stage_3d_start_tilt_anim(server, tex, tx, ty, ov, 400, in_angle, 0.0f, target->z_offset, 0.0f, 800.0f, stage_switch_on_anim_done, ssa))
						ssa->remaining++;
				}

		}
	}

	
	if (old) stage_set_views_enabled(old, false);
	stage_set_views_enabled(target, false);

after_captures:
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

	struct custom_stage *old = mgr->active_stage;
	struct custom_stage *target = NULL;

	if (reverse)
		target = wl_container_of(
			mgr->inactive_stages.prev, target, link);
	else
		target = wl_container_of(
			mgr->inactive_stages.next, target, link);

	if (!target)
		return;

	bool insert_tail = !reverse;

	int bw = server->config.border_width;
	int tb = server->config.titlebar_height;

	int scene_w = 1920;
	if (!wl_list_empty(&server->outputs)) {
		struct hsdwl_output *o = wl_container_of(
			server->outputs.next, o, link);
		if (o->wlr_output)
			scene_w = o->wlr_output->width;
	}
	float scene_cx = (float)scene_w / 2.0f;

	stage_3d_cancel(server);
	struct stage_switch_anim *ssa = calloc(1, sizeof(*ssa));
	if (!ssa)
		goto instant_switch;
	ssa->server = server;
	ssa->old_stage = old;
	ssa->new_stage = target;
	ssa->ws = ws;
	ssa->remaining = 0;
	ssa->insert_tail = insert_tail;
	ssa->n_overlays = 0;
	ssa->use_3d = false;

	if (ssa->use_3d) {
		stage_3d_cancel(server);
		if (old) {
			struct custom_window *cw;
			wl_list_for_each(cw, &old->windows, link)
			{
				if (ssa->n_overlays >= MAX_STAGE_WINDOWS) break;

				struct wlr_buffer *buf = view_capture_full_window(
					server, cw->view, (int)cw->w, (int)cw->h,
					bw, tb);
				if (!buf) continue;

				struct wlr_texture *tex = wlr_texture_from_buffer(
					server->renderer, buf);
				wlr_buffer_drop(buf);
				if (!tex) continue;

				int fw = (int)cw->w + 2 * bw;
				int fh = (int)cw->h + tb + bw;
				int fx = SIDEBAR_WIDTH + (int)cw->x;
				int fy = (int)cw->y;

				stage_3d_start_flip(server,
					tex, fw, fh, fx, fy,
					NULL, 0, 0, fx, fy,
					400, 0.0f, 0.0f, 800.0f,
					stage_switch_on_anim_done, ssa);
				ssa->n_overlays++;
				ssa->remaining++;
			}
		}

		{
			struct custom_window *cw;
			wl_list_for_each(cw, &target->windows, link)
			{
				if (ssa->n_overlays >= MAX_STAGE_WINDOWS) break;

				struct wlr_buffer *buf = view_capture_full_window(
					server, cw->view, (int)cw->w, (int)cw->h,
					bw, tb);
				if (!buf) continue;

				struct wlr_texture *tex = wlr_texture_from_buffer(
					server->renderer, buf);
				wlr_buffer_drop(buf);
				if (!tex) continue;

				int tx = (int)cw->w + 2 * bw;
				int ty = (int)cw->h + tb + bw;
				int ttx = SIDEBAR_WIDTH + (int)cw->x;
				int tty = (int)cw->y;

				stage_3d_start_flip(server,
					NULL, 0, 0, ttx, tty,
					tex, tx, ty, ttx, tty,
					400, 0.0f, 0.0f, 800.0f,
					stage_switch_on_anim_done, ssa);
				ssa->n_overlays++;
				ssa->remaining++;
			}
		}

		if (old) stage_set_views_enabled(old, false);
		stage_set_views_enabled(target, false);
		goto after_captures_cycle;
	}

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

				get_thumb_size(fw, fh, SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD, &tw, &th);

				struct wlr_scene_buffer *ov =
					wlr_scene_buffer_create(
						server->animation_tree, buf);
				
					struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buf);
					wlr_buffer_drop(buf);
				wlr_scene_node_set_position(&ov->node,fx,fy);
				wlr_scene_buffer_set_dest_size(ov,fw,fh);
				wlr_scene_node_raise_to_top(&ov->node);
				ssa->overlays[ssa->n_overlays++] = ov;
				ssa->remaining++;
				animation_create(server, ov,400,HSDWL_EASE_BEZIER,
					fx,fy,fw,fh,
					target->thumb_x,target->thumb_y,tw,th,
					stage_switch_on_anim_done,ssa);
    float out_angle = pos_tilt_angle(fx + fw / 2.0f, scene_cx);
    if (tex) {
        if (stage_3d_start_tilt_anim(server, tex, fw, fh, ov, 400, 0.0f, out_angle, 0.0f, old->z_offset, 800.0f, stage_switch_on_anim_done, ssa))
            ssa->remaining++;
    }

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

			get_thumb_size(fw, fh, SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD, &tw, &th);

			struct wlr_scene_buffer *ov = wlr_scene_buffer_create(
				server->animation_tree, buf);
			
				struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buf);
				wlr_buffer_drop(buf);
			wlr_scene_node_set_position(&ov->node, fx, fy);
			wlr_scene_buffer_set_dest_size(ov, fw, fh);
			wlr_scene_node_raise_to_top(&ov->node);

			ssa->overlays[ssa->n_overlays++] = ov;
			ssa->remaining++;

			animation_create(server, ov, 400, HSDWL_EASE_BEZIER,
				fx, fy, fw, fh,
				target->thumb_x, target->thumb_y, tw, th,
				stage_switch_on_anim_done, ssa);
				float out_angle = pos_tilt_angle(fx + fw / 2.0f, scene_cx);
				if (tex) {
					if (stage_3d_start_tilt_anim(server, tex, fw, fh, ov, 400, 0.0f, out_angle, 0.0f, old->z_offset, 800.0f, stage_switch_on_anim_done, ssa))
						ssa->remaining++;
				}

		}
	}

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

				get_thumb_size((int)cw_, (int)ch_, SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD, &tw, &th);
				ttx = SIDEBAR_WIDTH + (int)g->scene_tree->node.x;
				tty = (int)g->scene_tree->node.y
					+ g->tab_bar_thickness;
				tx = cw_;  ty = ch_;

				struct wlr_scene_buffer *ov =
					wlr_scene_buffer_create(
						server->animation_tree, buf);
				
					struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buf);
					wlr_buffer_drop(buf);
				wlr_scene_node_set_position(&ov->node,
					target->thumb_x, target->thumb_y);
				wlr_scene_buffer_set_dest_size(ov, tw, th);
				wlr_scene_node_raise_to_top(&ov->node);
				ssa->overlays[ssa->n_overlays++] = ov;
				ssa->remaining++;
				animation_create(server, ov,400,HSDWL_EASE_BEZIER,
					target->thumb_x,target->thumb_y,tw,th,
					ttx,tty,tx,ty,
					stage_switch_on_anim_done,ssa);
    float in_angle = pos_tilt_angle(ttx + tx / 2.0f, scene_cx);
    if (tex) {
        if (stage_3d_start_tilt_anim(server, tex, tx, ty, ov, 400, in_angle, 0.0f, target->z_offset, 0.0f, 800.0f, stage_switch_on_anim_done, ssa))
            ssa->remaining++;
    }

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

			get_thumb_size(tx, ty, SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD, &tw, &th);

			struct wlr_scene_buffer *ov = wlr_scene_buffer_create(
				server->animation_tree, buf);
			
				struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buf);
				wlr_buffer_drop(buf);
			wlr_scene_node_set_position(&ov->node,
				target->thumb_x, target->thumb_y);
			wlr_scene_buffer_set_dest_size(ov, tw, th);
			wlr_scene_node_raise_to_top(&ov->node);

			ssa->overlays[ssa->n_overlays++] = ov;
			ssa->remaining++;

			animation_create(server, ov, 400, HSDWL_EASE_BEZIER,
				target->thumb_x, target->thumb_y, tw, th,
				ttx, tty, tx, ty,
				stage_switch_on_anim_done, ssa);
				float in_angle = pos_tilt_angle(ttx + tx / 2.0f, scene_cx);
				if (tex) {
					if (stage_3d_start_tilt_anim(server, tex, tx, ty, ov, 400, in_angle, 0.0f, target->z_offset, 0.0f, 800.0f, stage_switch_on_anim_done, ssa))
						ssa->remaining++;
				}

		}
	}

	if (old) stage_set_views_enabled(old, false);
	stage_set_views_enabled(target, false);

after_captures_cycle:
	if (ssa->remaining > 0) return;

	free(ssa);

instant_switch:
	if (old) {
		stage_set_views_enabled(old, false);
		if (insert_tail)
			wl_list_insert(mgr->inactive_stages.prev,
				&old->link);
		else
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



void stage_manager_merge(struct hsdwl_server *server,
		struct custom_stage *source, size_t ws)
{
	struct workspace_stage_mgr *mgr = &server->ws_stage_mgrs[ws];
	if (!source || !mgr->active_stage || source == mgr->active_stage)
		return;

	int bw = server->config.border_width;
	int tb = server->config.titlebar_height;

	int scene_w = 1920;
	if (!wl_list_empty(&server->outputs)) {
		struct hsdwl_output *o = wl_container_of(
			server->outputs.next, o, link);
		if (o->wlr_output)
			scene_w = o->wlr_output->width;
	}
	float scene_cx = (float)scene_w / 2.0f;

	stage_3d_cancel(server);
	struct stage_merge_anim *sma = calloc(1, sizeof(*sma));
	if (!sma)
		goto instant_merge;
	sma->server = server;
	sma->source = source;
	sma->ws = ws;
	sma->remaining = 0;
	sma->n_overlays = 0;

	
	{
		struct custom_window *cw;
		struct hsdwl_tab_group *seen_tg[64];
		int n_seen_tg = 0;
		wl_list_for_each(cw, &source->windows, link)
		{
			if (sma->n_overlays >= MAX_STAGE_WINDOWS) break;

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

				get_thumb_size((int)cw_, (int)ch_, SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD, &tw, &th);
				ttx = SIDEBAR_WIDTH + (int)g->scene_tree->node.x;
				tty = (int)g->scene_tree->node.y
					+ g->tab_bar_thickness;
				tx = cw_;  ty = ch_;

				struct wlr_scene_buffer *ov =
					wlr_scene_buffer_create(
						server->animation_tree, buf);
				
					struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buf);
					wlr_buffer_drop(buf);
				wlr_scene_node_set_position(&ov->node,
					source->thumb_x, source->thumb_y);
				wlr_scene_buffer_set_dest_size(ov, tw, th);
				wlr_scene_node_raise_to_top(&ov->node);
				sma->overlays[sma->n_overlays++] = ov;
				sma->remaining++;
				animation_create(server, ov,400,HSDWL_EASE_BEZIER,
					source->thumb_x,source->thumb_y,tw,th,
					ttx,tty,tx,ty,
					stage_merge_on_anim_done,sma);
    float in_angle = pos_tilt_angle(ttx + tx / 2.0f, scene_cx);
    if (tex) {
        if (stage_3d_start_tilt_anim(server, tex, tx, ty, ov, 400, in_angle, 0.0f, source->z_offset, 0.0f, 800.0f, stage_merge_on_anim_done, sma))
            sma->remaining++;
    }

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

			get_thumb_size(tx, ty, SIDEBAR_WIDTH - 2 * STAGE_THUMB_PAD, &tw, &th);

			struct wlr_scene_buffer *ov = wlr_scene_buffer_create(
				server->animation_tree, buf);
			
				struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buf);
				wlr_buffer_drop(buf);
			wlr_scene_node_set_position(&ov->node,
				source->thumb_x, source->thumb_y);
			wlr_scene_buffer_set_dest_size(ov, tw, th);
			wlr_scene_node_raise_to_top(&ov->node);

			sma->overlays[sma->n_overlays++] = ov;
			sma->remaining++;

			animation_create(server, ov, 400, HSDWL_EASE_BEZIER,
				source->thumb_x, source->thumb_y, tw, th,
				ttx, tty, tx, ty,
				stage_merge_on_anim_done, sma);
				float in_angle = pos_tilt_angle(ttx + tx / 2.0f, scene_cx);
				if (tex) {
					if (stage_3d_start_tilt_anim(server, tex, tx, ty, ov, 400, in_angle, 0.0f, source->z_offset, 0.0f, 800.0f, stage_merge_on_anim_done, sma))
						sma->remaining++;
				}

		}
	}

	stage_set_views_enabled(source, false);

	if (sma->remaining > 0) return;

	free(sma);

instant_merge:
	{
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
}
