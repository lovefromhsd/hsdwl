#define _GNU_SOURCE

#include "server.h"
#include "input.h"
#include "layer-shell.h"
#include "output.h"
#include "output-management.h"
#include "pointer.h"
#include "seat.h"
#include "stage.h"
#include "tab-group.h"
#include "view.h"
#include "xwayland.h"

#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

void decoration_handle_request_mode(
		struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	wlr_xdg_toplevel_decoration_v1_set_mode(deco,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void decoration_handle_destroy(
		struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(
		listener, view, decoration_destroy);
	wl_list_remove(&view->decoration_destroy.link);
	if (view->decoration_request_mode.notify)
		wl_list_remove(&view->decoration_request_mode.link);
	wl_list_init(&view->decoration_destroy.link);
	wl_list_init(&view->decoration_request_mode.link);
	view->decoration = NULL;
}

static void handle_new_toplevel_decoration(
		struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	struct hsdwl_view *view = deco->toplevel->base->data;
	if (!view)
		return;
	view->decoration = deco;
	view->decoration_destroy.notify = decoration_handle_destroy;
	wl_signal_add(&deco->events.destroy,
		&view->decoration_destroy);
}

void hsdwl_server_switch_workspace(struct hsdwl_server *server, size_t ws)
{
	if (ws >= HSDWL_NUM_WORKSPACES
			|| ws == server->current_workspace)
		return;

	struct wlr_surface *focused_surface =
		server->seat->keyboard_state.focused_surface;
	struct hsdwl_view *v;
	wl_list_for_each(v, &server->views, link)
	{
		if (view_get_surface(v) == focused_surface)
		{
			server->focused_views[server->current_workspace] = v;
			break;
		}
	}

	for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
		wlr_scene_node_set_enabled(
			&server->workspaces[i]->node, i == ws);
	server->current_workspace = ws;

	struct hsdwl_view *next = server->focused_views[ws];
	if (next)
	{
		bool still_valid = false;
		wl_list_for_each(v, &server->views, link)
		{
			if (v == next) { still_valid = true; break; }
		}
		if (!still_valid)
			next = NULL;
	}
	if (next && (!next->scene_tree
			|| !view_is_on_workspace(next,
				server->workspaces[ws])))
		next = NULL;

	if (!next)
	{
		wl_list_for_each(v, &server->views, link)
		{
			if (!v->scene_tree)
				continue;
			bool xdg_usable = v->xdg_surface
				&& v->xdg_surface->configured;
			bool xwayland_usable = v->xwayland_surface
				&& v->xwayland_surface->surface
				&& (!v->xwayland_surface->override_redirect
					|| wlr_xwayland_surface_override_redirect_wants_focus(
						v->xwayland_surface));
			if (!xdg_usable && !xwayland_usable)
				continue;
			if (!view_is_on_workspace(v,
					server->workspaces[ws]))
				continue;
			next = v;
			break;
		}
	}
	view_focus(server, next);
}

void hsdwl_server_move_to_workspace(struct hsdwl_server *server,
		struct hsdwl_view *view, size_t ws)
{
	if (ws >= HSDWL_NUM_WORKSPACES || !view
			|| !view->scene_tree)
		return;

	wlr_scene_node_reparent(&view->scene_tree->node,
		server->workspaces[ws]);
	hsdwl_server_switch_workspace(server, ws);
}

static int sig[2];
static struct wl_event_loop *event_loop;

static void handle_signal(int signo)
{
	(void)signo;
	if (write(sig[1], "", 1) < 0)
	{
	}
}

static int signal_event(int fd, uint32_t mask, void *data)
{
	(void)mask;
	(void)data;
	char buf[16];
	if (read(fd, buf, sizeof(buf)) < 0)
	{
	}
	struct hsdwl_server *server = data;
	wl_display_terminate(server->display);
	return 1;
}

bool hsdwl_server_init(struct hsdwl_server *server)
{
	hsdwl_config_load(&server->config);

	server->display = wl_display_create();
	if (!server->display)
	{
		wlr_log(WLR_ERROR, "wl_display_create failed");
		return false;
	}

	server->backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server->display), NULL);
	if (!server->backend)
	{
		wlr_log(WLR_ERROR, "wlr_backend_autocreate failed");
		return false;
	}

	server->renderer = wlr_renderer_autocreate(server->backend);
	if (!server->renderer)
	{
		wlr_log(WLR_ERROR, "wlr_renderer_autocreate failed");
		return false;
	}
	wlr_renderer_init_wl_display(server->renderer, server->display);

	server->allocator = wlr_allocator_autocreate(
		server->backend, server->renderer);
	if (!server->allocator)
	{
		wlr_log(WLR_ERROR, "wlr_allocator_autocreate failed");
		return false;
	}

	server->compositor = wlr_compositor_create(
		server->display, 6, server->renderer);
	wlr_subcompositor_create(server->display);
	wlr_data_device_manager_create(server->display);

	server->scene = wlr_scene_create();
	if (!server->scene)
	{
		wlr_log(WLR_ERROR, "wlr_scene_create failed");
		return false;
	}

	server->output_layout = wlr_output_layout_create(server->display);
	server->scene_layout = wlr_scene_attach_output_layout(
		server->scene, server->output_layout);

	wl_list_init(&server->keyboards);
	wl_list_init(&server->views);
	wl_list_init(&server->outputs);
	wl_list_init(&server->animations);
	hsdwl_tab_group_init(server);

	server->animation_tree = wlr_scene_tree_create(
		&server->scene->tree);
	if (!server->animation_tree)
	{
		wlr_log(WLR_ERROR, "animation_tree create failed");
		return false;
	}
	wlr_scene_node_raise_to_top(
		&server->animation_tree->node);

	if (!output_manager_init(server))
	{
		wlr_log(WLR_ERROR, "output_manager_init failed");
		return false;
	}

	server->xdg_output_manager =
		wlr_xdg_output_manager_v1_create(
			server->display, server->output_layout);
	if (!server->xdg_output_manager)
	{
		wlr_log(WLR_ERROR,
			"wlr_xdg_output_manager_v1_create failed");
		return false;
	}

	server->layer_trees[0] = wlr_scene_tree_create(
		&server->scene->tree);
	server->layer_trees[1] = wlr_scene_tree_create(
		&server->scene->tree);

	for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
	{
		server->workspaces[i] = wlr_scene_tree_create(
			&server->scene->tree);
		if (!server->workspaces[i])
		{
			wlr_log(WLR_ERROR, "wlr_scene_tree_create failed");
			return false;
		}
		wlr_scene_node_set_enabled(
			&server->workspaces[i]->node, false);
		server->focused_views[i] = NULL;
	}
	server->current_workspace = 0;
	wlr_scene_node_set_enabled(
		&server->workspaces[0]->node, true);

	server->override_tree = wlr_scene_tree_create(
		&server->scene->tree);
	if (!server->override_tree)
	{
		wlr_log(WLR_ERROR, "wlr_scene_tree_create failed");
		return false;
	}

	server->layer_trees[2] = wlr_scene_tree_create(
		&server->scene->tree);
	server->layer_trees[3] = wlr_scene_tree_create(
		&server->scene->tree);

	server->cursor = wlr_cursor_create();
	if (!server->cursor)
	{
		wlr_log(WLR_ERROR, "wlr_cursor_create failed");
		return false;
	}
	wlr_cursor_attach_output_layout(server->cursor,
		server->output_layout);

	server->cursor_mgr = wlr_xcursor_manager_create(NULL,
		server->config.cursor_size);
	if (!server->cursor_mgr)
	{
		wlr_log(WLR_ERROR, "wlr_xcursor_manager_create failed");
		return false;
	}
	wlr_xcursor_manager_load(server->cursor_mgr, 1);

	server->new_output.notify = output_handle_new;
	wl_signal_add(&server->backend->events.new_output,
		&server->new_output);
	server->new_xdg_toplevel.notify = view_handle_new_xdg_toplevel;

	struct wlr_xdg_shell *xdg_shell = wlr_xdg_shell_create(
		server->display, 5);
	if (!xdg_shell)
	{
		wlr_log(WLR_ERROR, "wlr_xdg_shell_create failed");
		return false;
	}
	wl_signal_add(&xdg_shell->events.new_toplevel,
		&server->new_xdg_toplevel);

	server->new_input.notify = input_handle_new;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);

	server->seat = wlr_seat_create(server->display, "seat0");
	if (!server->seat)
	{
		wlr_log(WLR_ERROR, "wlr_seat_create failed");
		return false;
	}

	server->cursor_mode = HSDWL_CURSOR_PASSTHROUGH;
	server->grabbed_view = NULL;
	server->grab_target = NULL;

	if (!pointer_init(server))
	{
		wlr_log(WLR_ERROR, "pointer_init failed");
		return false;
	}

	if (!seat_init(server))
	{
		wlr_log(WLR_ERROR, "seat_init failed");
		return false;
	}

	if (!wlr_ext_data_control_manager_v1_create(server->display, 1))
	{
		wlr_log(WLR_ERROR,
			"wlr_ext_data_control_manager_v1_create failed");
		return false;
	}

	if (!wlr_screencopy_manager_v1_create(server->display))
	{
		wlr_log(WLR_ERROR,
			"wlr_screencopy_manager_v1_create failed");
		return false;
	}

	if (!wlr_ext_output_image_capture_source_manager_v1_create(
			server->display, 1))
	{
		wlr_log(WLR_ERROR,
			"wlr_ext_output_image_capture_source_"
			"manager_v1_create failed");
		return false;
	}

	if (!wlr_ext_image_copy_capture_manager_v1_create(
			server->display, 1))
	{
		wlr_log(WLR_ERROR,
			"wlr_ext_image_copy_capture_manager_v1_create failed");
		return false;
	}

	struct wlr_xdg_decoration_manager_v1 *deco_mgr =
		wlr_xdg_decoration_manager_v1_create(server->display);
	if (!deco_mgr)
	{
		wlr_log(WLR_ERROR,
			"wlr_xdg_decoration_manager_v1_create failed");
		return false;
	}

	static struct wl_listener deco_listener;
	deco_listener.notify = handle_new_toplevel_decoration;
	wl_signal_add(&deco_mgr->events.new_toplevel_decoration,
		&deco_listener);

	if (!hsdwl_xwayland_init(server))
	{
		wlr_log(WLR_ERROR, "hsdwl_xwayland_init failed");
		return false;
	}

	wl_list_init(&server->layer_surfaces);
	server->focused_layer = NULL;

	if (!layer_shell_init(server))
	{
		wlr_log(WLR_ERROR, "layer_shell_init failed");
		return false;
	}

	stage_manager_init(server);

	return true;
}

