#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "stage-util.h"
#include "stage.h"
#include "server.h"
#include "output.h"
#include "view.h"
#include "tab-group.h"

#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>


void stage_focus_first(struct custom_stage *stage, struct hsdwl_server *server) {
	struct custom_window *cw;
	wl_list_for_each(cw, &stage->windows, link) {
		view_focus(server, cw->view);
		break;
	}
}


bool stage_compute_bbox(struct custom_stage *stage, struct wlr_box *out_bbox) {
	struct wlr_box bbox = {0};
	bool first = true;
	struct custom_window *cw;
	wl_list_for_each(cw, &stage->windows, link) {
		if (first) {
			bbox.x = cw->x; bbox.y = cw->y;
			bbox.width = cw->w; bbox.height = cw->h;
			first = false;
		} else {
			double x1 = fmin(bbox.x, cw->x);
			double y1 = fmin(bbox.y, cw->y);
			double x2 = fmax(bbox.x + bbox.width, cw->x + cw->w);
			double y2 = fmax(bbox.y + bbox.height, cw->y + cw->h);
			bbox.x = x1; bbox.y = y1;
			bbox.width = x2 - x1; bbox.height = y2 - y1;
		}
	}
	*out_bbox = bbox;
	return !first && bbox.width >= 1 && bbox.height >= 1;
}


bool stage_thumb_init(struct custom_stage *stage, struct hsdwl_server *server, size_t ws) {
	stage->thumb_tree = wlr_scene_tree_create(server->ws_sidebar_trees[ws]);
	if (!stage->thumb_tree)
		return false;
	stage->thumb_tree->node.data = stage;
	stage->thumb_buf = wlr_scene_buffer_create(stage->thumb_tree, NULL);
	if (!stage->thumb_buf)
		wlr_log(WLR_ERROR, "thumb_buf create failed");
	return true;
}


struct wlr_scene_node *stage_window_node(struct custom_window *cw) {
	if (cw->view && cw->view->tab_group && cw->view->tab_group->scene_tree)
		return &cw->view->tab_group->scene_tree->node;
	else if (cw->view && cw->view->scene_tree)
		return &cw->view->scene_tree->node;
	return NULL;
}


int output_get_width(struct hsdwl_server *server) {
	if (!wl_list_empty(&server->outputs)) {
		struct hsdwl_output *o = wl_container_of(server->outputs.next, o, link);
		if (o->wlr_output) return o->wlr_output->width;
	}
	return 1920;
}


int output_get_height(struct hsdwl_server *server) {
	if (!wl_list_empty(&server->outputs)) {
		struct hsdwl_output *o = wl_container_of(server->outputs.next, o, link);
		if (o->wlr_output) return o->wlr_output->height;
	}
	return 1080;
}
