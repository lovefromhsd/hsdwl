#define _GNU_SOURCE

#include "tab-group.h"
#include "tab-group-layout.h"
#include "server.h"
#include "deco.h"

#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>

/* ── view tab-group helpers ── */

static void view_enter_tab_group(struct hsdwl_view *view,
		struct hsdwl_tab_group *group)
{
	view->tab_group = group;
	wl_list_insert(&group->views, &view->tab_group_link);

	if (!view->scene_tree)
		return;

	wlr_scene_node_reparent(&view->scene_tree->node,
		group->content_area);

	wlr_scene_node_set_position(&view->scene_tree->node, 0, 0);
	if (view->content_tree)
		wlr_scene_node_set_position(&view->content_tree->node, 0, 0);

	for (int i = 0; i < 4; i++)
	{
		if (view->border_rects[i])
			wlr_scene_node_set_enabled(
				&view->border_rects[i]->node, false);
	}
	if (view->title_text_buf)
		wlr_scene_node_set_enabled(
			&view->title_text_buf->node, false);
}

static void view_leave_tab_group(struct hsdwl_view *view)
{
	struct hsdwl_server *server = view->server;
	struct hsdwl_tab_group *g = view->tab_group;
	if (!g)
		return;

	view->tab_group = NULL;
	wl_list_remove(&view->tab_group_link);
	wl_list_init(&view->tab_group_link);

	if (!view->scene_tree)
		return;

	/* reparent back to the group's parent tree (stage or workspace) */
	struct wlr_scene_tree *target = g->scene_tree && g->scene_tree->node.parent
		? g->scene_tree->node.parent
		: server->workspaces[server->current_workspace];

	/* compute absolute scene offset of the target tree */
	int off_x = 0, off_y = 0;
	struct wlr_scene_tree *pn = target;
	while (pn)
	{
		off_x += pn->node.x;
		off_y += pn->node.y;
		pn = pn->node.parent;
	}

	int bw = server->config.border_width;
	int tb = server->config.titlebar_height;
	if (tb < 0) tb = 0;
	int pos_x = (int)server->cursor->x - off_x;
	int pos_y = (int)server->cursor->y - off_y;

	wlr_scene_node_reparent(&view->scene_tree->node, target);
	wlr_scene_node_set_position(&view->scene_tree->node, pos_x, pos_y);
	wlr_scene_node_set_enabled(&view->scene_tree->node, true);
	if (view->content_tree)
		wlr_scene_node_set_position(&view->content_tree->node,
			bw, tb > 0 ? tb : bw);

	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	if (view->border_rects[0])
		for (int i = 0; i < 4; i++)
			wlr_scene_node_set_enabled(
				&view->border_rects[i]->node, true);
	if (view->title_text_buf)
		wlr_scene_node_set_enabled(
			&view->title_text_buf->node, true);
	view_borders_update(view);
	titlebar_text_update(view);
}

/* ── public API ── */

void hsdwl_tab_group_init(struct hsdwl_server *server)
{
	wl_list_init(&server->tab_groups);
}

void hsdwl_tab_group_finish(struct hsdwl_server *server)
{
	struct hsdwl_tab_group *group, *tmp;
	wl_list_for_each_safe(group, tmp, &server->tab_groups, link)
		hsdwl_tab_group_destroy(group);
}

struct hsdwl_tab_group *hsdwl_tab_group_create(struct hsdwl_server *server,
		struct hsdwl_view *a, struct hsdwl_view *b,
		enum hsdwl_tab_orientation orientation)
{
	struct hsdwl_tab_group *group = calloc(1, sizeof(*group));
	if (!group)
		return NULL;

	group->server = server;
	group->orientation = orientation;
	group->tab_bar_thickness = 28;
	wl_list_init(&group->views);
	wl_list_init(&group->tab_buttons);

	/* Use view b (the target) for initial placement */
	struct wlr_scene_tree *parent = b->scene_tree
		? b->scene_tree->node.parent
		: server->workspaces[server->current_workspace];
	if (!parent)
		parent = &server->scene->tree;

