#define _GNU_SOURCE

#include "layer-shell.h"
#include "stage.h"
#include "tab-group.h"
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
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#include "animation.h"

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
	if (view->server->config.stage_manager_enabled)
		stage_manager_new_window(view->server, view);
	else
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
	if (!view->title_text_buf)
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
		struct hsdwl_tab_group *tg;
		wl_list_for_each(tg, &server->tab_groups, link)
			hsdwl_tab_group_update_layout(tg);
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

	struct hsdwl_tab_group *tg;
	wl_list_for_each(tg, &server->tab_groups, link)
		hsdwl_tab_group_update_layout(tg);

	if (view->tab_group && view->tab_group->scene_tree)
		wlr_scene_node_raise_to_top(
			&view->tab_group->scene_tree->node);
	else
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

static bool scene_tree_is_descendant(struct wlr_scene_tree *tree,
		struct wlr_scene_tree *ancestor)
{
	if (!tree || !ancestor) return false;
	struct wlr_scene_tree *p = tree;
	while (p)
	{
		if (p == ancestor) return true;
		p = p->node.parent;
	}
	return false;
}

static bool view_on_workspace(struct hsdwl_view *v,
		struct wlr_scene_tree *ws)
{
	if (!v->scene_tree || !ws)
		return false;
	if (scene_tree_is_descendant(v->scene_tree->node.parent, ws))
		return true;
	if (v->tab_group && v->tab_group->scene_tree
			&& scene_tree_is_descendant(
				v->tab_group->scene_tree->node.parent, ws))
		return true;
	return false;
}

bool view_is_on_workspace(struct hsdwl_view *view, struct wlr_scene_tree *ws)
{
	return view_on_workspace(view, ws);
}