void hsdwl_server_destroy(struct hsdwl_server *server)
{
	wl_list_remove(&server->cursor_motion.link);
	wl_list_remove(&server->cursor_motion_absolute.link);
	wl_list_remove(&server->cursor_button.link);
	wl_list_remove(&server->cursor_axis.link);
	wl_list_remove(&server->cursor_frame.link);
	wl_list_remove(&server->request_cursor.link);
	wl_list_remove(&server->pointer_focus_change.link);
	wl_list_remove(&server->request_set_selection.link);
	wl_list_remove(&server->request_set_primary_selection.link);
	wl_list_remove(&server->request_start_drag.link);
	wl_list_remove(&server->start_drag.link);
	if (server->drag_icon_destroy.notify)
		wl_list_remove(&server->drag_icon_destroy.link);
	wlr_xcursor_manager_destroy(server->cursor_mgr);
	wlr_cursor_destroy(server->cursor);
	if (server->preview_tree)
		wlr_scene_node_destroy(&server->preview_tree->node);
	animation_cancel_all(server);
	if (server->animation_tree)
		wlr_scene_node_destroy(&server->animation_tree->node);
	stage_manager_destroy(server);
	hsdwl_tab_group_finish(server);
	hsdwl_xwayland_finish(server);
	output_manager_finish(server);
	wl_list_remove(&server->new_layer_surface.link);
	hsdwl_config_finish(&server->config);
	wl_display_destroy(server->display);
}

