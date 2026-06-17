#define _GNU_SOURCE

#include "tab-group.h"
#include "server.h"
#include "view.h"

#include <cairo.h>
#include <drm_fourcc.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#define TAB_BAR_THICKNESS 28
#define TAB_BUTTON_MIN_WIDTH 60
#define TAB_BUTTON_MAX_WIDTH 200
#define TAB_BUTTON_PADDING 8
#define PREVIEW_ALPHA 0.25f

/* ── custom wlr_buffer for cairo-rendered tab text ── */

struct tab_text_buffer
{
	struct wlr_buffer base;
	unsigned char *data;
	int width, height, stride;
	uint32_t format;
};

static void tab_text_buffer_destroy(struct wlr_buffer *wbuffer)
{
	struct tab_text_buffer *buf = wl_container_of(wbuffer, buf, base);
	free(buf->data);
	free(buf);
}

static bool tab_text_buffer_get_shm(struct wlr_buffer *wbuffer,
		struct wlr_shm_attributes *attribs)
{
	struct tab_text_buffer *buf = wl_container_of(wbuffer, buf, base);
	*attribs = (struct wlr_shm_attributes){
		.fd = -1,
		.format = buf->format,
		.width = buf->width,
		.height = buf->height,
		.stride = buf->stride,
		.offset = 0,
	};
	return true;
}

static bool tab_text_buffer_begin_data_ptr_access(
		struct wlr_buffer *wbuffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride)
{
	(void)flags;
	struct tab_text_buffer *buf = wl_container_of(wbuffer, buf, base);
	*data = buf->data;
	*format = buf->format;
	*stride = buf->stride;
	return true;
}

static void tab_text_buffer_end_data_ptr_access(struct wlr_buffer *wbuffer)
{
	(void)wbuffer;
}

static const struct wlr_buffer_impl tab_text_buffer_impl = {
	.destroy = tab_text_buffer_destroy,
	.get_shm = tab_text_buffer_get_shm,
	.begin_data_ptr_access = tab_text_buffer_begin_data_ptr_access,
	.end_data_ptr_access = tab_text_buffer_end_data_ptr_access,
};

static struct tab_text_buffer *tab_text_buffer_create(int width, int height)
{
	struct tab_text_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf)
		return NULL;
	buf->width = width;
	buf->height = height;
	buf->stride = width * 4;
	buf->format = DRM_FORMAT_ARGB8888;
	buf->data = calloc(1, (size_t)height * buf->stride);
	if (!buf->data)
	{
		free(buf);
		return NULL;
	}
	wlr_buffer_init(&buf->base, &tab_text_buffer_impl, width, height);
	return buf;
}

/* ── tab button text rendering ── */

static void tab_button_update_text(struct hsdwl_tab_button *btn,
		bool active, int width, int height)
{
	struct hsdwl_tab_group *group = btn->view->tab_group;
	if (!group || !btn->text)
		return;

	if (width < 4 || height < 4)
		return;

	const char *title = btn->view->cached_title[0]
		? btn->view->cached_title : "Untitled";

	float *tc = active
		? group->server->config.title_text_color_focused
		: group->server->config.title_text_color;

	cairo_surface_t *surf = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, width, height);
	cairo_t *cr = cairo_create(surf);

	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	PangoLayout *layout = pango_cairo_create_layout(cr);
	pango_layout_set_text(layout, title, -1);
	pango_layout_set_width(layout, (width - 12) * PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	PangoFontDescription *font = pango_font_description_from_string(
		"Sans 10");
	pango_layout_set_font_description(layout, font);

	cairo_set_source_rgba(cr, tc[0], tc[1], tc[2], tc[3]);

	int text_w, text_h;
	pango_layout_get_pixel_size(layout, &text_w, &text_h);
	int text_x = (width - text_w) / 2;
	int text_y = (height - text_h) / 2;
	cairo_move_to(cr, text_x, text_y);
	pango_cairo_show_layout(cr, layout);

	pango_font_description_free(font);
	g_object_unref(layout);
	cairo_destroy(cr);

	int cw = cairo_image_surface_get_width(surf);
	int ch = cairo_image_surface_get_height(surf);
	const unsigned char *src = cairo_image_surface_get_data(surf);

	struct tab_text_buffer *tbuf = tab_text_buffer_create(cw, ch);
	if (!tbuf)
	{
		cairo_surface_destroy(surf);
		return;
	}
	memcpy(tbuf->data, src, (size_t)ch * tbuf->stride);
	cairo_surface_destroy(surf);

	wlr_scene_buffer_set_buffer(btn->text, &tbuf->base);
	wlr_buffer_drop(&tbuf->base);
}

