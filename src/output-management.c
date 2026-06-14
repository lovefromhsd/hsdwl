#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "output-management.h"
#include "output.h"
#include "server.h"

#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/util/log.h>

static void handle_apply(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, output_manager_apply);
	struct wlr_output_configuration_v1 *config = data;

	struct wlr_output_configuration_head_v1 *head;
	bool ok = true;

	wl_list_for_each(head, &config->heads, link)
	{
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_head_v1_state_apply(&head->state, &state);

		if (!wlr_output_test_state(head->state.output, &state))
		{
			wlr_log(WLR_DEBUG,
				"output test failed for %s",
				head->state.output->name);
			wlr_output_state_finish(&state);
			ok = false;
			break;
		}

		wlr_output_state_finish(&state);
	}

	wl_list_for_each(head, &config->heads, link)
	{
		if (!ok)
			break;

		wlr_output_layout_add(server->output_layout,
			head->state.output, head->state.x, head->state.y);

		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_head_v1_state_apply(&head->state, &state);

		if (!wlr_output_commit_state(head->state.output, &state))
		{
			wlr_log(WLR_ERROR,
				"output commit failed for %s",
				head->state.output->name);
			wlr_output_state_finish(&state);
			ok = false;
			break;
		}

		wlr_output_state_finish(&state);
	}

	if (ok)
	{
		wlr_output_configuration_v1_send_succeeded(config);
		output_manager_update(server);
	}
	else
	{
		wlr_output_configuration_v1_send_failed(config);
	}

	wlr_output_configuration_v1_destroy(config);
}

static void handle_test(struct wl_listener *listener, void *data)
{
	struct hsdwl_server *server = wl_container_of(
		listener, server, output_manager_test);
	struct wlr_output_configuration_v1 *config = data;

	struct wlr_output_configuration_head_v1 *head;
	bool ok = true;

	wl_list_for_each(head, &config->heads, link)
	{
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_head_v1_state_apply(&head->state, &state);

		if (!wlr_output_test_state(head->state.output, &state))
		{
			wlr_output_state_finish(&state);
			ok = false;
			break;
		}

		wlr_output_state_finish(&state);
	}

	if (ok)
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);

	wlr_output_configuration_v1_destroy(config);
}

static void handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct hsdwl_server *server = wl_container_of(
		listener, server, output_manager_destroy);
	wl_list_remove(&server->output_manager_apply.link);
	wl_list_remove(&server->output_manager_test.link);
	wl_list_remove(&server->output_manager_destroy.link);
	server->output_manager = NULL;
}

bool output_manager_init(struct hsdwl_server *server)
{
	server->output_manager = wlr_output_manager_v1_create(
		server->display);
	if (!server->output_manager)
	{
		wlr_log(WLR_ERROR,
			"wlr_output_manager_v1_create failed");
		return false;
	}

	server->output_manager_apply.notify = handle_apply;
	wl_signal_add(&server->output_manager->events.apply,
		&server->output_manager_apply);
	server->output_manager_test.notify = handle_test;
	wl_signal_add(&server->output_manager->events.test,
		&server->output_manager_test);
	server->output_manager_destroy.notify = handle_destroy;
	wl_signal_add(&server->output_manager->events.destroy,
		&server->output_manager_destroy);

	output_manager_update(server);

	return true;
}

void output_manager_finish(struct hsdwl_server *server)
{
	if (!server->output_manager)
		return;
	wl_list_remove(&server->output_manager_apply.link);
	wl_list_remove(&server->output_manager_test.link);
	wl_list_remove(&server->output_manager_destroy.link);
	wlr_output_manager_v1_set_configuration(
		server->output_manager, NULL);
	server->output_manager = NULL;
}

void output_manager_update(struct hsdwl_server *server)
{
	if (!server->output_manager)
		return;

	struct wlr_output_configuration_v1 *config =
		wlr_output_configuration_v1_create();

	struct hsdwl_output *output;
	wl_list_for_each(output, &server->outputs, link)
	{
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(
				config, output->wlr_output);

		struct wlr_output_layout_output *lo =
			wlr_output_layout_get(
				server->output_layout,
				output->wlr_output);
		if (lo)
		{
			head->state.x = lo->x;
			head->state.y = lo->y;
		}
	}

	wlr_output_manager_v1_set_configuration(
		server->output_manager, config);
}
