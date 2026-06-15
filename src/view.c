#define _GNU_SOURCE

#include "layer-shell.h"
#include "view.h"
#include "server.h"

#include <math.h>
#include <cairo.h>
#include <drm_fourcc.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

/* ── custom wlr_buffer wrapping a cairo-rendered pixel buffer ── */

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

/* ── titlebar text rendering ── */

void titlebar_text_update(struct hsdwl_view *view)
{
	if (!view->title_text_buf)
		return;
	int tb_h = view->server->config.titlebar_height;
	if (tb_h < 1)
		return;

	struct hsdwl_config *cfg = &view->server->config;

	int bw = cfg->border_width;
	int tw = 0, th = tb_h;
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

	/* draw background with rounded top corners */
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

	/* render text */
	PangoLayout *layout = pango_cairo_create_layout(cr);
	pango_layout_set_text(layout, title, -1);
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
	cairo_move_to(cr, 4, 2);
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

static void view_handle_set_title(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, set_title);
	if (view->xdg_surface && view->xdg_surface->toplevel
			&& view->xdg_surface->toplevel->title)
	{
		strncpy(view->cached_title,
			view->xdg_surface->toplevel->title,
			sizeof(view->cached_title) - 1);
		view->cached_title[sizeof(view->cached_title) - 1] = '\0';
	}
	titlebar_text_update(view);
}

static void view_handle_map(struct wl_listener *listener, void *data)
{
	fprintf(stderr, "TRACE: view_handle_map\n");
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, map);
	if (!view->scene_tree)
	{
		int bw = view->server->config.border_width;
		int tb = view->server->config.titlebar_height;
		view->scene_tree = wlr_scene_tree_create(
			view->server->workspaces[
				view->server->current_workspace]);
		if (!view->scene_tree)
			return;
		view->scene_tree->node.data = view;
		view->content_tree = wlr_scene_xdg_surface_create(
			view->scene_tree, view->xdg_surface);
		if (!view->content_tree)
			return;
		wlr_scene_node_set_position(
			&view->content_tree->node, bw, tb > 0 ? tb : bw);
		view_borders_create(view);
		wlr_scene_node_set_enabled(
			&view->scene_tree->node, true);
	}

	if (view->xdg_surface->toplevel)
	{
		if (view->decoration && !view->decoration_request_mode.notify)
		{
			view->decoration_request_mode.notify =
				decoration_handle_request_mode;
			wl_signal_add(&view->decoration->events.request_mode,
				&view->decoration_request_mode);
			wlr_xdg_toplevel_decoration_v1_set_mode(
				view->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		}
	}

	titlebar_text_update(view);
	view_focus(view->server, view);
}

struct wlr_surface *view_get_surface(struct hsdwl_view *view)
{
	return view->xdg_surface
		? view->xdg_surface->surface
		: view->xwayland_surface
			? view->xwayland_surface->surface
			: NULL;
}

void view_borders_create(struct hsdwl_view *view)
{
	fprintf(stderr, "TRACE: view_borders_create\n");
	if (view->xwayland_surface
			&& view->xwayland_surface->override_redirect)
		return;
	struct hsdwl_config *cfg = &view->server->config;
	for (int i = 0; i < 4; i++)
	{
		view->border_rects[i] = wlr_scene_rect_create(
			view->scene_tree, 1, 1,
			cfg->border_color);
	}
	if (cfg->titlebar_height > 0 && !view->title_text_buf)
	{
		view->title_text_buf = wlr_scene_buffer_create(
			view->scene_tree, NULL);
	}
	view_borders_update(view);
}

