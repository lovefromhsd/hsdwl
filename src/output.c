#define _GNU_SOURCE

#include "animation.h"
#include "layer-shell.h"
#include "output.h"
#include "output-management.h"
#include "server.h"
#include "stage.h"

#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

static void output_handle_frame(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		output->server->scene, output->wlr_output);
	if (!scene_output)
		return;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	animation_tick(output->server, &now);

	stage_manager_check_sidebar_overlap(output->server,
		output->server->current_workspace);

	if (!wl_list_empty(&output->server->animations))
		wlr_output_schedule_frame(output->wlr_output);

	if (!wlr_scene_output_commit(scene_output, NULL))
		return;

	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->link);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->destroy.link);
	layer_shell_rearrange(output->server);
	free(output);
}

void output_handle_new(struct wl_listener *listener, void *data)
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
	wl_list_insert(&server->outputs, &output->link);

	output->frame.notify = output_handle_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->destroy.notify = output_handle_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wlr_output_create_global(wlr_output, server->display);

	struct wlr_scene_output *scene_output = wlr_scene_output_create(
		server->scene, wlr_output);
	struct wlr_output_layout_output *layout_output = wlr_output_layout_add(
		server->output_layout, wlr_output, 0, 0);
	wlr_scene_output_layout_add_output(server->scene_layout,
		layout_output, scene_output);

	output_manager_update(server);
	layer_shell_rearrange(server);
}
