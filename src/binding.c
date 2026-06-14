#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <xkbcommon/xkbcommon.h>
#include "binding.h"
#include "config.h"
#include "server.h"
#include "view.h"

static bool mods_match(struct hsdwl_binding *b, struct wlr_keyboard *kb)
{
	char mods_copy[128];
	snprintf(mods_copy, sizeof(mods_copy), "%s", b->mods);
	struct xkb_state *state = kb->xkb_state;

	if (mods_copy[0] == '\0') return false;

	char *save = NULL;
	char *tok = strtok_r(mods_copy, "+", &save);
	while (tok) {
		while (*tok == ' ') tok++;
		if (*tok == '\0') { tok = strtok_r(NULL, "+", &save); continue; }

		bool active = xkb_state_mod_name_is_active(state, tok,
			XKB_STATE_MODS_EFFECTIVE);
		if (!active) return false;
		tok = strtok_r(NULL, "+", &save);
	}
	return true;
}

static struct hsdwl_view *focused_view(struct hsdwl_server *server)
{
	struct wlr_surface *focused =
		server->seat->keyboard_state.focused_surface;
	if (!focused) return NULL;
	struct hsdwl_view *v;
	wl_list_for_each(v, &server->views, link)
	{
		if (v->xdg_surface
				&& v->xdg_surface->surface == focused)
			return v;
	}
	return NULL;
}

bool binding_dispatch(struct hsdwl_server *server,
	struct wlr_keyboard *kb, struct wlr_keyboard_key_event *event)
{
	struct xkb_state *state = kb->xkb_state;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(state,
		event->keycode + 8);

	struct hsdwl_binding *b;
	wl_list_for_each(b, &server->config.bindings, link)
	{
		if (!mods_match(b, kb)) continue;

		switch (b->action)
		{
		case HSDWL_ACTION_CYCLE_FOCUS:
		case HSDWL_ACTION_CYCLE_FOCUS_REVERSE:
			if (sym != XKB_KEY_Tab) continue;
			break;
		case HSDWL_ACTION_SPAWN:
			if (sym != XKB_KEY_Return) continue;
			break;
		case HSDWL_ACTION_QUIT:
			if (sym != XKB_KEY_Escape) continue;
			break;
		case HSDWL_ACTION_SWITCH_WORKSPACE:
		case HSDWL_ACTION_MOVE_TO_WORKSPACE:
			if (b->arg < 1 || b->arg > 9) continue;
			if (event->keycode != (uint32_t)(KEY_1 + (b->arg - 1))) continue;
			break;
		default:
			continue;
		}

		switch (b->action)
		{
		case HSDWL_ACTION_SPAWN:
			if (fork() == 0)
			{
				execl("/bin/sh", "sh", "-c", b->command, NULL);
				exit(EXIT_FAILURE);
			}
			return true;
		case HSDWL_ACTION_QUIT:
			wl_display_terminate(server->display);
			return true;
		case HSDWL_ACTION_CYCLE_FOCUS:
		{
			struct hsdwl_view *cur = focused_view(server);
			struct hsdwl_view *next = view_next(server, cur);
			if (next) view_focus(server, next);
			return true;
		}
		case HSDWL_ACTION_CYCLE_FOCUS_REVERSE:
		{
			struct hsdwl_view *cur = focused_view(server);
			struct hsdwl_view *prev = view_prev(server, cur);
			if (prev) view_focus(server, prev);
			return true;
		}
		case HSDWL_ACTION_SWITCH_WORKSPACE:
			if (b->arg >= 1 && b->arg <= 9)
				hsdwl_server_switch_workspace(server,
					(size_t)(b->arg - 1));
			return true;
		case HSDWL_ACTION_MOVE_TO_WORKSPACE:
		{
			struct hsdwl_view *cur = focused_view(server);
			if (cur && b->arg >= 1 && b->arg <= 9)
				hsdwl_server_move_to_workspace(server,
					cur, (size_t)(b->arg - 1));
			return true;
		}
		default:
			return false;
		}
	}
	return false;
}
