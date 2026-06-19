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
#include <stddef.h>
#include "config.h"
#include "config_defs.h"

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

static const int def_cursor_size = 24;
static const int def_keyboard_repeat_rate = 25;
static const int def_keyboard_repeat_delay = 600;
static const int def_edge_threshold = 10;
static const int def_min_window_size = 50;
static const int def_border_width = 2;
static const int def_title_font_size = 12;
static const int def_titlebar_radius = 8;
static const int def_stage_anim_duration = 400;
static const float def_group_overlap_threshold = 0.5f;
static const bool def_smart_gaps = true;
static const bool def_stage_manager_enabled = true;
static const bool def_stage_3d_enabled = true;
static const int def_stage_float_max_size = 360;

const struct config_field config_fields[] = {
	{"cursor_size",             FIELD_INT,    offsetof(struct hsdwl_config, cursor_size),             0, &def_cursor_size},
	{"keyboard_repeat_rate",    FIELD_INT,    offsetof(struct hsdwl_config, keyboard_repeat_rate),    0, &def_keyboard_repeat_rate},
	{"keyboard_repeat_delay",   FIELD_INT,    offsetof(struct hsdwl_config, keyboard_repeat_delay),   0, &def_keyboard_repeat_delay},
	{"edge_threshold",          FIELD_INT,    offsetof(struct hsdwl_config, edge_threshold),          0, &def_edge_threshold},
	{"min_window_size",         FIELD_INT,    offsetof(struct hsdwl_config, min_window_size),         0, &def_min_window_size},
	{"border_width",            FIELD_INT,    offsetof(struct hsdwl_config, border_width),            0, &def_border_width},
	{"preview_color",           FIELD_COLOR,  offsetof(struct hsdwl_config, preview_color),           0, "#334466"},
	{"border_color",            FIELD_COLOR,  offsetof(struct hsdwl_config, border_color),            0, "#444444"},
	{"border_color_focused",    FIELD_COLOR,  offsetof(struct hsdwl_config, border_color_focused),    0, "#5294e2"},
	{"titlebar_color",          FIELD_COLOR,  offsetof(struct hsdwl_config, titlebar_color),          0, "#333333"},
	{"titlebar_color_focused",  FIELD_COLOR,  offsetof(struct hsdwl_config, titlebar_color_focused),  0, "#335577"},
	{"title_font",              FIELD_STRING, offsetof(struct hsdwl_config, title_font),              128, "sans-serif"},
	{"title_font_size",         FIELD_INT,    offsetof(struct hsdwl_config, title_font_size),         0, &def_title_font_size},
	{"title_font_weight",       FIELD_STRING, offsetof(struct hsdwl_config, title_font_weight),       64, ""},
	{"titlebar_radius",         FIELD_INT,    offsetof(struct hsdwl_config, titlebar_radius),         0, &def_titlebar_radius},
	{"title_text_color",        FIELD_COLOR,  offsetof(struct hsdwl_config, title_text_color),        0, "#aaaaaa"},
	{"title_text_color_focused", FIELD_COLOR, offsetof(struct hsdwl_config, title_text_color_focused), 0, "#ffffff"},
	{"mod_key",                 FIELD_STRING, offsetof(struct hsdwl_config, mod_key),                 32, "Mod1"},
	{"kb_layout",               FIELD_STRING, offsetof(struct hsdwl_config, kb_layout),               128, "us"},
	{"smart_gaps",              FIELD_BOOL,   offsetof(struct hsdwl_config, smart_gaps),              0, &def_smart_gaps},
	{"stage_manager",           FIELD_BOOL,   offsetof(struct hsdwl_config, stage_manager_enabled),   0, &def_stage_manager_enabled},
	{"stage_anim_duration",     FIELD_INT,    offsetof(struct hsdwl_config, stage_anim_duration),     0, &def_stage_anim_duration},
	{"stage_3d_enabled",        FIELD_BOOL,   offsetof(struct hsdwl_config, stage_3d_enabled),        0, &def_stage_3d_enabled},
	{"stage_float_max_size",    FIELD_INT,    offsetof(struct hsdwl_config, stage_float_max_size),    0, &def_stage_float_max_size},
	{"group_overlap_threshold", FIELD_FLOAT,  offsetof(struct hsdwl_config, group_overlap_threshold), 0, &def_group_overlap_threshold},
	{"animation_bezier",        FIELD_BEZIER, offsetof(struct hsdwl_config, anim_bezier_x1),          16, "0.25, 0.1, 0.25, 1.0"},
};
int config_fields_count = sizeof(config_fields) / sizeof(config_fields[0]);

