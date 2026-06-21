#define _GNU_SOURCE

#include "deco.h"
#include "server.h"

#include <cairo.h>
#include <drm_fourcc.h>
#include <math.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>


struct title_buffer
{
	struct wlr_buffer base;
	unsigned char *data;
	int width, height, stride;
	uint32_t format;
};

static void title_buffer_destroy(struct wlr_buffer *wbuffer)
{
	struct title_buffer *buf = wl_container_of(wbuffer, buf, base);
	free(buf->data);
	free(buf);
}

static bool title_buffer_get_shm(struct wlr_buffer *wbuffer,
		struct wlr_shm_attributes *attribs)
{
	struct title_buffer *buf = wl_container_of(wbuffer, buf, base);
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

static bool title_buffer_begin_data_ptr_access(
		struct wlr_buffer *wbuffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride)
{
	(void)flags;
	struct title_buffer *buf = wl_container_of(wbuffer, buf, base);
	*data = buf->data;
	*format = buf->format;
	*stride = buf->stride;
	return true;
}

static void title_buffer_end_data_ptr_access(struct wlr_buffer *wbuffer)
{
	(void)wbuffer;
}

static const struct wlr_buffer_impl title_buffer_impl = {
	.destroy = title_buffer_destroy,
	.get_shm = title_buffer_get_shm,
	.begin_data_ptr_access = title_buffer_begin_data_ptr_access,
	.end_data_ptr_access = title_buffer_end_data_ptr_access,
};

static struct title_buffer *title_buffer_create(
		int width, int height)
{
	struct title_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf) return NULL;
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
	wlr_buffer_init(&buf->base, &title_buffer_impl, width, height);
	return buf;
}


void titlebar_text_update(struct hsdwl_view *view)
{
	if (!view->title_text_buf)
		return;
	if (view->tab_group)
		return;

	struct hsdwl_config *cfg = &view->server->config;

	int bw = cfg->border_width;
	int tw = 0, th = cfg->titlebar_height;
	if (view->xdg_surface && view->xdg_surface->configured)
		tw = view->xdg_surface->geometry.width + 2 * bw;
	else if (view->xwayland_surface)
		tw = view->xwayland_surface->width + 2 * bw;
	if (tw < 4 || th < 4)
		return;

	const char *title = view->cached_title[0]
		? view->cached_title : "Untitled";

	bool focused = (view == view->server->focused_views[
		view->server->current_workspace]);

	cairo_surface_t *surf = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, tw, th);
	cairo_t *cr = cairo_create(surf);

	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_SLIGHT);
	cairo_set_font_options(cr, fo);
	cairo_font_options_destroy(fo);

	float *bg = focused
		? cfg->titlebar_color_focused
		: cfg->titlebar_color;
	cairo_set_source_rgba(cr, bg[0], bg[1], bg[2], bg[3]);
	int r = cfg->titlebar_radius;
	if (r > th / 2)
		r = th / 2;
	if (r > 0)
	{
		cairo_move_to(cr, 0, r);
		cairo_arc(cr, r, r, r, M_PI, 3 * M_PI_2);
		cairo_arc(cr, tw - r, r, r, 3 * M_PI_2, 0);
		cairo_line_to(cr, tw, th);
		cairo_line_to(cr, 0, th);
		cairo_close_path(cr);
	}
	else
	{
		cairo_rectangle(cr, 0, 0, tw, th);
	}
	cairo_fill(cr);

	PangoLayout *layout = pango_cairo_create_layout(cr);
	pango_layout_set_text(layout, title, -1);
	pango_layout_set_width(layout, (tw - 24) * PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	const char *weight = cfg->title_font_weight;
	char font_desc[256];
	if (weight && weight[0])
		snprintf(font_desc, sizeof(font_desc), "%s %s %d",
			cfg->title_font, weight,
			cfg->title_font_size);
	else
		snprintf(font_desc, sizeof(font_desc), "%s %d",
			cfg->title_font,
			cfg->title_font_size);
	PangoFontDescription *font = pango_font_description_from_string(font_desc);
	pango_layout_set_font_description(layout, font);

	float *tc = focused
		? cfg->title_text_color_focused
		: cfg->title_text_color;
	cairo_set_source_rgba(cr, tc[0], tc[1], tc[2], tc[3]);

	PangoRectangle ink_r, log_r;
	pango_layout_get_pixel_extents(layout, &ink_r, &log_r);
	int baseline_y = (th - ink_r.height) / 2 - ink_r.y + 1;
	int text_x = (tw - log_r.width) / 2 - log_r.x;

	cairo_move_to(cr, text_x, baseline_y);
	pango_cairo_show_layout(cr, layout);

	pango_font_description_free(font);
	g_object_unref(layout);
	cairo_destroy(cr);

	int w = cairo_image_surface_get_width(surf);
	int h = cairo_image_surface_get_height(surf);
	const unsigned char *src = cairo_image_surface_get_data(surf);

	struct title_buffer *tbuf = title_buffer_create(w, h);
	if (!tbuf)
	{
		cairo_surface_destroy(surf);
		return;
	}
	memcpy(tbuf->data, src, (size_t)h * tbuf->stride);

	cairo_surface_destroy(surf);

	wlr_scene_buffer_set_buffer(view->title_text_buf, &tbuf->base);
	wlr_buffer_drop(&tbuf->base);
}


