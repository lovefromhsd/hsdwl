#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include "config.h"

static const char *default_config_text =
	"terminal = foot\n"
	"cursor_size = 24\n"
	"keyboard_repeat_rate = 25\n"
	"keyboard_repeat_delay = 600\n"
	"edge_threshold = 10\n"
	"min_window_size = 50\n"
	"mod_key = Mod1\n"
	"\n"
	"bind = mod_key+Return, spawn_terminal\n"
	"bind = mod_key+Escape, quit\n"
	"bind = mod_key+Tab, cycle_focus\n"
	"bind = mod_key+Shift+Tab, cycle_focus_reverse\n"
	"bind = mod_key+1, switch_workspace, 1\n"
	"bind = mod_key+2, switch_workspace, 2\n"
	"bind = mod_key+3, switch_workspace, 3\n"
	"bind = mod_key+4, switch_workspace, 4\n"
	"bind = mod_key+5, switch_workspace, 5\n"
	"bind = mod_key+6, switch_workspace, 6\n"
	"bind = mod_key+7, switch_workspace, 7\n"
	"bind = mod_key+8, switch_workspace, 8\n"
	"bind = mod_key+9, switch_workspace, 9\n"
	"bind = mod_key+Shift+1, move_to_workspace, 1\n"
	"bind = mod_key+Shift+2, move_to_workspace, 2\n"
	"bind = mod_key+Shift+3, move_to_workspace, 3\n"
	"bind = mod_key+Shift+4, move_to_workspace, 4\n"
	"bind = mod_key+Shift+5, move_to_workspace, 5\n"
	"bind = mod_key+Shift+6, move_to_workspace, 6\n"
	"bind = mod_key+Shift+7, move_to_workspace, 7\n"
	"bind = mod_key+Shift+8, move_to_workspace, 8\n"
	"bind = mod_key+Shift+9, move_to_workspace, 9\n";

static void write_default_config(const char *path)
{
	FILE *f = fopen(path, "we");
	if (!f) return;
	fwrite(default_config_text, 1, strlen(default_config_text), f);
	fclose(f);
}

static char *config_path(void)
{
	static char buf[4096];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && xdg[0]) {
		snprintf(buf, sizeof(buf), "%s/hsdwl/config", xdg);
	} else {
		const char *home = getenv("HOME");
		if (home && home[0]) {
			snprintf(buf, sizeof(buf), "%s/.config/hsdwl/config", home);
		} else {
			struct passwd *pw = getpwuid(getuid());
			if (pw && pw->pw_dir)
				snprintf(buf, sizeof(buf), "%s/.config/hsdwl/config", pw->pw_dir);
			else
				snprintf(buf, sizeof(buf), "/tmp/hsdwl_config");
		}
	}
	return buf;
}

static int parse_action(const char *s)
{
	if (strcmp(s, "spawn_terminal") == 0) return HSDWL_ACTION_SPAWN_TERMINAL;
	if (strcmp(s, "quit") == 0) return HSDWL_ACTION_QUIT;
	if (strcmp(s, "cycle_focus") == 0) return HSDWL_ACTION_CYCLE_FOCUS;
	if (strcmp(s, "cycle_focus_reverse") == 0) return HSDWL_ACTION_CYCLE_FOCUS_REVERSE;
	if (strcmp(s, "switch_workspace") == 0) return HSDWL_ACTION_SWITCH_WORKSPACE;
	if (strcmp(s, "move_to_workspace") == 0) return HSDWL_ACTION_MOVE_TO_WORKSPACE;
	return HSDWL_ACTION_NONE;
}

static void resolve_mod_key_token(char *mods, const char *mod_key)
{
	char *p;
	while ((p = strstr(mods, "mod_key"))) {
		size_t mklen = strlen(mod_key);
		size_t rest = strlen(p + 7) + 1;
		memmove(p + mklen, p + 7, rest);
		memcpy(p, mod_key, mklen);
	}
}

bool hsdwl_config_load(struct hsdwl_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	wl_list_init(&cfg->bindings);

	cfg->cursor_size = 24;
	cfg->keyboard_repeat_rate = 25;
	cfg->keyboard_repeat_delay = 600;
	cfg->edge_threshold = 10;
	cfg->min_window_size = 50;
	snprintf(cfg->terminal, sizeof(cfg->terminal), "foot");
	snprintf(cfg->mod_key, sizeof(cfg->mod_key), "Mod1");

	char *path = config_path();
	FILE *f = fopen(path, "re");
	if (!f) {
		write_default_config(path);
		f = fopen(path, "re");
		if (!f) return false;
	}

	char *line = NULL;
	size_t len = 0;
	ssize_t nread;

	while ((nread = getline(&line, &len, f)) != -1) {
		if (nread > 0 && line[nread-1] == '\n') line[nread-1] = '\0';
		char *s = line;
		while (*s && isspace((unsigned char)*s)) s++;
		if (*s == '#' || *s == '\0') continue;

		if (strncmp(s, "bind = ", 7) == 0) {
			char *rest = s + 7;
			char mods_k[128];
			char action_str[64];
			int arg = 0;
			int matched = sscanf(rest, "%127[^,], %63[^,],%d", mods_k, action_str, &arg);
			if (matched < 2) continue;

			/* remove trailing spaces from mods_k and action_str */
			char *end = mods_k + strlen(mods_k);
			while (end > mods_k && isspace((unsigned char)*(end-1))) end--;
			*end = '\0';
			end = action_str + strlen(action_str);
			while (end > action_str && isspace((unsigned char)*(end-1))) end--;
			*end = '\0';

			resolve_mod_key_token(mods_k, cfg->mod_key);

			/* strip last +-separated part (the key name) from mods */
			char *last_plus = strrchr(mods_k, '+');
			if (last_plus)
				*last_plus = '\0';
			else
				mods_k[0] = '\0';

			struct hsdwl_binding *b = calloc(1, sizeof(*b));
			if (!b) continue;
			snprintf(b->mods, sizeof(b->mods), "%s", mods_k);
			b->action = parse_action(action_str);
			b->arg = arg;

			/* figure out key matching — store keycode 0 sym 0 for non-number */
			b->keycode = 0;
			b->sym = XKB_KEY_NoSymbol;

			wl_list_insert(&cfg->bindings, &b->link);
			continue;
		}

		char key[64];
		char val[256];
		if (sscanf(s, "%63[^=] = %255s", key, val) < 2) continue;

		if (strcmp(key, "terminal") == 0)
			snprintf(cfg->terminal, sizeof(cfg->terminal), "%.255s", val);
		else if (strcmp(key, "cursor_size") == 0)
			cfg->cursor_size = atoi(val);
		else if (strcmp(key, "keyboard_repeat_rate") == 0)
			cfg->keyboard_repeat_rate = atoi(val);
		else if (strcmp(key, "keyboard_repeat_delay") == 0)
			cfg->keyboard_repeat_delay = atoi(val);
		else if (strcmp(key, "edge_threshold") == 0)
			cfg->edge_threshold = atoi(val);
		else if (strcmp(key, "min_window_size") == 0)
			cfg->min_window_size = atoi(val);
		else if (strcmp(key, "mod_key") == 0)
			snprintf(cfg->mod_key, sizeof(cfg->mod_key), "%.31s", val);
	}

	free(line);
	fclose(f);
	return true;
}

void hsdwl_config_finish(struct hsdwl_config *cfg)
{
	struct hsdwl_binding *b, *tmp;
	wl_list_for_each_safe(b, tmp, &cfg->bindings, link) {
		wl_list_remove(&b->link);
		free(b);
	}
}