struct hsdwl_view *view_next(struct hsdwl_server *server,
		struct hsdwl_view *current)
{
	struct wlr_scene_tree *ws =
		server->workspaces[server->current_workspace];
	struct hsdwl_view *first = NULL;
	struct hsdwl_view *v;
	bool found = false;
	wl_list_for_each(v, &server->views, link)
	{
		if (!view_is_usable(v) || !view_on_workspace(v, ws))
			continue;
		if (v->tab_group && v != v->tab_group->active)
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
	struct wlr_scene_tree *ws =
		server->workspaces[server->current_workspace];
	struct hsdwl_view *last = NULL;
	struct hsdwl_view *v;
	bool found = false;
	wl_list_for_each_reverse(v, &server->views, link)
	{
		if (!view_is_usable(v) || !view_on_workspace(v, ws))
			continue;
		if (v->tab_group && v != v->tab_group->active)
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
	if (!view->server->config.stage_manager_enabled)
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

	if (view->tab_group && view->xdg_surface->configured)
	{
		struct hsdwl_tab_group *g = view->tab_group;
		int cw = view->xdg_surface->geometry.width;
		int ch = view->xdg_surface->geometry.height;
		if (cw != g->content_area_box.width
				|| ch != g->content_area_box.height)
		{
			g->content_area_box.width = cw;
			g->content_area_box.height = ch;
			struct hsdwl_view *vi;
			wl_list_for_each(vi, &g->views, tab_group_link)
			{
				if (vi != view)
					view_configure_size(vi, cw, ch);
			}
		}
		hsdwl_tab_group_update_layout(g);
	}

	view_borders_update(view);
	titlebar_text_update(view);

	if (view->server->config.stage_manager_enabled)
		stage_manager_notify_surface_commit(view->server, view);
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
	if (view->server->grab_target == view)
		view->server->grab_target = NULL;
	for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
	{
		if (view->server->focused_views[i] == view)
			view->server->focused_views[i] = NULL;
	}
	if (view->tab_group)
		hsdwl_tab_group_remove_view(view->tab_group, view);
	wl_list_remove(&view->tab_group_link);
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
	if (view->server->config.stage_manager_enabled)
		stage_manager_notify_view_removed(view->server, view);

	/* cancel any active animation for this view */
	{
		struct hsdwl_animation *anim, *tmp;
		wl_list_for_each_safe(anim, tmp,
			&view->server->animations, link)
		{
			if (anim->user_data == view)
			{
				wl_list_remove(&anim->link);
				free(anim);
			}
		}
	}
	if (view->anim_overlay)
	{
		wlr_scene_node_destroy(&view->anim_overlay->node);
		view->anim_overlay = NULL;
	}

	free(view);
}

static void view_do_unmaximize(struct hsdwl_view *view)
{
	struct hsdwl_config *cfg = &view->server->config;
	wlr_scene_node_set_position(&view->scene_tree->node,
		view->saved_geometry.x, view->saved_geometry.y);

	if (view->xdg_surface && view->xdg_surface->configured)
		wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel,
			view->saved_geometry.width, view->saved_geometry.height);
	else if (view->xwayland_surface)
		wlr_xwayland_surface_configure(view->xwayland_surface,
			view->saved_geometry.x, view->saved_geometry.y,
			view->saved_geometry.width, view->saved_geometry.height);

	for (int i = 0; i < 4; i++)
		if (view->border_rects[i])
			wlr_scene_node_set_enabled(&view->border_rects[i]->node, true);
	if (view->title_text_buf)
		wlr_scene_node_set_enabled(&view->title_text_buf->node, true);

	int bw = cfg->border_width;
	int tb = cfg->titlebar_height > 0 ? cfg->titlebar_height : bw;
	if (view->content_tree)
		wlr_scene_node_set_position(&view->content_tree->node, bw, tb);

	view_borders_update(view);
	titlebar_text_update(view);
}

/* ── capture view content surface to buffer ── */

struct wlr_buffer *view_capture_content_only(
	struct hsdwl_server *server,
	struct hsdwl_view *view,
	int target_w, int target_h)
{
	struct wlr_surface *surface = view_get_surface(view);
	struct wlr_texture *texture = surface
		? wlr_surface_get_texture(surface) : NULL;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format fmt = {
		.format = DRM_FORMAT_ARGB8888,
		.len = 1,
		.modifiers = mods,
	};
	struct wlr_buffer *buf = wlr_allocator_create_buffer(
		server->allocator, target_w, target_h, &fmt);
	if (!buf)
		return NULL;

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, buf, NULL);
	if (!pass)
	{
		wlr_buffer_drop(buf);
		return NULL;
	}

	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .width = target_w, .height = target_h },
		.color = { 0.0f, 0.0f, 0.0f, 0.0f },
	});

	if (texture)
	{
		float tex_w = surface->current.width;
		float tex_h = surface->current.height;
		if (tex_w < 1 || tex_h < 1)
		{
			tex_w = target_w;
			tex_h = target_h;
		}
		float scale = fmin((float)target_w / tex_w,
			(float)target_h / tex_h);
		float fit_w = tex_w * scale;
		float fit_h = tex_h * scale;
		float fit_x = ((float)target_w - fit_w) / 2.0f;
		float fit_y = ((float)target_h - fit_h) / 2.0f;

		const float alpha = 1.0f;
		wlr_render_pass_add_texture(pass,
			&(struct wlr_render_texture_options){
				.texture = texture,
				.dst_box = {
					.x = (int)(fit_x + 0.5f),
					.y = (int)(fit_y + 0.5f),
					.width = (int)(fit_w + 0.5f),
					.height = (int)(fit_h + 0.5f),
				},
				.alpha = &alpha,
				.transform = WL_OUTPUT_TRANSFORM_NORMAL,
			});
	}

	if (!wlr_render_pass_submit(pass))
	{
		wlr_buffer_drop(buf);
		return NULL;
	}

	return buf;
}

/* ── capture full window (content + borders + titlebar) to buffer ── */

