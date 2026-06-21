#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "layer-shell.h"
#include "server.h"
#include "output.h"

#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

static uint32_t exclusive_edge_from_anchor(uint32_t anchor)
{
	if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)
		return ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
		return ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)
		return ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)
		return ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	return 0;
}

static void configure_and_position(
	struct hsdwl_layer_surface *layer, struct wlr_box *area)
{
	struct wlr_layer_surface_v1 *ls = layer->layer_surface;
	struct wlr_layer_surface_v1_state *state = &ls->current;
	uint32_t anchor = state->anchor;

	int w = state->desired_width;
	int h = state->desired_height;
	if (w == 0)
		w = area->width;
	if (h == 0)
		h = area->height;

	if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
			&& anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)
	{
		w = area->width - state->margin.left - state->margin.right;
		if (w < 0) w = 0;
	}
	if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
			&& anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
	{
		h = area->height - state->margin.top - state->margin.bottom;
		if (h < 0) h = 0;
	}

	if (w != (int)state->actual_width
			|| h != (int)state->actual_height)
	{
		wlr_layer_surface_v1_configure(ls, w, h);
	}
	else
	{
		w = state->actual_width;
		h = state->actual_height;
	}

	int x = area->x;
	int y = area->y;

	if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
			&& anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)
	{
		x = area->x + state->margin.left;
	}
	else if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)
	{
		x = area->x + state->margin.left;
	}
	else if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)
	{
		x = area->x + area->width - state->margin.right - w;
	}
	else
	{
		x = area->x + (area->width - w) / 2;
	}

	if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
			&& anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
	{
		y = area->y + state->margin.top;
	}
	else if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)
	{
		y = area->y + state->margin.top;
	}
	else if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
	{
		y = area->y + area->height - state->margin.bottom - h;
	}
	else
	{
		y = area->y + (area->height - h) / 2;
	}

	if (w < 0) w = 0;
	if (h < 0) h = 0;

	if (layer->scene_tree)
	{
		wlr_scene_node_set_position(
			&layer->scene_tree->node, x, y);
	}

	int ez = state->exclusive_zone;
	if (ez > 0)
	{
		uint32_t edge = exclusive_edge_from_anchor(anchor);
		if (edge)
		{
			if (edge == ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)
			{
				area->y += ez;
				area->height -= ez;
			}
			else if (edge == ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
			{
				area->height -= ez;
			}
			else if (edge == ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)
			{
				area->x += ez;
				area->width -= ez;
			}
			else if (edge == ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)
			{
				area->width -= ez;
			}
		}
	}
}

static void layer_popup_handle_destroy(
	struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_layer_popup *popup = wl_container_of(
		listener, popup, destroy);
	wl_list_remove(&popup->link);
	wl_list_remove(&popup->destroy.link);
	wlr_scene_node_destroy(&popup->scene_tree->node);
	free(popup);
}