/* ── tab button lifecycle ── */

static struct hsdwl_tab_button *tab_button_create(
		struct hsdwl_tab_group *group, struct hsdwl_view *view)
{
	struct hsdwl_tab_button *btn = calloc(1, sizeof(*btn));
	if (!btn)
		return NULL;

	btn->view = view;

	btn->text = wlr_scene_buffer_create(group->scene_tree, NULL);
	if (!btn->text)
	{
		free(btn);
		return NULL;
	}
	btn->text->node.data = view;

	return btn;
}

static void tab_button_destroy(struct hsdwl_tab_button *btn)
{
	if (!btn)
		return;
	wlr_scene_node_destroy(&btn->text->node);
	btn->text = NULL;
	wl_list_remove(&btn->link);
	free(btn);
}

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

/* ── configure view to a given content size ── */

void view_configure_size(struct hsdwl_view *view, int w, int h)
{
	if (view->xdg_surface && view->xdg_surface->configured)
	{
		wlr_xdg_toplevel_set_size(
			view->xdg_surface->toplevel, w, h);
	}
	else if (view->xwayland_surface)
	{
		wlr_xwayland_surface_configure(
			view->xwayland_surface,
			view->xwayland_surface->x,
			view->xwayland_surface->y, w, h);
	}
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
	group->tab_bar_thickness = TAB_BAR_THICKNESS;
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

	if (group->active == view)
		return;

	struct hsdwl_view *prev = group->active;
	if (prev && prev->scene_tree)
		wlr_scene_node_set_enabled(&prev->scene_tree->node, false);

	group->active = view;
	if (view->scene_tree)
		wlr_scene_node_set_enabled(&view->scene_tree->node, true);

	view_focus(group->server, view);
	hsdwl_tab_group_update_layout(group);
}