struct wlr_buffer *view_capture_full_window(
	struct hsdwl_server *server,
	struct hsdwl_view *view,
	int content_w, int content_h,
	int bw, int tb)
{
	int win_w = content_w + 2 * bw;
	int win_h = content_h + tb + bw;
	if (win_w < 1) win_w = 1;
	if (win_h < 1) win_h = 1;

	struct wlr_surface *surface = view_get_surface(view);
	struct wlr_texture *texture = surface
		? wlr_surface_get_texture(surface) : NULL;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format fmt = {
		.format = DRM_FORMAT_ARGB8888,
		.len = 1,
		.modifiers = mods,
	};
	struct wlr_buffer *buf = wlr_allocator_create_buffer(
		server->allocator, win_w, win_h, &fmt);
	if (!buf)
		return NULL;

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, buf, NULL);
	if (!pass)
	{
		wlr_buffer_drop(buf);
		return NULL;
	}

	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .width = win_w, .height = win_h },
		.color = { 0.0f, 0.0f, 0.0f, 0.0f },
	});

	bool focused = (view == server->focused_views[
		server->current_workspace]);
	float *bcol = focused
		? server->config.border_color_focused
		: server->config.border_color;
	float *tcol = focused
		? server->config.titlebar_color_focused
		: server->config.titlebar_color;

	/* titlebar background (solid color — actual titlebar stays
	   visible in the scene_tree during animation so we don't need
	   to replicate the text here) */
	if (tb > 0)
	{
		wlr_render_pass_add_rect(pass,
			&(struct wlr_render_rect_options){
				.box = { .width = content_w + 2 * bw, .height = tb },
				.color = { tcol[0], tcol[1], tcol[2], tcol[3] },
			});
	}

	/* borders */
	if (bw > 0)
	{
		int side_y = tb > 0 ? tb : bw;
		wlr_render_pass_add_rect(pass,
			&(struct wlr_render_rect_options){
				.box = { .x = 0, .y = side_y,
					.width = bw, .height = content_h },
				.color = { bcol[0], bcol[1], bcol[2], bcol[3] },
			});
		wlr_render_pass_add_rect(pass,
			&(struct wlr_render_rect_options){
				.box = { .x = content_w + bw, .y = side_y,
					.width = bw, .height = content_h },
				.color = { bcol[0], bcol[1], bcol[2], bcol[3] },
			});
		if (tb == 0)
		{
			wlr_render_pass_add_rect(pass,
				&(struct wlr_render_rect_options){
					.box = { .x = bw, .y = 0,
						.width = content_w, .height = bw },
					.color = { bcol[0], bcol[1], bcol[2], bcol[3] },
				});
		}
		int bot_y = tb > 0 ? tb + content_h : bw + content_h;
		wlr_render_pass_add_rect(pass,
			&(struct wlr_render_rect_options){
				.box = { .x = bw, .y = bot_y,
					.width = content_w, .height = bw },
				.color = { bcol[0], bcol[1], bcol[2], bcol[3] },
			});
	}

	/* content texture at (bw, tb) */
	if (texture)
	{
		float tex_w = surface->current.width;
		float tex_h = surface->current.height;
		if (tex_w < 1 || tex_h < 1)
		{
			tex_w = content_w;
			tex_h = content_h;
		}
		float scale = fmin((float)content_w / tex_w,
			(float)content_h / tex_h);
		float fit_w = tex_w * scale;
		float fit_h = tex_h * scale;
		float fit_x = bw + ((float)content_w - fit_w) / 2.0f;
		float fit_y = tb + ((float)content_h - fit_h) / 2.0f;

		const float alpha = 1.0f;
		wlr_render_pass_add_texture(pass,
			&(struct wlr_render_texture_options){
				.texture = texture,
				.dst_box = {
					.x = (int)(fit_x + 0.5f),
					.y = (int)(fit_y + 0.5f),
					.width = (int)(fit_w + 0.5f),
					.height = (int)(fit_h + 0.5f),
				},
				.alpha = &alpha,
				.transform = WL_OUTPUT_TRANSFORM_NORMAL,
			});
	}

	if (!wlr_render_pass_submit(pass))
	{
		wlr_buffer_drop(buf);
		return NULL;
	}

	return buf;
}