static void layer_surface_handle_new_popup(
	struct wl_listener *listener, void *data)
{
	struct hsdwl_layer_surface *layer = wl_container_of(
		listener, layer, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;

	struct hsdwl_layer_popup *popup = calloc(1, sizeof(*popup));
	if (!popup)
		return;

	popup->wlr_popup = wlr_popup;
	popup->scene_tree = wlr_scene_xdg_surface_create(
		layer->scene_tree, wlr_popup->base);
	if (!popup->scene_tree)
	{
		free(popup);
		return;
	}

	popup->destroy.notify = layer_popup_handle_destroy;
	wl_signal_add(&wlr_popup->base->events.destroy,
		&popup->destroy);

	wl_list_insert(&layer->popups, &popup->link);

	struct wlr_box output_box;
	wlr_output_layout_get_box(layer->server->output_layout,
		layer->layer_surface->output, &output_box);
	wlr_xdg_popup_unconstrain_from_box(wlr_popup, &output_box);
}

static void layer_surface_handle_map(
	struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_layer_surface *layer = wl_container_of(
		listener, layer, map);

	if (!layer->scene_tree)
	{
		enum zwlr_layer_shell_v1_layer l =
			layer->layer_surface->current.layer;
		if (l > 3) l = 3;

		layer->scene_tree = wlr_scene_tree_create(
			layer->server->layer_trees[l]);
		if (!layer->scene_tree)
		{
			wlr_log(WLR_ERROR,
				"wlr_scene_tree_create for layer failed");
			return;
		}

		if (!wlr_scene_surface_create(
				layer->scene_tree,
				layer->layer_surface->surface))
		{
			wlr_log(WLR_ERROR,
				"wlr_scene_surface_create for layer failed");
			return;
		}
	}

	wlr_scene_node_set_enabled(
		&layer->scene_tree->node, true);
	layer_shell_rearrange(layer->server);
}

static void layer_surface_handle_unmap(
	struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_layer_surface *layer = wl_container_of(
		listener, layer, unmap);

	if (layer->scene_tree)
		wlr_scene_node_set_enabled(
			&layer->scene_tree->node, false);

	if (layer->server->focused_layer == layer)
		layer->server->focused_layer = NULL;
}

static void layer_surface_handle_commit(
	struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_layer_surface *layer = wl_container_of(
		listener, layer, commit);

	layer_shell_rearrange(layer->server);
}

static void layer_surface_handle_destroy(
	struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_layer_surface *layer = wl_container_of(
		listener, layer, destroy);
	struct hsdwl_server *server = layer->server;

	if (server->focused_layer == layer)
		server->focused_layer = NULL;

	wl_list_remove(&layer->map.link);
	wl_list_remove(&layer->unmap.link);
	wl_list_remove(&layer->commit.link);
	wl_list_remove(&layer->new_popup.link);
	wl_list_remove(&layer->destroy.link);
	wl_list_remove(&layer->link);

	if (layer->scene_tree)
		wlr_scene_node_destroy(
			&layer->scene_tree->node);

	free(layer);
	layer_shell_rearrange(server);
}

static void layer_shell_handle_new_surface(
	struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *wlr_layer_surface = data;

	if (!wlr_layer_surface->output)
	{
		struct hsdwl_output *o;
		wl_list_for_each(o, &server->outputs, link)
		{
			wlr_layer_surface->output = o->wlr_output;
			break;
		}
		if (!wlr_layer_surface->output)
		{
			wlr_log(WLR_ERROR,
				"no output for layer surface");
			return;
		}
	}

	struct hsdwl_layer_surface *layer = calloc(1, sizeof(*layer));
	if (!layer)
		return;

	layer->server = server;
	layer->layer_surface = wlr_layer_surface;
	wlr_layer_surface->data = layer;
	wl_list_init(&layer->popups);
	wl_list_insert(&server->layer_surfaces, &layer->link);

	layer->destroy.notify = layer_surface_handle_destroy;
	wl_signal_add(&wlr_layer_surface->events.destroy,
		&layer->destroy);

	layer->map.notify = layer_surface_handle_map;
	wl_signal_add(&wlr_layer_surface->surface->events.map,
		&layer->map);

	layer->unmap.notify = layer_surface_handle_unmap;
	wl_signal_add(&wlr_layer_surface->surface->events.unmap,
		&layer->unmap);

	layer->commit.notify = layer_surface_handle_commit;
	wl_signal_add(&wlr_layer_surface->surface->events.commit,
		&layer->commit);

	layer->new_popup.notify = layer_surface_handle_new_popup;
	wl_signal_add(&wlr_layer_surface->events.new_popup,
		&layer->new_popup);
}

void layer_shell_rearrange(struct hsdwl_server *server)
{
	struct hsdwl_output *output;
	wl_list_for_each(output, &server->outputs, link)
	{
		struct wlr_box area;
		wlr_output_layout_get_box(server->output_layout,
			output->wlr_output, &area);

		for (uint32_t l = 0; l < 4; l++)
		{
			struct wlr_box layer_area = area;


			if ((l == 1 || l == 2) && server->config.stage_manager_enabled) {
				struct workspace_stage_mgr *mgr =
					&server->ws_stage_mgrs[server->current_workspace];
				if (!mgr->sidebar_hidden) {
					layer_area.x += SIDEBAR_WIDTH;
					layer_area.width -= SIDEBAR_WIDTH;
				}
			}

			struct hsdwl_layer_surface *layer;
			wl_list_for_each(layer,
				&server->layer_surfaces, link)
			{
				if (layer->layer_surface->output
						!= output->wlr_output)
					continue;
				if (layer->layer_surface->current.layer
						!= l)
					continue;

				configure_and_position(layer,
					&layer_area);
			}
		}

		output->work_area = area;
	}
}

bool layer_shell_init(struct hsdwl_server *server)
{
	server->layer_shell = wlr_layer_shell_v1_create(
		server->display, 4);
	if (!server->layer_shell)
	{
		wlr_log(WLR_ERROR,
			"wlr_layer_shell_v1_create failed");
		return false;
	}

	server->new_layer_surface.notify =
		layer_shell_handle_new_surface;
	wl_signal_add(&server->layer_shell->events.new_surface,
		&server->new_layer_surface);

	wl_list_init(&server->layer_surfaces);
	return true;
}
