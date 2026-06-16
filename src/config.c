#define _POSIX_C_SOURCE 200809L
#include <cairo.h>
#include <ctype.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <xkbcommon/xkbcommon.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include "config.h"

#define HSDWL_MAX_VARS 64

static int parse_hex_color(const char *s, float color[4])
{
	if (!s || s[0] != '#')
		return 0;
	s++;
	size_t len = strlen(s);
	if (len != 6)
		return 0;
	char buf[3] = {0};
	for (int i = 0; i < 3; i++)
	{
		buf[0] = s[i * 2];
		buf[1] = s[i * 2 + 1];
		char *end = NULL;
		long v = strtol(buf, &end, 16);
		if (*end != '\0')
			return 0;
		color[i] = v / 255.0f;
	}
	color[3] = 1.0f;
	return 1;
}

struct config_var
{
	char key[64];
	char val[256];
};

static const char *default_config_text =
	"cursor_size = 24\n"
	"keyboard_repeat_rate = 25\n"
	"keyboard_repeat_delay = 600\n"
	"edge_threshold = 10\n"
	"min_window_size = 50\n"
	"border_width = 2\n"
	"border_color = #333333\n"
	"border_color_focused = #335577\n"
	"titlebar_color = #333333\n"
	"titlebar_color_focused = #335577\n"
	"title_font = sans-serif\n"
	"title_font_size = 12\n"
	"title_font_weight = \n"
	"titlebar_radius = 8\n"
	"title_text_color = #aaaaaa\n"
	"title_text_color_focused = #ffffff\n"
	"mod_key = Mod1\n"
	"kb_layout = us\n"
	"smart_gaps = true\n"
	"stage_manager = true\n"
	"group_overlap_threshold = 0.5\n"
	"\n"
	"bind = mod_key+Return, foot\n"
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
	"bind = mod_key+Shift+9, move_to_workspace, 9\n"
	"bind = mod_key+Q, close_focused\n"
	"bind = mod_key+i, maximize\n"
	"bind = mod_key+h, cycle_tab_prev\n"
	"bind = mod_key+l, cycle_tab_next\n";

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
	if (strcmp(s, "quit") == 0) return HSDWL_ACTION_QUIT;
	if (strcmp(s, "cycle_focus") == 0) return HSDWL_ACTION_CYCLE_FOCUS;
	if (strcmp(s, "cycle_focus_reverse") == 0) return HSDWL_ACTION_CYCLE_FOCUS_REVERSE;
	if (strcmp(s, "switch_workspace") == 0) return HSDWL_ACTION_SWITCH_WORKSPACE;
	if (strcmp(s, "move_to_workspace") == 0) return HSDWL_ACTION_MOVE_TO_WORKSPACE;
	if (strcmp(s, "close_focused") == 0) return HSDWL_ACTION_CLOSE_FOCUSED;
	if (strcmp(s, "maximize") == 0) return HSDWL_ACTION_MAXIMIZE;
	if (strcmp(s, "cycle_tab_next") == 0) return HSDWL_ACTION_CYCLE_TAB_NEXT;
	if (strcmp(s, "cycle_tab_prev") == 0) return HSDWL_ACTION_CYCLE_TAB_PREV;
	return HSDWL_ACTION_NONE;
}

static int var_key_len_desc(const void *a, const void *b)
{
	const struct config_var *va = a, *vb = b;
	int la = (int)strlen(va->key);
	int lb = (int)strlen(vb->key);
	if (la > lb) return -1;
	if (la < lb) return 1;
	return 0;
}

static void resolve_vars(char *mods, struct config_var *vars, int nv)
{
	qsort(vars, nv, sizeof(vars[0]), var_key_len_desc);
	for (int i = 0; i < nv; i++) {
		size_t klen = strlen(vars[i].key);
		size_t vlen = strlen(vars[i].val);
		if (klen == 0) continue;
		char *p;
		while ((p = strstr(mods, vars[i].key))) {
			size_t rest = strlen(p + klen) + 1;
			memmove(p + vlen, p + klen, rest);
			memcpy(p, vars[i].val, vlen);
		}
	}
}