void hsdwl_tab_group_destroy(struct hsdwl_tab_group *group)
{
	if (!group)
		return;

	struct hsdwl_tab_button *btn, *tmp;
	wl_list_for_each_safe(btn, tmp, &group->tab_buttons, link)
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

static void render_tab_bar_background(struct hsdwl_tab_group *group)
{
	if (!group->tab_bar_bg)
		return;

	int w = group->content_area_box.width;
	int h = group->tab_bar_thickness;
	if (w < 4 || h < 4)
		return;

	int num_views = wl_list_length(&group->tab_buttons);

	bool global_focused = group->active == group->server->focused_views[
		group->server->current_workspace];

	int seg_w = w / num_views;
	int r = group->server->config.titlebar_radius;

	cairo_surface_t *surf = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(surf);

	int idx = 0;
	struct hsdwl_tab_button *btn;
	wl_list_for_each(btn, &group->tab_buttons, link)
	{
		int sx = idx * seg_w;
		int sw = (idx == num_views - 1) ? w - sx : seg_w;

		bool seg_active = (btn->view == group->active);
		float *col = seg_active && global_focused
			? group->server->config.titlebar_color_focused
			: group->server->config.titlebar_color;

		cairo_set_source_rgba(cr, col[0], col[1], col[2], col[3]);

		if (group->maximized)
		{
			cairo_rectangle(cr, sx, 0, sw, h);
			cairo_fill(cr);
		}
		else if (idx == 0)
		{
			cairo_move_to(cr, 0, r);
			cairo_arc(cr, r, r, r, M_PI, 3 * M_PI_2);
			cairo_line_to(cr, sw, 0);
			cairo_line_to(cr, sw, h);
			cairo_line_to(cr, 0, h);
			cairo_close_path(cr);
			cairo_fill(cr);
		}
		else if (idx == num_views - 1)
		{
			cairo_move_to(cr, sx, 0);
			cairo_arc(cr, sx + sw - r, r, r, 3 * M_PI_2, 0);
			cairo_line_to(cr, sx + sw, h);
			cairo_line_to(cr, sx, h);
			cairo_close_path(cr);
			cairo_fill(cr);
		}
		else
		{
			cairo_rectangle(cr, sx, 0, sw, h);
			cairo_fill(cr);
		}

		idx++;
	}

	cairo_destroy(cr);

	int cw = cairo_image_surface_get_width(surf);
	int ch = cairo_image_surface_get_height(surf);
	const unsigned char *src = cairo_image_surface_get_data(surf);

	struct tab_text_buffer *tbuf = tab_text_buffer_create(cw, ch);
	if (!tbuf)
	{
		cairo_surface_destroy(surf);
		return;
	}
	memcpy(tbuf->data, src, (size_t)ch * tbuf->stride);
	cairo_surface_destroy(surf);

	wlr_scene_buffer_set_buffer(group->tab_bar_bg, &tbuf->base);
	wlr_buffer_drop(&tbuf->base);
}

void hsdwl_tab_group_update_layout(struct hsdwl_tab_group *group)
{
	if (!group || !group->scene_tree)
		return;

	render_tab_bar_background(group);

	int num_views = wl_list_length(&group->tab_buttons);
	if (num_views == 0)
		return;

	int avail = group->content_area_box.width;
	int seg_w = avail / num_views;

	int x = 0;
	struct hsdwl_tab_button *btn;
	wl_list_for_each(btn, &group->tab_buttons, link)
	{
		bool active = (btn->view == group->active);
		int sw = seg_w;
		if (x + sw > avail)
			sw = avail - x;
		wlr_scene_node_set_position(&btn->text->node,
			x, 0);
		tab_button_update_text(btn, active, sw,
			group->tab_bar_thickness);
		x += seg_w;
	}
}

void hsdwl_tab_group_reorder(struct hsdwl_tab_group *group,
		struct hsdwl_view *view, int new_index)
{
	if (!group || !view || view->tab_group != group)
		return;

	struct hsdwl_tab_button *btn;
	int idx = 0;
	wl_list_for_each(btn, &group->tab_buttons, link)
	{
		if (btn->view == view)
			break;
		idx++;
	}
	if (idx == new_index)
		return;

	/* Reorder by moving button to desired position */
	int count = wl_list_length(&group->tab_buttons);
	if (new_index < 0) new_index = 0;
	if (new_index >= count) new_index = count - 1;

	if (btn && btn->link.next)
	{
		wl_list_remove(&btn->link);
		struct hsdwl_tab_button *tmp;
		bool inserted = false;
		int pos = 0;
		wl_list_for_each(tmp, &group->tab_buttons, link)
		{
			if (pos == new_index)
			{
				wl_list_insert(tmp->link.prev, &btn->link);
				inserted = true;
				break;
			}
			pos++;
		}
		if (!inserted)
			wl_list_insert(group->tab_buttons.prev, &btn->link);
	}

	hsdwl_tab_group_update_layout(group);
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

/* ── maximize / restore ── */

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

	wlr_scene_node_set_position(&group->scene_tree->node,
		pad, 0);
	group->content_area_box.width = zw;
	group->content_area_box.height = zh;

	struct hsdwl_view *vi;
	wl_list_for_each(vi, &group->views, tab_group_link)
		view_configure_size(vi, zw, zh);

	group->zoomed = true;
	group->maximized = false;
	hsdwl_tab_group_update_layout(group);
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

		wlr_scene_node_set_position(&group->scene_tree->node,
			-SIDEBAR_WIDTH, 0);
		wlr_scene_node_raise_to_top(&group->scene_tree->node);

		int fh = obox.height - group->tab_bar_thickness;
		if (fh < 1) fh = 1;

		group->content_area_box.width = obox.width;
		group->content_area_box.height = fh;

		struct hsdwl_view *vi;
		wl_list_for_each(vi, &group->views, tab_group_link)
			view_configure_size(vi, obox.width, fh);

		group->zoomed = false;
		group->maximized = true;
		hsdwl_tab_group_update_layout(group);
		return;
	}

	hsdwl_tab_group_zoom(group, server);
}

void hsdwl_tab_group_restore(struct hsdwl_tab_group *group)
{
	if (!group || (!group->maximized && !group->zoomed))
		return;

	wlr_scene_node_set_position(&group->scene_tree->node,
		group->saved_geometry.x, group->saved_geometry.y);
	group->content_area_box.width = group->saved_geometry.width;
	group->content_area_box.height =
		group->saved_geometry.height - group->tab_bar_thickness;

	struct hsdwl_view *vi;
	wl_list_for_each(vi, &group->views, tab_group_link)
		view_configure_size(vi,
			group->content_area_box.width,
			group->content_area_box.height);

	group->maximized = false;
	group->zoomed = false;
	hsdwl_tab_group_update_layout(group);
}

/* ── preview overlay ── */

void hsdwl_tab_group_show_preview(struct hsdwl_server *server,
		struct hsdwl_view *target, double cursor_x, double cursor_y)
{
	(void)cursor_x;
	(void)cursor_y;

	if (!target || !target->scene_tree)
		return;

	if (server->preview_tree)
		hsdwl_tab_group_hide_preview(server);

