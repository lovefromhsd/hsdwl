#define _GNU_SOURCE
#define WLR_USE_UNSTABLE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <stdarg.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

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

struct hsdwl_output
{
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wlr_output *wlr_output;
	struct hsdwl_server *server;
};

struct hsdwl_view
{
	struct hsdwl_server *server;
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	const char *app_id;
};

struct hsdwl_keyboard
{
	struct wl_list link;
	struct hsdwl_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->wlr_keyboard->modifiers);
}

static bool handle_keybinding(struct hsdwl_server *server, xkb_keysym_t sym)
{
	switch (sym)
	{
	case XKB_KEY_Escape:
		wl_display_terminate(server->display);
		return true;
	default:
		return false;
	}
}

static void keyboard_handle_key(struct wl_listener *listener, void *data)
{
	struct hsdwl_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct hsdwl_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_keyboard *wlr_keyboard = keyboard->wlr_keyboard;
	struct wlr_seat *seat = server->seat;

	uint32_t keycode = event->keycode + 8;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(wlr_keyboard->xkb_state, keycode);

	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
	{
		if (handle_keybinding(server, sym))
			return;
	}

	wlr_seat_set_keyboard(seat, wlr_keyboard);
	wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->link);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	free(keyboard);
}

static void server_new_keyboard(struct hsdwl_server *server, struct wlr_input_device *device)
{
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct hsdwl_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (!keyboard)
		return;

	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_rule_names rules = {0};
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context)
	{
		free(keyboard);
		return;
	}
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap)
	{
		xkb_context_unref(context);
		free(keyboard);
		return;
	}
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wl_list_insert(&server->keyboards, &keyboard->link);

	wlr_seat_set_keyboard(server->seat, wlr_keyboard);
	wlr_seat_set_capabilities(server->seat, WL_SEAT_CAPABILITY_KEYBOARD);
}

static void server_new_input(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type)
	{
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	default:
		break;
	}
}

static void output_handle_frame(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(output->server->scene, output->wlr_output);
	if (!scene_output)
		return;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	if (!wlr_scene_output_commit(scene_output, NULL))
		return;

	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->destroy.link);
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode)
		wlr_output_state_set_mode(&state, mode);

	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct hsdwl_output *output = calloc(1, sizeof(*output));
	if (!output)
		return;
	output->wlr_output = wlr_output;
	output->server = server;

	output->frame.notify = output_handle_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->destroy.notify = output_handle_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wlr_output_create_global(wlr_output, server->display);

	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	struct wlr_output_layout_output *layout_output = wlr_output_layout_add(server->output_layout, wlr_output, 0, 0);
	wlr_scene_output_layout_add_output(server->scene_layout, layout_output, scene_output);
}

static void view_handle_map(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, map);
	if (!view->scene_tree)
	{
		view->scene_tree = wlr_scene_xdg_surface_create(&view->server->scene->tree, view->xdg_surface);
		if (view->scene_tree)
		{
			wlr_scene_node_set_enabled(&view->scene_tree->node, true);
		}
	}

	if (view->xdg_surface->toplevel)
	{
		wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, true);
		wlr_xdg_surface_schedule_configure(view->xdg_surface);
	}

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(view->server->seat);
	if (kb)
	{
		wlr_seat_keyboard_notify_enter(view->server->seat, view->xdg_surface->surface, NULL, 0, NULL);
	}
}

static void view_handle_unmap(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, unmap);
	if (view->scene_tree)
		wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	if (view->xdg_surface->toplevel)
	{
		wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, false);
		wlr_xdg_surface_schedule_configure(view->xdg_surface);
	}
	wlr_seat_keyboard_clear_focus(view->server->seat);
}

static void view_handle_commit(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, commit);
	if (!view->xdg_surface)
		return;

	if (view->xdg_surface->initial_commit && view->xdg_surface->toplevel)
	{
		wlr_xdg_toplevel_set_fullscreen(view->xdg_surface->toplevel, true);
		wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, true);
		wlr_xdg_surface_schedule_configure(view->xdg_surface);
	}
}

static void view_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_view *view = wl_container_of(listener, view, destroy);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->destroy.link);
	free(view);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;
	struct wlr_xdg_surface *xdg_surface = toplevel->base;

	struct hsdwl_view *view = calloc(1, sizeof(*view));
	if (!view)
		return;

	view->server = server;
	view->xdg_surface = xdg_surface;
	xdg_surface->data = view;

	view->map.notify = view_handle_map;
	wl_signal_add(&xdg_surface->surface->events.map, &view->map);
	view->unmap.notify = view_handle_unmap;
	wl_signal_add(&xdg_surface->surface->events.unmap, &view->unmap);
	view->commit.notify = view_handle_commit;
	wl_signal_add(&xdg_surface->surface->events.commit, &view->commit);
	view->destroy.notify = view_handle_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);
}

static void spawn_client(struct hsdwl_server *server)
{
	pid_t pid = fork();
	if (pid < 0)
	{
		wlr_log(WLR_ERROR, "%s", "fork failed");
		return;
	}

	if (pid == 0)
	{
		setenv("WAYLAND_DISPLAY", server->socket, true);
		unsetenv("WAYLAND_SOCKET");
		execlp("foot", "foot", NULL);
		execlp("weston-terminal", "weston-terminal", NULL);
		wlr_log(WLR_ERROR, "%s", "execlp foot failed");
		_exit(1);
	}

	server->child_pid = pid;
}

static void handle_signal(int signo)
{
	if (signo == SIGCHLD)
	{
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
	}
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	wlr_log_init(WLR_INFO, NULL);

	struct hsdwl_server server = {0};
	wl_list_init(&server.keyboards);
	server.child_pid = -1;

	server.display = wl_display_create();
	if (!server.display)
		return 1;

	server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.display), NULL);
	if (!server.backend)
		return 1;

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer)
		return 1;

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (!server.allocator)
		return 1;

	server.scene = wlr_scene_create();
	if (!server.scene)
		return 1;

	server.output_layout = wlr_output_layout_create(server.display);
	if (!server.output_layout)
		return 1;

	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
	if (!server.scene_layout)
		return 1;

	wlr_compositor_create(server.display, 5, server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);
	wlr_renderer_init_wl_shm(server.renderer, server.display);

	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);

	struct wlr_xdg_shell *xdg_shell = wlr_xdg_shell_create(server.display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);

	server.seat = wlr_seat_create(server.display, "seat0");
	if (!server.seat)
		return 1;

	server.socket = wl_display_add_socket_auto(server.display);
	if (!server.socket)
	{
		wlr_log(WLR_ERROR, "%s", "failed to add wayland socket");
		return 1;
	}

	signal(SIGCHLD, handle_signal);

	if (!wlr_backend_start(server.backend))
	{
		wlr_log(WLR_ERROR, "%s", "failed to start backend");
		return 1;
	}

	spawn_client(&server);

	wlr_log(WLR_INFO, "%s", "hsdwl compositor running");

	wl_display_run(server.display);

	if (server.child_pid > 0)
	{
		kill(server.child_pid, SIGTERM);
		waitpid(server.child_pid, NULL, 0);
	}

	wl_display_destroy(server.display);
	return 0;
}