void view_borders_create(struct hsdwl_view *view)
{
	if (view->xwayland_surface
			&& view->xwayland_surface->override_redirect)
		return;
	struct hsdwl_config *cfg = &view->server->config;

	if (cfg->shadow_enabled && !view->shadow_rect) {
		float color[4] = {
			cfg->shadow_color[0],
			cfg->shadow_color[1],
			cfg->shadow_color[2],
			cfg->shadow_opacity,
		};
		view->shadow_rect = wlr_scene_rect_create(
			view->scene_tree, 1, 1, color);
		wlr_scene_node_lower_to_bottom(
			&view->shadow_rect->node);
	}

	for (int i = 0; i < 4; i++)
	{
		view->border_rects[i] = wlr_scene_rect_create(
			view->scene_tree, 1, 1,
			cfg->border_color);
	}
	if (!view->title_text_buf)
	{
		view->title_text_buf = wlr_scene_buffer_create(
			view->scene_tree, NULL);
	}
	view_borders_update(view);
}

void view_borders_update(struct hsdwl_view *view)
{
	if (!view->scene_tree || !view->content_tree)
		return;
	if (!view->border_rects[0])
		return;
	int cw = 0, ch = 0;
	if (view->xdg_surface && view->xdg_surface->configured)
	{
		cw = view->xdg_surface->geometry.width;
		ch = view->xdg_surface->geometry.height;
	}
	else if (view->xwayland_surface)
	{
		cw = view->xwayland_surface->width;
		ch = view->xwayland_surface->height;
	}
	if (cw < 1 || ch < 1)
		return;
	int bw = view->server->config.border_width;
	int tb = view->server->config.titlebar_height;
	if (tb < 0) tb = 0;

	if (view->shadow_rect)
	{
		int shadow_w = cw + 2 * bw;
		int shadow_h;
		if (tb > 0)
			shadow_h = tb + ch + bw;
		else
			shadow_h = ch + 2 * bw;

		struct hsdwl_config *cfg = &view->server->config;
		float color[4] = {
			cfg->shadow_color[0],
			cfg->shadow_color[1],
			cfg->shadow_color[2],
			cfg->shadow_opacity,
		};
		wlr_scene_rect_set_size(view->shadow_rect,
			shadow_w, shadow_h);
		wlr_scene_rect_set_color(view->shadow_rect, color);
		wlr_scene_node_set_position(
			&view->shadow_rect->node,
			cfg->shadow_x_offset,
			cfg->shadow_y_offset);
		wlr_scene_node_set_enabled(
			&view->shadow_rect->node, true);
	}

	if (view->tab_group)
		return;

	if (bw < 1)
	{
		for (int i = 0; i < 4; i++)
			wlr_scene_node_set_enabled(
				&view->border_rects[i]->node, false);
		return;
	}

	for (int i = 0; i < 4; i++)
		wlr_scene_node_set_enabled(
			&view->border_rects[i]->node, true);

	int total_h = tb + ch;
	if (tb > 0)
	{
		wlr_scene_node_set_enabled(
			&view->border_rects[0]->node, false);
		wlr_scene_rect_set_size(
			view->border_rects[1], cw + bw * 2, bw);
		wlr_scene_node_set_position(
			&view->border_rects[1]->node, 0, tb + ch);
		wlr_scene_rect_set_size(
			view->border_rects[2], bw, ch);
		wlr_scene_node_set_position(
			&view->border_rects[2]->node, 0, tb);
		wlr_scene_rect_set_size(
			view->border_rects[3], bw, ch);
		wlr_scene_node_set_position(
			&view->border_rects[3]->node, cw + bw, tb);
	}
	else
	{
		wlr_scene_node_set_enabled(
			&view->border_rects[0]->node, true);
		wlr_scene_rect_set_size(
			view->border_rects[0], cw + bw * 2, bw);
		wlr_scene_node_set_position(
			&view->border_rects[0]->node, 0, 0);
		wlr_scene_rect_set_size(
			view->border_rects[1], cw + bw * 2, bw);
		wlr_scene_node_set_position(
			&view->border_rects[1]->node, 0, bw + total_h);
		wlr_scene_rect_set_size(
			view->border_rects[2], bw, total_h);
		wlr_scene_node_set_position(
			&view->border_rects[2]->node, 0, bw);
		wlr_scene_rect_set_size(
			view->border_rects[3], bw, total_h);
		wlr_scene_node_set_position(
			&view->border_rects[3]->node, cw + bw, bw);
	}

	if (view->title_text_buf)
	{
		wlr_scene_node_set_position(
			&view->title_text_buf->node, 0, 0);
	}

}
