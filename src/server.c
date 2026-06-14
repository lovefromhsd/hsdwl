#define _GNU_SOURCE

#include "server.h"
#include "input.h"
#include "output.h"
#include "pointer.h"
#include "view.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
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
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

int hsdwl_server_spawn_client(struct hsdwl_server *server)
{
	pid_t pid = fork();
	if (pid < 0)
	{
		wlr_log(WLR_ERROR, "fork failed: %s", strerror(errno));
		return -1;
	}
	if (pid > 0)
	{
		server->child_pid = pid;
		return 0;
	}

	int dev_null = open("/dev/null", O_RDWR);
	if (dev_null < 0)
		_exit(EXIT_FAILURE);
	dup2(dev_null, STDIN_FILENO);
	dup2(dev_null, STDOUT_FILENO);
	dup2(dev_null, STDERR_FILENO);
	close(dev_null);

	const char *cmd[] = {"foot", NULL};
	execvp(cmd[0], (char *const *)cmd);
	_exit(EXIT_FAILURE);
}

void hsdwl_server_switch_workspace(struct hsdwl_server *server, size_t ws)
{
	if (ws >= HSDWL_NUM_WORKSPACES
			|| ws == server->current_workspace)
		return;

	for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
		wlr_scene_node_set_enabled(
			&server->workspaces[i]->node, i == ws);
	server->current_workspace = ws;

	struct hsdwl_view *v;
	struct hsdwl_view *next = NULL;
	wl_list_for_each(v, &server->views, link)
	{
		if (!v->scene_tree || !v->xdg_surface
				|| !v->xdg_surface->configured)
			continue;
		if (v->scene_tree->node.parent
				!= server->workspaces[ws])
			continue;
		next = v;
		break;
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
	if (server->child_pid > 0)
	{
		kill(server->child_pid, SIGTERM);
		int status;
		waitpid(server->child_pid, &status, 0);
		server->child_pid = 0;
	}
	return 1;
}

bool hsdwl_server_init(struct hsdwl_server *server)
{
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

	wlr_compositor_create(server->display, 6, server->renderer);
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
	}
	server->current_workspace = 0;
	wlr_scene_node_set_enabled(
		&server->workspaces[0]->node, true);

	server->cursor = wlr_cursor_create();
	if (!server->cursor)
	{
		wlr_log(WLR_ERROR, "wlr_cursor_create failed");
		return false;
	}
	wlr_cursor_attach_output_layout(server->cursor,
		server->output_layout);

	server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
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

	wl_list_init(&server->keyboards);
	wl_list_init(&server->views);
	server->cursor_mode = HSDWL_CURSOR_PASSTHROUGH;
	server->grabbed_view = NULL;

	if (!pointer_init(server))
	{
		wlr_log(WLR_ERROR, "pointer_init failed");
		return false;
	}

	return true;
}

void hsdwl_server_destroy(struct hsdwl_server *server)
{
	if (server->child_pid > 0)
	{
		int status;
		waitpid(server->child_pid, &status, 0);
		server->child_pid = 0;
	}
	wl_list_remove(&server->cursor_motion.link);
	wl_list_remove(&server->cursor_motion_absolute.link);
	wl_list_remove(&server->cursor_button.link);
	wl_list_remove(&server->cursor_axis.link);
	wl_list_remove(&server->cursor_frame.link);
	wl_list_remove(&server->request_cursor.link);
	wl_list_remove(&server->pointer_focus_change.link);
	wl_list_remove(&server->request_set_selection.link);
	wlr_xcursor_manager_destroy(server->cursor_mgr);
	wlr_cursor_destroy(server->cursor);
	wlr_xwayland_destroy(NULL);
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

	hsdwl_server_spawn_client(server);

	wl_display_run(server->display);

	close(sig[0]);
	close(sig[1]);

	return 0;
}