	int bx = b->scene_tree ? b->scene_tree->node.x : 0;
	int by = b->scene_tree ? b->scene_tree->node.y : 0;

	int content_w = 800, content_h = 600;
	if (b->xdg_surface && b->xdg_surface->configured)
	{
		content_w = b->xdg_surface->geometry.width;
		content_h = b->xdg_surface->geometry.height;
	}
	else if (b->xwayland_surface)
	{
		content_w = b->xwayland_surface->width;
		content_h = b->xwayland_surface->height;
	}

	group->scene_tree = wlr_scene_tree_create(parent);
	if (!group->scene_tree)
	{
		free(group);
		return NULL;
	}
	wlr_scene_node_set_position(&group->scene_tree->node, bx, by);
	wlr_scene_node_raise_to_top(&group->scene_tree->node);

	int cont_w = content_w;
	int cont_h = content_h;
	if (cont_h < 1) cont_h = 1;

	group->content_area_box = (struct wlr_box){
		.x = 0,
		.y = group->tab_bar_thickness,
		.width = cont_w,
		.height = cont_h,
	};

	group->tab_bar_bg = wlr_scene_buffer_create(
		group->scene_tree, NULL);

	group->content_area = wlr_scene_tree_create(group->scene_tree);
	if (!group->content_area)
	{
		hsdwl_tab_group_destroy(group);
		return NULL;
	}

	if (group->orientation == HSDWL_TAB_HORIZONTAL)
		wlr_scene_node_set_position(&group->content_area->node,
			0, group->tab_bar_thickness);
	else
		wlr_scene_node_set_position(&group->content_area->node,
			group->tab_bar_thickness, 0);

	view_enter_tab_group(a, group);
	view_enter_tab_group(b, group);

	struct hsdwl_view *views[2] = {a, b};
	for (int i = 0; i < 2; i++)
	{
		view_configure_size(views[i], cont_w, cont_h);

		struct hsdwl_tab_button *btn = tab_button_create(group, views[i]);
		if (btn)
			wl_list_insert(&group->tab_buttons, &btn->link);
	}

	wl_list_insert(&server->tab_groups, &group->link);

	/* Disable all views, then enable only the active one */
	struct hsdwl_view *vi;
	wl_list_for_each(vi, &group->views, tab_group_link)
	{
		if (vi->scene_tree)
			wlr_scene_node_set_enabled(
				&vi->scene_tree->node, false);
	}
	group->active = a;
	if (a->scene_tree)
		wlr_scene_node_set_enabled(&a->scene_tree->node, true);

	view_focus(group->server, a);
	hsdwl_tab_group_update_layout(group);

	return group;
}

void hsdwl_tab_group_add_view(struct hsdwl_tab_group *group,
		struct hsdwl_view *view)
{
	if (!group || !view || view->tab_group)
		return;

	view_enter_tab_group(view, group);
	view_configure_size(view,
		group->content_area_box.width,
		group->content_area_box.height);

	struct hsdwl_tab_button *btn = tab_button_create(group, view);
	if (btn)
		wl_list_insert(&group->tab_buttons, &btn->link);

	hsdwl_tab_group_set_active(group, view);
}

void hsdwl_tab_group_remove_view(struct hsdwl_tab_group *group,
		struct hsdwl_view *view)
{
	if (!group || !view || view->tab_group != group)
		return;

	struct hsdwl_tab_button *btn, *tmp;
	wl_list_for_each_safe(btn, tmp, &group->tab_buttons, link)
	{
		if (btn->view == view)
		{
			tab_button_destroy(btn);
			break;
		}
	}

	view_leave_tab_group(view);

	if (view->scene_tree)
		wlr_scene_node_raise_to_top(&view->scene_tree->node);

	if (wl_list_length(&group->views) <= 1)
	{
		struct hsdwl_view *remaining = NULL;
		int gx = group->scene_tree ? group->scene_tree->node.x : 0;
		int gy = group->scene_tree ? group->scene_tree->node.y : 0;
		if (wl_list_length(&group->views) == 1)
		{
			remaining = wl_container_of(
				group->views.next, remaining, tab_group_link);
		}
		if (remaining)
		{
			view_leave_tab_group(remaining);
			if (remaining->scene_tree)
				wlr_scene_node_set_position(
					&remaining->scene_tree->node,
					gx, gy);
		}
		hsdwl_tab_group_destroy(group);
		return;
	}