static char *trim_tail(char *s)
{
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)*(end-1))) end--;
	*end = '\0';
	return s;
}

bool hsdwl_config_load(struct hsdwl_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	wl_list_init(&cfg->bindings);

	struct config_var vars[HSDWL_MAX_VARS];
	int num_vars = 0;

	cfg->cursor_size = 24;
	cfg->keyboard_repeat_rate = 25;
	cfg->keyboard_repeat_delay = 600;
	cfg->edge_threshold = 10;
	cfg->min_window_size = 50;
	cfg->border_width = 2;
	parse_hex_color("#444444", cfg->border_color);
	parse_hex_color("#5294e2", cfg->border_color_focused);
	cfg->titlebar_height = 0;
	cfg->titlebar_radius = 8;
	parse_hex_color("#333333", cfg->titlebar_color);
	parse_hex_color("#335577", cfg->titlebar_color_focused);
	snprintf(cfg->title_font, sizeof(cfg->title_font), "sans-serif");
	cfg->title_font_size = 12;
	cfg->title_font_weight[0] = '\0';
	parse_hex_color("#aaaaaa", cfg->title_text_color);
	parse_hex_color("#ffffff", cfg->title_text_color_focused);
	snprintf(cfg->mod_key, sizeof(cfg->mod_key), "Mod1");
	cfg->smart_gaps = true;
	cfg->stage_manager_enabled = true;
	cfg->group_overlap_threshold = 0.5f;
	parse_hex_color("#334466", cfg->preview_color);

	if (num_vars < HSDWL_MAX_VARS) {
		snprintf(vars[num_vars].key, sizeof(vars[0].key), "mod_key");
		snprintf(vars[num_vars].val, sizeof(vars[0].val), "%s", cfg->mod_key);
		num_vars++;
	}

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
			char mods_k[1024];
			char action_part[1024];
			if (sscanf(rest, "%1023[^,], %1023[^\n]", mods_k, action_part) < 2)
				continue;

			/* trim trailing spaces from mods_k */
			trim_tail(mods_k);

			/* resolve any config variable references in mods */
			resolve_vars(mods_k, vars, num_vars);

			/* capture key name before stripping it */
			char *last_plus = strrchr(mods_k, '+');
			const char *key_name = last_plus
				? last_plus + 1 : mods_k;

			/* trim trailing spaces from key name */
			char key_name_buf[64];
			snprintf(key_name_buf, sizeof(key_name_buf), "%s", key_name);
			trim_tail(key_name_buf);

			/* strip last +-separated part (the key name) from mods */
			if (last_plus)
				*last_plus = '\0';
			else
				mods_k[0] = '\0';

			struct hsdwl_binding *b = calloc(1, sizeof(*b));
			if (!b) continue;
			snprintf(b->mods, sizeof(b->mods), "%s", mods_k);
			b->keysym = xkb_keysym_from_name(key_name_buf,
				XKB_KEYSYM_CASE_INSENSITIVE);
			if (b->keysym == XKB_KEY_NoSymbol)
			{
				free(b);
				continue;
			}

			char action_str[64];
			int action_arg = 0;
			sscanf(action_part, "%63[^,],%d", action_str, &action_arg);
			trim_tail(action_str);

			b->action = parse_action(action_str);
			if (b->action != HSDWL_ACTION_NONE) {
				b->arg = action_arg;
			} else {
				b->action = HSDWL_ACTION_SPAWN;
				trim_tail(action_part);
				snprintf(b->command, sizeof(b->command), "%s", action_part);
			}

			wl_list_insert(&cfg->bindings, &b->link);
			continue;
		}

		char key[64];
		char val[256];
		if (sscanf(s, "%63[^=] = %255s", key, val) < 2) continue;

		trim_tail(key);

		if (strcmp(key, "cursor_size") == 0)
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
		else if (strcmp(key, "kb_layout") == 0)
			snprintf(cfg->kb_layout, sizeof(cfg->kb_layout), "%.127s", val);
		else if (strcmp(key, "border_width") == 0)
			cfg->border_width = atoi(val);
		else if (strcmp(key, "preview_color") == 0)
			parse_hex_color(val, cfg->preview_color);
		else if (strcmp(key, "border_color") == 0)
			parse_hex_color(val, cfg->border_color);
		else if (strcmp(key, "border_color_focused") == 0)
			parse_hex_color(val, cfg->border_color_focused);
		else if (strcmp(key, "titlebar_color") == 0)
			parse_hex_color(val, cfg->titlebar_color);
		else if (strcmp(key, "titlebar_color_focused") == 0)
			parse_hex_color(val, cfg->titlebar_color_focused);
		else if (strcmp(key, "titlebar_radius") == 0)
			cfg->titlebar_radius = atoi(val);
		else if (strcmp(key, "title_font") == 0)
			snprintf(cfg->title_font, sizeof(cfg->title_font), "%.127s", val);
		else if (strcmp(key, "title_font_size") == 0)
			cfg->title_font_size = atoi(val);
		else if (strcmp(key, "title_font_weight") == 0)
			snprintf(cfg->title_font_weight,
				sizeof(cfg->title_font_weight), "%.63s", val);
		else if (strcmp(key, "title_text_color") == 0)
			parse_hex_color(val, cfg->title_text_color);
		else if (strcmp(key, "title_text_color_focused") == 0)
			parse_hex_color(val, cfg->title_text_color_focused);
		else if (strcmp(key, "smart_gaps") == 0)
			cfg->smart_gaps = strcmp(val, "true") == 0;
		else if (strcmp(key, "stage_manager") == 0)
			cfg->stage_manager_enabled = strcmp(val, "true") == 0;
		else if (strcmp(key, "group_overlap_threshold") == 0)
			cfg->group_overlap_threshold = atof(val);

		/* store every key=value pair in var table for bind resolution */
		if (num_vars < HSDWL_MAX_VARS) {
			int found = 0;
			for (int i = 0; i < num_vars; i++) {
				if (strcmp(vars[i].key, key) == 0) {
					snprintf(vars[i].val, sizeof(vars[0].val), "%s", val);
					found = 1;
					break;
				}
			}
			if (!found) {
				snprintf(vars[num_vars].key, sizeof(vars[0].key), "%s", key);
				snprintf(vars[num_vars].val, sizeof(vars[0].val), "%s", val);
				num_vars++;
			}
		}
	}

	free(line);
	fclose(f);

	/* auto-compute titlebar height from font metrics */
	cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *ct = cairo_create(cs);
	PangoLayout *pl = pango_cairo_create_layout(ct);
	char fd[256];
	if (cfg->title_font_weight[0])
		snprintf(fd, sizeof(fd), "%s %s %d",
			cfg->title_font, cfg->title_font_weight,
			cfg->title_font_size);
	else
		snprintf(fd, sizeof(fd), "%s %d",
			cfg->title_font, cfg->title_font_size);
	PangoFontDescription *pf = pango_font_description_from_string(fd);
	pango_layout_set_font_description(pl, pf);
	PangoContext *pc = pango_layout_get_context(pl);
	PangoFontMetrics *pm = pango_context_get_metrics(pc, pf, NULL);
	int asc = pango_font_metrics_get_ascent(pm) / PANGO_SCALE;
	int dsc = pango_font_metrics_get_descent(pm) / PANGO_SCALE;
	pango_font_metrics_unref(pm);
	cfg->titlebar_height = asc + dsc + 10;
	pango_font_description_free(pf);
	g_object_unref(pl);
	cairo_destroy(ct);
	cairo_surface_destroy(cs);
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
