#define _GNU_SOURCE

#include "tab-group-layout.h"
#include "server.h"
#include "deco.h"

#include <math.h>

#include <cairo.h>
#include <drm_fourcc.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>

#define TAB_BAR_THICKNESS 28



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



struct hsdwl_tab_button *tab_button_create(
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

void tab_button_destroy(struct hsdwl_tab_button *btn)
{
	if (!btn)
		return;
	wlr_scene_node_destroy(&btn->text->node);
	btn->text = NULL;
	wl_list_remove(&btn->link);
	free(btn);
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



#define PREVIEW_ALPHA 0.25f

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
			struct wlr_scene_tree *pn = target->scene_tree->node.parent;
			while (pn)
			{
				tx += pn->node.x;
				ty += pn->node.y;
				pn = pn->node.parent;
			}
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

	cairo_set_source_rgba(cr2, pc[0], pc[1], pc[2], pc[3] * PREVIEW_ALPHA);
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