	if (group->active == view)
	{
		struct hsdwl_view *next = NULL;
		wl_list_for_each(btn, &group->tab_buttons, link)
		{
			next = btn->view;
			break;
		}
		if (next)
		{
			group->active = next;
			if (next->scene_tree)
				wlr_scene_node_set_enabled(
					&next->scene_tree->node, true);
		}
	}
	hsdwl_tab_group_update_layout(group);

	if (view->scene_tree)
		wlr_scene_node_raise_to_top(&view->scene_tree->node);
}

void hsdwl_tab_group_set_active(struct hsdwl_tab_group *group,
		struct hsdwl_view *view)
{
	if (!group || !view || view->tab_group != group)
		return;

	if (group->active != view)
	{
		struct hsdwl_view *prev = group->active;
		if (prev && prev->scene_tree)
			wlr_scene_node_set_enabled(
				&prev->scene_tree->node, false);

		group->active = view;
		if (view->scene_tree)
			wlr_scene_node_set_enabled(
				&view->scene_tree->node, true);
	}

	view_focus(group->server, view);
	hsdwl_tab_group_update_layout(group);
}

void hsdwl_tab_group_destroy(struct hsdwl_tab_group *group)
{
	if (!group)
		return;

	/* cancel any pending animation for this group */
	struct hsdwl_animation *anim, *tmp;
	wl_list_for_each_safe(anim, tmp, &group->server->animations, link)
	{
		if (anim->user_data == group)
		{
			wl_list_remove(&anim->link);
			free(anim);
		}
	}

	struct hsdwl_tab_button *btn, *tmp2;
	wl_list_for_each_safe(btn, tmp2, &group->tab_buttons, link)
		tab_button_destroy(btn);

	if (group->scene_tree)
	{
		group->scene_tree->node.data = NULL;
		wlr_scene_node_destroy(&group->scene_tree->node);
	}

	wl_list_remove(&group->link);
	free(group);
}

bool hsdwl_tab_group_is_member(struct hsdwl_view *view)
{
	return view && view->tab_group != NULL;
}

struct hsdwl_view *hsdwl_tab_group_view_at(struct hsdwl_server *server,
		double lx, double ly)
{
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, &sx, &sy);
	if (!node)
		return NULL;

	if (node->data)
	{
		struct hsdwl_view *v = node->data;
		if (v->tab_group)
			return v;
	}
	return NULL;
}

struct hsdwl_tab_group *hsdwl_tab_group_at(struct hsdwl_server *server,
		double lx, double ly)
{
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, &sx, &sy);
	if (!node)
		return NULL;

	struct wlr_scene_tree *tree = node->parent;
	while (tree && tree != &server->scene->tree)
	{
		if (tree->node.data)
		{
			struct hsdwl_tab_group *group = tree->node.data;
			if (group && group->server)
				return group;
		}
		if (!tree->node.parent)
			break;
		tree = tree->node.parent;
	}
	return NULL;
}

struct hsdwl_view *hsdwl_tab_group_next(struct hsdwl_tab_group *group,
		struct hsdwl_view *current, bool reverse)
{
	if (!group || wl_list_empty(&group->tab_buttons))
		return NULL;

	struct hsdwl_tab_button *btn;
	bool found = false;
	struct hsdwl_tab_button *first = NULL;

	if (reverse)
	{
		wl_list_for_each_reverse(btn, &group->tab_buttons, link)
		{
			if (!first) first = btn;
			if (found) return btn->view;
			if (btn->view == current) found = true;
		}
	}
	else
	{
		wl_list_for_each(btn, &group->tab_buttons, link)
		{
			if (!first) first = btn;
			if (found) return btn->view;
			if (btn->view == current) found = true;
		}
	}

	return first ? first->view : NULL;
}
