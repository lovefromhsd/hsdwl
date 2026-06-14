#ifndef HSDWL_SERVER_H
#define HSDWL_SERVER_H

#define WLR_USE_UNSTABLE

#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <wayland-server-core.h>

#include "config.h"

#define HSDWL_NUM_WORKSPACES 9

struct wlr_backend;
struct wlr_compositor;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_scene;
struct wlr_scene_tree;
struct wlr_scene_output_layout;
struct wlr_output_layout;
struct wlr_seat;
struct wlr_cursor;
struct wlr_xcursor_manager;
struct wlr_output_manager_v1;
struct wlr_xwayland;
struct wlr_layer_shell_v1;

struct hsdwl_view;
struct hsdwl_layer_surface;

enum hsdwl_cursor_mode
{
	HSDWL_CURSOR_PASSTHROUGH,
	HSDWL_CURSOR_MOVE,
	HSDWL_CURSOR_RESIZE,
};

struct hsdwl_server
{
	struct wl_display *display;
	struct wlr_backend *backend;
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
	struct wl_listener request_set_selection;
	struct wl_listener pointer_focus_change;
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;
	struct wl_listener output_manager_destroy;
	enum hsdwl_cursor_mode cursor_mode;
	struct hsdwl_view *grabbed_view;
	double grab_x;
	double grab_y;
	int grab_view_x;
	int grab_view_y;
	uint32_t resize_edges;
	int grab_geom_width;
	int grab_geom_height;
	struct wlr_scene_tree *workspaces[HSDWL_NUM_WORKSPACES];
	size_t current_workspace;
	struct wlr_xwayland *xwayland;
	struct wlr_output_manager_v1 *output_manager;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_scene_tree *layer_trees[4];
	struct wl_listener new_layer_surface;
	struct wl_list layer_surfaces;
	struct hsdwl_layer_surface *focused_layer;
	const char *socket;
};

bool hsdwl_server_init(struct hsdwl_server *server);
void hsdwl_server_destroy(struct hsdwl_server *server);
int hsdwl_server_run(struct hsdwl_server *server);
void hsdwl_server_switch_workspace(struct hsdwl_server *server, size_t ws);
void hsdwl_server_move_to_workspace(struct hsdwl_server *server,
		struct hsdwl_view *view, size_t ws);

#endif