void view_borders_update(struct hsdwl_view *view)
{
	fprintf(stderr, "TRACE: view_borders_update\n");
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
	if (bw < 1)
	{
		for (int i = 0; i < 4; i++)
			wlr_scene_node_set_enabled(
				&view->border_rects[i]->node, false);
		return;
	}

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

void view_focus(struct hsdwl_server *server, struct hsdwl_view *view)
{
	fprintf(stderr, "TRACE: view_focus view=%p\n", (void*)view);
	server->focused_layer = NULL;

	if (!view)
	{
		wlr_seat_keyboard_clear_focus(server->seat);
		struct hsdwl_view *v;
		wl_list_for_each(v, &server->views, link)
		{
			if (!v->scene_tree || !v->scene_tree->node.enabled)
				continue;
			if (v->xwayland_surface
					&& v->xwayland_surface->override_redirect)
				continue;
			for (int i = 0; i < 4; i++)
			{
				if (v->border_rects[i])
					wlr_scene_rect_set_color(
						v->border_rects[i],
						server->config.border_color);
			}
			titlebar_text_update(v);
		}
		return;
	}
	if (!view->scene_tree)
		return;

	server->focused_views[server->current_workspace] = view;
	struct hsdwl_view *v;
	wl_list_for_each(v, &server->views, link)
	{
		if (!v->scene_tree || !v->scene_tree->node.enabled)
			continue;
		bool active = (v == view);
		if (v->xdg_surface && v->xdg_surface->configured
				&& v->xdg_surface->surface->buffer)
		{
			wlr_xdg_toplevel_set_activated(
				v->xdg_surface->toplevel, active);
			wlr_xdg_surface_schedule_configure(v->xdg_surface);
		}
		if (v->xwayland_surface
				&& (!v->xwayland_surface->override_redirect
					|| wlr_xwayland_surface_override_redirect_wants_focus(
						v->xwayland_surface)))
		{
			wlr_xwayland_surface_activate(
				v->xwayland_surface, active);
		}
		if (v->xwayland_surface
				&& v->xwayland_surface->override_redirect)
			continue;
		float *color = active
			? server->config.border_color_focused
			: server->config.border_color;
		for (int i = 0; i < 4; i++)
		{
			if (v->border_rects[i])
				wlr_scene_rect_set_color(
					v->border_rects[i], color);
		}
		titlebar_text_update(v);
	}

	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (kb)
	{
		struct wlr_surface *s = view_get_surface(view);
		if (s)
			wlr_seat_keyboard_notify_enter(server->seat,
				s, NULL, 0, NULL);
	}
}

static bool view_is_usable(struct hsdwl_view *v)
{
	if (!v->scene_tree || !v->scene_tree->node.enabled)
		return false;
	if (v->xdg_surface && v->xdg_surface->configured)
		return true;
	if (v->xwayland_surface && v->xwayland_surface->surface
			&& !v->xwayland_surface->override_redirect)
		return true;
	return false;
}

struct hsdwl_view *view_next(struct hsdwl_server *server,
		struct hsdwl_view *current)
{
	struct hsdwl_view *first = NULL;
	struct hsdwl_view *v;
	bool found = false;
	wl_list_for_each(v, &server->views, link)
	{
		if (!view_is_usable(v))
			continue;
		if (!first)
			first = v;
		if (found)
			return v;
		if (v == current)
			found = true;
	}
	return first;
}

struct hsdwl_view *view_prev(struct hsdwl_server *server,
		struct hsdwl_view *current)
{
	struct hsdwl_view *last = NULL;
	struct hsdwl_view *v;
	bool found = false;
	wl_list_for_each_reverse(v, &server->views, link)
	{
		if (!view_is_usable(v))
			continue;
		if (!last)
			last = v;
		if (found)
			return v;
		if (v == current)
			found = true;
	}
	return last;
}

static void view_handle_unmap(struct wl_listener *listener, void *data)
{
	fprintf(stderr, "TRACE: view_handle_unmap\n");
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, unmap);
	if (view->scene_tree)
		wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	if (view->xdg_surface && view->xdg_surface->toplevel)
	{
		wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, false);
		wlr_xdg_surface_schedule_configure(view->xdg_surface);
	}
	if (view->server->grabbed_view == view)
		view->server->grabbed_view = NULL;
	view_focus(view->server, view_next(view->server, view));
}

static void view_handle_commit(struct wl_listener *listener, void *data)
{
	fprintf(stderr, "TRACE: view_handle_commit\n");
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, commit);
	if (!view->xdg_surface)
		return;

	if (view->xdg_surface->initial_commit && view->xdg_surface->toplevel)
	{
		wlr_xdg_toplevel_set_activated(
			view->xdg_surface->toplevel, true);
		wlr_xdg_surface_schedule_configure(view->xdg_surface);
	}

	view_borders_update(view);
	titlebar_text_update(view);
}

static void view_handle_toplevel_destroy(struct wl_listener *listener, void *data)
{
	fprintf(stderr, "TRACE: view_handle_toplevel_destroy\n");
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, toplevel_destroy);
	wl_list_remove(&view->set_title.link);
	wl_list_init(&view->set_title.link);
	wl_list_remove(&view->toplevel_destroy.link);
}

static void view_handle_destroy(struct wl_listener *listener, void *data)
{
	fprintf(stderr, "TRACE: view_handle_destroy\n");
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, destroy);
	if (view->server->grabbed_view == view)
		view->server->grabbed_view = NULL;
	for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
	{
		if (view->server->focused_views[i] == view)
			view->server->focused_views[i] = NULL;
	}
	if (view->scene_tree)
	{
		view->scene_tree->node.data = NULL;
		wlr_scene_node_destroy(&view->scene_tree->node);
	}
	if (view->decoration_destroy.notify)
		wl_list_remove(&view->decoration_destroy.link);
	if (view->decoration_request_mode.notify)
		wl_list_remove(&view->decoration_request_mode.link);
	wl_list_remove(&view->link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->destroy.link);
	view->content_tree = NULL;
	view->scene_tree = NULL;
	free(view);
}

void view_close(struct hsdwl_view *view)
{
	if (!view)
		return;
	if (view->xwayland_surface
			&& view->xwayland_surface->override_redirect)
		return;
	if (view->xdg_surface && view->xdg_surface->toplevel)
		wlr_xdg_toplevel_send_close(view->xdg_surface->toplevel);
	else if (view->xwayland_surface)
		wlr_xwayland_surface_close(view->xwayland_surface);
}

void view_handle_new_xdg_toplevel(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;
	struct wlr_xdg_surface *xdg_surface = toplevel->base;

	struct hsdwl_view *view = calloc(1, sizeof(*view));
	if (!view)
		return;

	view->server = server;
	view->xdg_surface = xdg_surface;
	xdg_surface->data = view;
	wl_list_insert(&server->views, &view->link);

	view->map.notify = view_handle_map;
	wl_signal_add(&xdg_surface->surface->events.map, &view->map);
	view->unmap.notify = view_handle_unmap;
	wl_signal_add(&xdg_surface->surface->events.unmap, &view->unmap);
	view->commit.notify = view_handle_commit;
	wl_signal_add(&xdg_surface->surface->events.commit, &view->commit);
	view->destroy.notify = view_handle_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);
	view->set_title.notify = view_handle_set_title;
	wl_signal_add(&xdg_surface->toplevel->events.set_title,
		&view->set_title);
	view->toplevel_destroy.notify = view_handle_toplevel_destroy;
	wl_signal_add(&xdg_surface->toplevel->events.destroy,
		&view->toplevel_destroy);
	if (xdg_surface->toplevel->title)
	{
		strncpy(view->cached_title, xdg_surface->toplevel->title,
			sizeof(view->cached_title) - 1);
		view->cached_title[sizeof(view->cached_title) - 1] = '\0';
	}
}
