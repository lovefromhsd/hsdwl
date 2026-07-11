#ifndef HSDWL_STAGE_H
#define HSDWL_STAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct hsdwl_server;
struct hsdwl_view;
struct wlr_buffer;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_buffer;

#define SIDEBAR_WIDTH 200
#define STAGE_THUMB_PAD 10
#define STAGE_THUMB_GAP 6


struct custom_window
{
	struct wl_list link;
	struct hsdwl_view *view;
	double x, y, w, h;
};

struct custom_stage;

struct custom_stage
{
	struct wl_list link;           
	struct wl_list windows;        
	struct wlr_scene_tree *tree;   
	struct wlr_scene_tree *thumb_tree;  
	struct wlr_scene_buffer *thumb_buf;
	bool thumb_dirty;
	int thumb_x, thumb_y;
	int thumb_w, thumb_h;
	float z_offset;
};

struct workspace_stage_mgr
{
	struct custom_stage *active_stage;
	struct wl_list inactive_stages;  
	bool sidebar_hidden;
};


void stage_set_views_enabled(struct custom_stage *stage, bool enabled);
void stage_reparent_to_canvas(struct custom_stage *stage,
		struct hsdwl_server *server);
struct custom_window *find_custom_window(struct custom_stage *stage,
		struct hsdwl_view *view);
void stage_free(struct custom_stage *stage);

void stage_manager_init(struct hsdwl_server *server);
void stage_manager_destroy(struct hsdwl_server *server);

void stage_manager_new_window(struct hsdwl_server *server,
		struct hsdwl_view *view, bool animate);

bool stage_manager_remove_view(struct hsdwl_server *server,
		struct hsdwl_view *view, size_t ws);

void stage_manager_notify_view_removed(struct hsdwl_server *server,
		struct hsdwl_view *view);

struct custom_stage *stage_at(struct hsdwl_server *server,
		double lx, double ly, size_t ws);

void stage_manager_switch(struct hsdwl_server *server,
		struct custom_stage *target, size_t ws);

void stage_manager_cycle(struct hsdwl_server *server,
		size_t ws, bool reverse);

void stage_manager_merge(struct hsdwl_server *server,
		struct custom_stage *source, size_t ws);

void stage_manager_render_sidebar(struct hsdwl_server *server, size_t ws);

void stage_manager_migrate_existing(struct hsdwl_server *server);

void stage_manager_notify_surface_commit(struct hsdwl_server *server,
		struct hsdwl_view *view);

void stage_manager_check_sidebar_overlap(struct hsdwl_server *server,
		size_t ws);

int stage_manager_window_count(struct hsdwl_server *server, size_t ws);

#endif