/* ── animation completion callbacks ── */

static void destroy_anim_overlay(struct hsdwl_server *server,
	struct hsdwl_view *view)
{
	if (view->anim_overlay)
	{
		animation_cancel_buffer(server, view->anim_overlay);
		wlr_scene_node_destroy(&view->anim_overlay->node);
		view->anim_overlay = NULL;
	}
	if (view->content_tree)
		wlr_scene_node_set_enabled(&view->content_tree->node, true);
}

static void view_anim_zoom_finish(struct hsdwl_server *server, void *user_data)
{
	struct hsdwl_view *view = user_data;
	destroy_anim_overlay(server, view);
	view_borders_update(view);
	titlebar_text_update(view);
}

static void view_anim_full_finish(struct hsdwl_server *server, void *user_data)
{
	struct hsdwl_view *view = user_data;
	destroy_anim_overlay(server, view);
	if (view->content_tree)
		wlr_scene_node_set_position(&view->content_tree->node, 0, 0);
	for (int i = 0; i < 4; i++)
		if (view->border_rects[i])
			wlr_scene_node_set_enabled(&view->border_rects[i]->node, false);
	if (view->title_text_buf)
		wlr_scene_node_set_enabled(&view->title_text_buf->node, false);
}

static void view_anim_unmaximize_finish(struct hsdwl_server *server,
		void *user_data)
{
	struct hsdwl_view *view = user_data;
	destroy_anim_overlay(server, view);
	view_do_unmaximize(view);
}

/* Capture current content (no decorations) and create overlay */
static struct wlr_scene_buffer *create_content_overlay(
	struct hsdwl_server *server, struct hsdwl_view *view,
	int content_w, int content_h,
	int abs_x, int abs_y)
{
	struct wlr_buffer *cap = view_capture_full_window(
		server, view, content_w, content_h, 0, 0);
	if (!cap) return NULL;

	wlr_scene_node_raise_to_top(&server->animation_tree->node);

	struct wlr_scene_buffer *ov = wlr_scene_buffer_create(
		server->animation_tree, cap);
	wlr_buffer_drop(cap);
	wlr_scene_node_set_position(&ov->node, abs_x, abs_y);
	wlr_scene_buffer_set_dest_size(ov, content_w, content_h);
	wlr_scene_node_raise_to_top(&ov->node);
	return ov;
}

/* Get current content size from view */
static void get_content_size(struct hsdwl_view *view, int *w, int *h)
{
	if (view->xdg_surface && view->xdg_surface->configured)
	{
		*w = view->xdg_surface->geometry.width;
		*h = view->xdg_surface->geometry.height;
	}
	else if (view->xwayland_surface)
	{
		*w = view->xwayland_surface->width;
		*h = view->xwayland_surface->height;
	}
	else
	{
		*w = 1; *h = 1;
	}
}