const struct action_entry action_map[] = {
	{"quit",                 HSDWL_ACTION_QUIT},
	{"cycle_focus",          HSDWL_ACTION_CYCLE_FOCUS},
	{"cycle_focus_reverse",  HSDWL_ACTION_CYCLE_FOCUS_REVERSE},
	{"switch_workspace",     HSDWL_ACTION_SWITCH_WORKSPACE},
	{"move_to_workspace",    HSDWL_ACTION_MOVE_TO_WORKSPACE},
	{"close_focused",        HSDWL_ACTION_CLOSE_FOCUSED},
	{"maximize",             HSDWL_ACTION_MAXIMIZE},
	{"cycle_tab_next",       HSDWL_ACTION_CYCLE_TAB_NEXT},
	{"cycle_tab_prev",       HSDWL_ACTION_CYCLE_TAB_PREV},
};
int action_map_count = sizeof(action_map) / sizeof(action_map[0]);

static void write_default_config(const char *path)
{
	FILE *f = fopen(path, "we");
	if (!f) return;
	for (int i = 0; i < config_fields_count; i++) {
		const struct config_field *field = &config_fields[i];
		switch (field->type) {
		case FIELD_INT:
			fprintf(f, "%s = %d\n", field->key, *(const int*)field->default_ptr);
			break;
		case FIELD_BOOL:
			fprintf(f, "%s = %s\n", field->key, *(const bool*)field->default_ptr ? "true" : "false");
			break;
		case FIELD_FLOAT:
			fprintf(f, "%s = %g\n", field->key, *(const float*)field->default_ptr);
			break;
		case FIELD_STRING:
		case FIELD_COLOR:
		case FIELD_BEZIER:
			fprintf(f, "%s = %s\n", field->key, (const char*)field->default_ptr);
			break;
		}
	}
	fputs("\n", f);
	fputs("bind = mod_key+Return, foot\n", f);
	fputs("bind = mod_key+Escape, quit\n", f);
	fputs("bind = mod_key+Tab, cycle_focus\n", f);
	fputs("bind = mod_key+Shift+Tab, cycle_focus_reverse\n", f);
	fputs("bind = mod_key+1, switch_workspace, 1\n", f);
	fputs("bind = mod_key+2, switch_workspace, 2\n", f);
	fputs("bind = mod_key+3, switch_workspace, 3\n", f);
	fputs("bind = mod_key+4, switch_workspace, 4\n", f);
	fputs("bind = mod_key+5, switch_workspace, 5\n", f);
	fputs("bind = mod_key+6, switch_workspace, 6\n", f);
	fputs("bind = mod_key+7, switch_workspace, 7\n", f);
	fputs("bind = mod_key+8, switch_workspace, 8\n", f);
	fputs("bind = mod_key+9, switch_workspace, 9\n", f);
	fputs("bind = mod_key+Shift+1, move_to_workspace, 1\n", f);
	fputs("bind = mod_key+Shift+2, move_to_workspace, 2\n", f);
	fputs("bind = mod_key+Shift+3, move_to_workspace, 3\n", f);
	fputs("bind = mod_key+Shift+4, move_to_workspace, 4\n", f);
	fputs("bind = mod_key+Shift+5, move_to_workspace, 5\n", f);
	fputs("bind = mod_key+Shift+6, move_to_workspace, 6\n", f);
	fputs("bind = mod_key+Shift+7, move_to_workspace, 7\n", f);
	fputs("bind = mod_key+Shift+8, move_to_workspace, 8\n", f);
	fputs("bind = mod_key+Shift+9, move_to_workspace, 9\n", f);
	fputs("bind = mod_key+Q, close_focused\n", f);
	fputs("bind = mod_key+i, maximize\n", f);
	fputs("bind = mod_key+h, cycle_tab_prev\n", f);
	fputs("bind = mod_key+l, cycle_tab_next\n", f);
	fputs("bind = Control+Shift+Tab, cycle_tab_prev\n", f);
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
	for (int i = 0; i < action_map_count; i++)
		if (strcmp(s, action_map[i].name) == 0)
			return action_map[i].action;
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

static void parse_field(struct hsdwl_config *cfg, const struct config_field *f, const char *val)
{
	switch (f->type) {
	case FIELD_INT:
		*(int*)((char*)cfg + f->offset) = atoi(val);
		break;
	case FIELD_BOOL:
		*(bool*)((char*)cfg + f->offset) = (strcmp(val, "true") == 0);
		break;
	case FIELD_FLOAT:
		*(float*)((char*)cfg + f->offset) = atof(val);
		break;
	case FIELD_STRING:
		snprintf((char*)cfg + f->offset, f->data_size, "%.*s", (int)f->data_size - 1, val);
		break;
	case FIELD_COLOR:
		parse_hex_color(val, (float*)((char*)cfg + f->offset));
		break;
	case FIELD_BEZIER:
		{
			float vals[4];
			if (sscanf(val, "%f, %f, %f, %f", &vals[0], &vals[1], &vals[2], &vals[3]) == 4) {
				float *dst = (float*)((char*)cfg + f->offset);
				for (int j = 0; j < 4; j++) dst[j] = vals[j];
			}
		}
		break;
	}
}

bool hsdwl_config_load(struct hsdwl_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	wl_list_init(&cfg->bindings);

	struct config_var vars[HSDWL_MAX_VARS];
	int num_vars = 0;

	for (int i = 0; i < config_fields_count; i++) {
		const struct config_field *f = &config_fields[i];
		if (strcmp(f->key, "mod_key") == 0) continue;
		if (strcmp(f->key, "kb_layout") == 0) continue;
		if (strcmp(f->key, "title_font") == 0) continue;
		if (strcmp(f->key, "title_font_weight") == 0) continue;
		switch (f->type) {
		case FIELD_INT:
			*(int*)((char*)cfg + f->offset) = *(const int*)f->default_ptr;
			break;
		case FIELD_BOOL:
			*(bool*)((char*)cfg + f->offset) = *(const bool*)f->default_ptr;
			break;
		case FIELD_FLOAT:
			*(float*)((char*)cfg + f->offset) = *(const float*)f->default_ptr;
			break;
		case FIELD_STRING:
			snprintf((char*)cfg + f->offset, f->data_size, "%s", (const char*)f->default_ptr);
			break;
		case FIELD_COLOR:
			parse_hex_color((const char*)f->default_ptr, (float*)((char*)cfg + f->offset));
			break;
		case FIELD_BEZIER:
			{
				float vals[4];
				const char *s = (const char*)f->default_ptr;
				if (sscanf(s, "%f, %f, %f, %f", &vals[0], &vals[1], &vals[2], &vals[3]) == 4) {
					float *dst = (float*)((char*)cfg + f->offset);
					for (int j = 0; j < 4; j++) dst[j] = vals[j];
				}
			}
			break;
		}
	}

	cfg->titlebar_height = 0;
	snprintf(cfg->title_font, sizeof(cfg->title_font), "sans-serif");
	cfg->title_font_weight[0] = '\0';
	snprintf(cfg->mod_key, sizeof(cfg->mod_key), "Mod1");

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

			
			trim_tail(mods_k);

			
			resolve_vars(mods_k, vars, num_vars);

			
			char *last_plus = strrchr(mods_k, '+');
			const char *key_name = last_plus
				? last_plus + 1 : mods_k;

			
			char key_name_buf[64];
			snprintf(key_name_buf, sizeof(key_name_buf), "%s", key_name);
			trim_tail(key_name_buf);

			
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
		if (sscanf(s, "%63[^=] = %255[^\n]", key, val) < 2) continue;

		trim_tail(key);

		for (int i = 0; i < config_fields_count; i++) {
			if (strcmp(key, config_fields[i].key) == 0) {
				parse_field(cfg, &config_fields[i], val);
				break;
			}
		}

		
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
