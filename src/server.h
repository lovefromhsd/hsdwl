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
	const char *socket;
	pid_t child_pid;
};

bool hsdwl_server_init(struct hsdwl_server *server);
void hsdwl_server_destroy(struct hsdwl_server *server);
int hsdwl_server_run(struct hsdwl_server *server);

#endif