void view_maximize(struct hsdwl_server *server, struct hsdwl_view *view)
{
	if (!view || !view->scene_tree)
		return;

	if (hsdwl_tab_group_is_member(view))
	{
		hsdwl_tab_group_maximize(view->tab_group, server);
		return;
	}

	destroy_anim_overlay(server, view);

	struct hsdwl_config *cfg = &server->config;
	int bw = cfg->border_width;
	int tb = cfg->titlebar_height;
	int ct_off_x = bw;
	int ct_off_y = tb > 0 ? tb : bw;

	/* ── Stage 3: restore (undo full-max or zoom) ── */
	if (view->maximized)
	{
		struct wlr_output *wlr_o = wlr_output_layout_output_at(
			server->output_layout,
			view->saved_geometry.x + view->saved_geometry.width / 2,
			view->saved_geometry.y + view->saved_geometry.height / 2);
		if (!wlr_o)
		{
			view_do_unmaximize(view);
			view->maximized = false;
			view->zoomed = false;
			return;
		}

		int src_cw, src_ch;
		get_content_size(view, &src_cw, &src_ch);
		int src_abs_x = SIDEBAR_WIDTH + (int)view->scene_tree->node.x
			+ (view->content_tree ? view->content_tree->node.x : 0);
		int src_abs_y = (int)view->scene_tree->node.y
			+ (view->content_tree ? view->content_tree->node.y : 0);

		struct wlr_scene_buffer *ov = create_content_overlay(
			server, view, src_cw, src_ch, src_abs_x, src_abs_y);
		if (!ov) return;

		if (view->content_tree)
			wlr_scene_node_set_enabled(
				&view->content_tree->node, false);

		wlr_scene_node_set_position(&view->scene_tree->node,
			view->saved_geometry.x, view->saved_geometry.y);
		if (view->content_tree)
			wlr_scene_node_set_position(
				&view->content_tree->node, ct_off_x, ct_off_y);
		for (int i = 0; i < 4; i++)
			if (view->border_rects[i])
				wlr_scene_node_set_enabled(
					&view->border_rects[i]->node, true);
		if (view->title_text_buf)
			wlr_scene_node_set_enabled(&view->title_text_buf->node, true);

		if (view->xdg_surface && view->xdg_surface->configured)
			wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel,
				view->saved_geometry.width, view->saved_geometry.height);
		else if (view->xwayland_surface)
			wlr_xwayland_surface_configure(view->xwayland_surface,
				view->saved_geometry.x, view->saved_geometry.y,
				view->saved_geometry.width, view->saved_geometry.height);
		view_borders_update(view);
		titlebar_text_update(view);

		view->anim_overlay = ov;

		int tgt_abs_x = SIDEBAR_WIDTH
			+ (int)view->saved_geometry.x + ct_off_x;
		int tgt_abs_y = (int)view->saved_geometry.y + ct_off_y;
		int tgt_cw = view->saved_geometry.width;
		int tgt_ch = view->saved_geometry.height;

		animation_create(server, ov, 200, HSDWL_EASE_OUT_QUAD,
			src_abs_x, src_abs_y, src_cw, src_ch,
			tgt_abs_x, tgt_abs_y, tgt_cw, tgt_ch,
			view_anim_unmaximize_finish, view);

		view->maximized = false;
		view->zoomed = false;
		return;
	}

	/* ── Stage 2: full maximize (zoom → full-max, hide decorations) ── */
	if (view->zoomed)
	{
		struct wlr_output *wlr_o = wlr_output_layout_output_at(
			server->output_layout,
			view->saved_geometry.x + view->saved_geometry.width / 2,
			view->saved_geometry.y + view->saved_geometry.height / 2);
		if (!wlr_o) return;

		struct wlr_box obox;
		wlr_output_layout_get_box(server->output_layout, wlr_o, &obox);

		int fw = obox.width;
		if (fw < 1) fw = 1;

		int src_cw, src_ch;
		get_content_size(view, &src_cw, &src_ch);
		int src_abs_x = SIDEBAR_WIDTH + (int)view->scene_tree->node.x
			+ ct_off_x;
		int src_abs_y = (int)view->scene_tree->node.y + ct_off_y;

		struct wlr_scene_buffer *ov = create_content_overlay(
			server, view, src_cw, src_ch, src_abs_x, src_abs_y);
		if (!ov) return;

		if (view->content_tree)
			wlr_scene_node_set_enabled(
				&view->content_tree->node, false);

		wlr_scene_node_set_position(
			&view->scene_tree->node, -SIDEBAR_WIDTH, 0);
		if (view->content_tree)
			wlr_scene_node_set_position(
				&view->content_tree->node, 0, 0);
		for (int i = 0; i < 4; i++)
			if (view->border_rects[i])
				wlr_scene_node_set_enabled(
					&view->border_rects[i]->node, false);
		if (view->title_text_buf)
			wlr_scene_node_set_enabled(
				&view->title_text_buf->node, false);

		if (view->xdg_surface && view->xdg_surface->configured)
			wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel,
				fw, obox.height);
		else if (view->xwayland_surface)
			wlr_xwayland_surface_configure(view->xwayland_surface,
				-SIDEBAR_WIDTH, 0, fw, obox.height);

		view->anim_overlay = ov;

		int tgt_abs_x = 0;
		int tgt_abs_y = 0;
		int tgt_cw = fw;
		int tgt_ch = obox.height;

		animation_create(server, ov, 200, HSDWL_EASE_OUT_QUAD,
			src_abs_x, src_abs_y, src_cw, src_ch,
			tgt_abs_x, tgt_abs_y, tgt_cw, tgt_ch,
			view_anim_full_finish, view);

		view->zoomed = false;
		view->maximized = true;
		return;
	}

	/* ── Stage 1: zoom (normal → zoomed, keep decorations) ── */
	view->saved_geometry.x = view->scene_tree->node.x;
	view->saved_geometry.y = view->scene_tree->node.y;
	if (view->xdg_surface && view->xdg_surface->configured)
	{
		view->saved_geometry.width = view->xdg_surface->geometry.width;
		view->saved_geometry.height = view->xdg_surface->geometry.height;
	}
	else if (view->xwayland_surface)
	{
		view->saved_geometry.width = view->xwayland_surface->width;
		view->saved_geometry.height = view->xwayland_surface->height;
	}
	else return;

	struct wlr_output *wlr_o = wlr_output_layout_output_at(
		server->output_layout,
		view->saved_geometry.x + view->saved_geometry.width / 2,
		view->saved_geometry.y + view->saved_geometry.height / 2);
	if (!wlr_o) return;

	struct wlr_box obox;
	wlr_output_layout_get_box(server->output_layout, wlr_o, &obox);

	int pad = 16;
	int zw = obox.width - SIDEBAR_WIDTH - pad;
	int zh = obox.height;
	if (zw < 1) zw = 1;
	int cw = zw - 2 * bw;
	int ch = zh - (tb > 0 ? tb : 0) - bw;
	if (cw < 1) cw = 1;
	if (ch < 1) ch = 1;

	int src_cw = view->saved_geometry.width;
	int src_ch = view->saved_geometry.height;
	int src_abs_x = SIDEBAR_WIDTH + (int)view->saved_geometry.x + ct_off_x;
	int src_abs_y = (int)view->saved_geometry.y + ct_off_y;

	struct wlr_scene_buffer *ov = create_content_overlay(
		server, view, src_cw, src_ch, src_abs_x, src_abs_y);
	if (!ov) return;

	if (view->content_tree)
		wlr_scene_node_set_enabled(&view->content_tree->node, false);

	wlr_scene_node_set_position(&view->scene_tree->node, pad, 0);
	if (view->xdg_surface && view->xdg_surface->configured)
		wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel, cw, ch);
	else if (view->xwayland_surface)
		wlr_xwayland_surface_configure(view->xwayland_surface,
			pad, 0, cw, ch);

	view->anim_overlay = ov;

	int tgt_abs_x = SIDEBAR_WIDTH + pad + ct_off_x;
	int tgt_abs_y = ct_off_y;
	int tgt_cw = cw;
	int tgt_ch = ch;

	animation_create(server, ov, 200, HSDWL_EASE_OUT_QUAD,
		src_abs_x, src_abs_y, src_cw, src_ch,
		tgt_abs_x, tgt_abs_y, tgt_cw, tgt_ch,
		view_anim_zoom_finish, view);

	view->zoomed = true;
	view->maximized = false;
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
	view->tab_group = NULL;
	wl_list_init(&view->tab_group_link);
	view->maximized = false;
	view->zoomed = false;
	memset(&view->saved_geometry, 0, sizeof(view->saved_geometry));
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
