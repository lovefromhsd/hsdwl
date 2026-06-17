#define _GNU_SOURCE

#include "tab-group-anim.h"
#include "tab-group-layout.h"
#include "server.h"
#include "view.h"

#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

#include "animation.h"

struct tg_anim_state {
	struct hsdwl_tab_group *group;
	int cw, ch;
};

static void tg_anim_zoom_finish(struct hsdwl_server *server, void *user_data)
{
	(void)server;
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

static void tg_anim_full_finish(struct hsdwl_server *server, void *user_data)
{
	(void)server;
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
	(void)server;
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

	group->content_area_box.width = zw;
	group->content_area_box.height = zh;

	struct hsdwl_view *vi;
	wl_list_for_each(vi, &group->views, tab_group_link)
		view_configure_size(vi, zw, zh);

	hsdwl_tab_group_update_layout(group);

	double cur_x = group->saved_geometry.x;
	double cur_y = group->saved_geometry.y;
	double tgt_x = pad;
	double tgt_y = 0;

	struct tg_anim_state *st = calloc(1, sizeof(*st));
	st->group = group;
	st->cw = zw;
	st->ch = zh;

	animation_create_node_pos(server, &group->scene_tree->node,
		200, HSDWL_EASE_BEZIER,
		cur_x, cur_y, tgt_x, tgt_y,
		tg_anim_zoom_finish, st);

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

		animation_create_node_pos(server, &group->scene_tree->node,
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

	group->content_area_box.width = tgt_w;
	group->content_area_box.height = tgt_h;

	struct hsdwl_view *vi;
	wl_list_for_each(vi, &group->views, tab_group_link)
		view_configure_size(vi, tgt_w, tgt_h);

	hsdwl_tab_group_update_layout(group);

	double cur_x = group->scene_tree->node.x;
	double cur_y = group->scene_tree->node.y;
	double tgt_x = group->saved_geometry.x;
	double tgt_y = group->saved_geometry.y;

	struct tg_anim_state *st = calloc(1, sizeof(*st));
	st->group = group;
	st->cw = tgt_w;
	st->ch = tgt_h;

	animation_create_node_pos(group->server, &group->scene_tree->node,
		200, HSDWL_EASE_BEZIER,
		cur_x, cur_y, tgt_x, tgt_y,
		tg_anim_restore_finish, st);

	struct wlr_output *wlr_o = wlr_output_layout_output_at(
		group->server->output_layout,
		group->saved_geometry.x + group->saved_geometry.width / 2,
		group->saved_geometry.y + group->saved_geometry.height / 2);
	if (wlr_o)
		wlr_output_schedule_frame(wlr_o);

	group->maximized = false;
	group->zoomed = false;
}