	int tx, ty, tw, th;
	if (hsdwl_tab_group_is_member(target))
	{
		struct hsdwl_tab_group *g = target->tab_group;
		if (g->maximized)
		{
			tx = 0;
			ty = 0;
		}
		else
		{
			tx = g->scene_tree->node.x;
			ty = g->scene_tree->node.y;
			struct wlr_scene_tree *pn = g->scene_tree->node.parent;
			while (pn)
			{
				tx += pn->node.x;
				ty += pn->node.y;
				pn = pn->node.parent;
			}
		}
		tw = g->content_area_box.width;
		th = g->content_area_box.height;
	}
	else
	{
		if (target->maximized)
		{
			tx = 0;
			ty = 0;
		}
		else
		{
			tx = target->scene_tree->node.x;
			ty = target->scene_tree->node.y;
		}
		tw = 800; th = 600;
		if (target->xdg_surface && target->xdg_surface->configured)
		{
			tw = target->xdg_surface->geometry.width;
			th = target->xdg_surface->geometry.height;
		}
		else if (target->xwayland_surface)
		{
			tw = target->xwayland_surface->width;
			th = target->xwayland_surface->height;
		}
	}

	int bw = server->config.border_width;
	int tb = server->config.titlebar_height;
	if (tb < 0) tb = 0;
	int total_w = tw + 2 * bw;
	int total_h = th + (tb > 0 ? tb : bw) + bw;

	float *pc = server->config.preview_color;
	int r = server->config.titlebar_radius;

	cairo_surface_t *surf = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, total_w, total_h);
	cairo_t *cr = cairo_create(surf);

	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	cairo_move_to(cr, 0, r);
	cairo_arc(cr, r, r, r, M_PI, 3 * M_PI_2);
	cairo_arc(cr, total_w - r, r, r, 3 * M_PI_2, 0);
	cairo_line_to(cr, total_w, total_h);
	cairo_line_to(cr, 0, total_h);
	cairo_close_path(cr);

	cairo_set_source_rgba(cr, pc[0], pc[1], pc[2], pc[3] * 0.25f);
	cairo_fill(cr);

	cairo_surface_destroy(surf);
	cairo_destroy(cr);

	server->preview_tree = wlr_scene_tree_create(
		&server->scene->tree);
	if (!server->preview_tree)
		return;
	wlr_scene_node_raise_to_top(&server->preview_tree->node);

	struct wlr_scene_buffer *pb = wlr_scene_buffer_create(
		server->preview_tree, NULL);
	if (!pb)
	{
		hsdwl_tab_group_hide_preview(server);
		return;
	}
	wlr_scene_node_set_position(&pb->node, tx, ty);

	cairo_surface_t *surf2 = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, total_w, total_h);
	cairo_t *cr2 = cairo_create(surf2);

	cairo_set_source_rgba(cr2, 0, 0, 0, 0);
	cairo_set_operator(cr2, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr2);
	cairo_set_operator(cr2, CAIRO_OPERATOR_OVER);

	cairo_move_to(cr2, 0, r);
	cairo_arc(cr2, r, r, r, M_PI, 3 * M_PI_2);
	cairo_arc(cr2, total_w - r, r, r, 3 * M_PI_2, 0);
	cairo_line_to(cr2, total_w, total_h);
	cairo_line_to(cr2, 0, total_h);
	cairo_close_path(cr2);

	cairo_set_source_rgba(cr2, pc[0], pc[1], pc[2], pc[3] * 0.25f);
	cairo_fill(cr2);

	cairo_move_to(cr2, 0, r);
	cairo_arc(cr2, r, r, r, M_PI, 3 * M_PI_2);
	cairo_arc(cr2, total_w - r, r, r, 3 * M_PI_2, 0);
	cairo_line_to(cr2, total_w, TAB_BAR_THICKNESS);
	cairo_line_to(cr2, 0, TAB_BAR_THICKNESS);
	cairo_close_path(cr2);

	cairo_set_source_rgba(cr2, pc[0], pc[1], pc[2], pc[3] * 0.5f);
	cairo_fill(cr2);

	cairo_destroy(cr2);

	int cw = cairo_image_surface_get_width(surf2);
	int ch = cairo_image_surface_get_height(surf2);
	const unsigned char *src = cairo_image_surface_get_data(surf2);

	struct tab_text_buffer *tbuf = tab_text_buffer_create(cw, ch);
	if (!tbuf)
	{
		cairo_surface_destroy(surf2);
		return;
	}
	memcpy(tbuf->data, src, (size_t)ch * tbuf->stride);
	cairo_surface_destroy(surf2);

	wlr_scene_buffer_set_buffer(pb, &tbuf->base);
	wlr_buffer_drop(&tbuf->base);
}

void hsdwl_tab_group_hide_preview(struct hsdwl_server *server)
{
	if (!server->preview_tree)
		return;
	wlr_scene_node_destroy(&server->preview_tree->node);
	server->preview_tree = NULL;
}
