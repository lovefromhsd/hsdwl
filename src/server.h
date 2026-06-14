#ifndef HSDWL_SERVER_H
#define HSDWL_SERVER_H

#define WLR_USE_UNSTABLE

#include <stdbool.h>
#include <signal.h>
#include <wayland-server-core.h>

struct wlr_backend;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_scene;
struct wlr_scene_output_layout;
struct wlr_output_layout;
struct wlr_seat;
struct wlr_cursor;
struct wlr_xcursor_manager;

struct hsdwl_view;

enum hsdwl_cursor_mode
{
	HSDWL_CURSOR_PASSTHROUGH,
	HSDWL_CURSOR_MOVE,
};

struct hsdwl_server
{
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *output_layout;
	struct wlr_seat *seat;
	struct wl_listener new_output;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_input;
	struct wl_list keyboards;
	struct wl_list views;
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
	enum hsdwl_cursor_mode cursor_mode;
	struct hsdwl_view *grabbed_view;
	double grab_x;
	double grab_y;
	int grab_view_x;
	int grab_view_y;
	const char *socket;
	pid_t child_pid;
};

bool hsdwl_server_init(struct hsdwl_server *server);
void hsdwl_server_destroy(struct hsdwl_server *server);
int hsdwl_server_run(struct hsdwl_server *server);
int hsdwl_server_spawn_client(struct hsdwl_server *server);

#endif
