#ifndef HSDWL_SERVER_H
#define HSDWL_SERVER_H

#define WLR_USE_UNSTABLE

#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <wayland-server-core.h>

#include "animation.h"
#include "config.h"
#include "stage-3d.h"
#include "stage.h"
#include "tab-group.h"

#define HSDWL_NUM_WORKSPACES 9

struct wlr_backend;
struct wlr_compositor;
struct wlr_renderer;
struct wlr_session;
struct wlr_allocator;
struct wlr_scene;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_output_layout;
struct wlr_output_layout;
struct wlr_seat;
struct wlr_cursor;
struct wlr_xcursor_manager;
struct wlr_output_manager_v1;
struct wlr_xdg_output_manager_v1;
struct wlr_xwayland;
struct wlr_layer_shell_v1;
struct wlr_pointer_constraints_v1;
struct wlr_relative_pointer_manager_v1;
struct wlr_pointer_constraint_v1;

struct hsdwl_view;
struct hsdwl_layer_surface;

enum hsdwl_cursor_mode
{
	HSDWL_CURSOR_PASSTHROUGH,
	HSDWL_CURSOR_MOVE,
	HSDWL_CURSOR_RESIZE,
	HSDWL_CURSOR_TAB_REORDER,
	HSDWL_CURSOR_STAGE_DRAG,
};

struct hsdwl_server
{
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_compositor *compositor;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct hsdwl_config config;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *output_layout;
	struct wlr_seat *seat;
	struct wl_listener new_output;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_shell_popup;
	struct wl_listener new_xwayland_surface;
	struct wl_listener new_input;
	struct wl_list keyboards;
	struct wl_list views;
	struct wl_list outputs;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener request_cursor;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_listener pointer_focus_change;
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;
	struct wl_listener output_manager_destroy;
	enum hsdwl_cursor_mode cursor_mode;
	struct hsdwl_view *grabbed_view;
	struct hsdwl_view *grab_target;
	double grab_x;
	double grab_y;
	int grab_view_x;
	int grab_view_y;
	uint32_t resize_edges;
	int grab_geom_width;
	int grab_geom_height;
	struct wlr_scene_tree *workspaces[HSDWL_NUM_WORKSPACES];
	struct hsdwl_view *focused_views[HSDWL_NUM_WORKSPACES];
	size_t current_workspace;
	struct wlr_xwayland *xwayland;
	struct wlr_output_manager_v1 *output_manager;
	struct wlr_xdg_output_manager_v1 *xdg_output_manager;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_scene_tree *layer_trees[4];
	struct wlr_scene_tree *override_tree;
	struct wl_listener new_layer_surface;
	struct wl_list layer_surfaces;
	struct hsdwl_layer_surface *focused_layer;
	const char *socket;
	struct wlr_scene_tree *drag_icon_tree;
	struct wl_listener drag_icon_destroy;
	struct wl_list tab_groups;
	struct wlr_scene_tree *preview_tree;
	struct wlr_scene_rect *resize_preview[4];
	int resize_preview_x;
	int resize_preview_y;
	int resize_preview_w;
	int resize_preview_h;


	struct workspace_stage_mgr ws_stage_mgrs[HSDWL_NUM_WORKSPACES];
	struct wlr_scene_tree *ws_sidebar_trees[HSDWL_NUM_WORKSPACES];
	struct wlr_scene_rect *ws_sidebar_bgs[HSDWL_NUM_WORKSPACES];
	struct wlr_scene_tree *ws_stage_canvases[HSDWL_NUM_WORKSPACES];
	struct custom_stage *drag_source_stage;


	struct wlr_pointer_constraints_v1 *pointer_constraints;
	struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
	struct wlr_pointer_constraint_v1 *active_constraint;
	struct wl_listener new_constraint;
	struct wl_listener constraint_destroy;
	struct wl_listener constraint_set_region;
	/* Last absolute pointer position, for computing relative
	 * deltas when pointer is locked with absolute input devices */
	double last_abs_x;
	double last_abs_y;
	bool last_abs_valid;


	struct wl_list animations;
	struct wl_list tilt_animations;
	struct wlr_scene_tree *animation_tree;
};

bool hsdwl_server_init(struct hsdwl_server *server);
void hsdwl_server_destroy(struct hsdwl_server *server);
int hsdwl_server_run(struct hsdwl_server *server);
void hsdwl_server_switch_workspace(struct hsdwl_server *server, size_t ws);
void hsdwl_server_move_to_workspace(struct hsdwl_server *server,
		struct hsdwl_view *view, size_t ws);

#endif
