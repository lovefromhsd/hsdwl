#ifndef HSDWL_STAGE_H
#define HSDWL_STAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct hsdwl_server;
struct hsdwl_view;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_buffer;

#define SIDEBAR_WIDTH 200
#define MAX_INACTIVE_STAGES 5
#define STAGE_THUMB_PAD 10
#define STAGE_THUMB_GAP 6

struct custom_window
{
	struct wl_list link;
	struct hsdwl_view *view;
	double x, y, w, h;
};

struct custom_stage
{
	struct wl_list link;           /* into workspace_stage_mgr->inactive_stages */
	struct wl_list windows;        /* of custom_window */
	struct wlr_scene_tree *tree;   /* container for windows, child of stage_canvas_tree */
	struct wlr_scene_tree *thumb_tree;  /* node in sidebar for this stage */
	struct wlr_scene_buffer *thumb_buf;
	struct wlr_scene_rect *thumb_bg;
	bool thumb_dirty;
};

struct workspace_stage_mgr
{
	struct custom_stage *active_stage;
	struct wl_list inactive_stages;  /* of custom_stage, max MAX_INACTIVE_STAGES */
};

void stage_manager_init(struct hsdwl_server *server);
void stage_manager_destroy(struct hsdwl_server *server);

void stage_manager_new_window(struct hsdwl_server *server,
		struct hsdwl_view *view);

void stage_manager_notify_view_removed(struct hsdwl_server *server,
		struct hsdwl_view *view);

struct custom_stage *stage_at(struct hsdwl_server *server,
		double lx, double ly, size_t ws);

void stage_manager_switch(struct hsdwl_server *server,
		struct custom_stage *target, size_t ws);

void stage_manager_merge(struct hsdwl_server *server,
		struct custom_stage *source, size_t ws);

void stage_manager_render_sidebar(struct hsdwl_server *server, size_t ws);

void stage_manager_migrate_existing(struct hsdwl_server *server);

void stage_manager_notify_surface_commit(struct hsdwl_server *server,
		struct hsdwl_view *view);

#endif