int hsdwl_server_run(struct hsdwl_server *server)
{
	server->socket = wl_display_add_socket_auto(server->display);
	if (!server->socket)
	{
		wlr_log(WLR_ERROR, "wl_display_add_socket_auto failed");
		return 1;
	}

	if (pipe(sig) < 0)
	{
		wlr_log(WLR_ERROR, "pipe failed");
		return 1;
	}

	if (!wlr_backend_start(server->backend))
	{
		wlr_log(WLR_ERROR, "wlr_backend_start failed");
		return 1;
	}

	event_loop = wl_display_get_event_loop(server->display);
	struct wl_event_source *sig_source = wl_event_loop_add_fd(
		event_loop, sig[0], WL_EVENT_READABLE, signal_event, server);
	if (!sig_source)
	{
		wlr_log(WLR_ERROR, "wl_event_loop_add_fd failed");
		return 1;
	}

	struct sigaction sa = {0};
	sa.sa_handler = handle_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	wlr_log(WLR_INFO, "running on wayland display: %s", server->socket);
	setenv("WAYLAND_DISPLAY", server->socket, true);

	char autostart_path[1024];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && xdg[0])
		snprintf(autostart_path, sizeof(autostart_path),
			"%s/hsdwl/autostart.sh", xdg);
	else
	{
		const char *home = getenv("HOME");
		if (!home)
		{
			struct passwd *pw = getpwuid(getuid());
			if (!pw)
				home = "/";
			else
				home = pw->pw_dir;
		}
		snprintf(autostart_path, sizeof(autostart_path),
			"%s/.config/hsdwl/autostart.sh", home);
	}

	if (access(autostart_path, F_OK) != 0)
	{
		FILE *f = fopen(autostart_path, "w");
		if (f)
		{
			fprintf(f, "#!/bin/sh\n");
			fclose(f);
			chmod(autostart_path, 0700);
		}
	}

	if (fork() == 0)
	{
		execl("/bin/sh", "sh", autostart_path, NULL);
		_exit(EXIT_FAILURE);
	}

	wl_display_run(server->display);

	close(sig[0]);
	close(sig[1]);

	return 0;
}
