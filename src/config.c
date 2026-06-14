#define _GNU_SOURCE

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_defaults(struct hsdwl_config *cfg)
{
	strcpy(cfg->terminal, "foot");
	cfg->cursor_size = 24;
	cfg->keyboard_repeat_rate = 25;
	cfg->keyboard_repeat_delay = 600;
	cfg->edge_threshold = 10;
	cfg->min_window_size = 50;
	strcpy(cfg->mod_key, "Mod1");
}

static char *config_path(void)
{
	const char *home = getenv("XDG_CONFIG_HOME");
	char *path = NULL;
	if (home && home[0])
	{
		if (asprintf(&path, "%s/hsdwl/config", home) < 0)
			path = NULL;
	}
	else
	{
		home = getenv("HOME");
		if (home && home[0])
		{
			if (asprintf(&path, "%s/.config/hsdwl/config", home) < 0)
				path = NULL;
		}
	}
	return path;
}

static bool ensure_dir(const char *path)
{
	// path is "basedir/filename" — extract basedir
	char *dir = strdup(path);
	if (!dir)
		return false;
	char *slash = strrchr(dir, '/');
	if (slash)
		*slash = '\0';
	int rc = mkdir(dir, 0755);
	free(dir);
	return rc == 0 || errno == EEXIST;
}

static void write_default(FILE *f, const struct hsdwl_config *cfg)
{
	fprintf(f, "# hsdwl config\n");
	fprintf(f, "\n");
	fprintf(f, "terminal = %s\n", cfg->terminal);
	fprintf(f, "cursor_size = %d\n", cfg->cursor_size);
	fprintf(f, "keyboard_repeat_rate = %d\n", cfg->keyboard_repeat_rate);
	fprintf(f, "keyboard_repeat_delay = %d\n", cfg->keyboard_repeat_delay);
	fprintf(f, "edge_threshold = %d\n", cfg->edge_threshold);
	fprintf(f, "min_window_size = %d\n", cfg->min_window_size);
	fprintf(f, "mod_key = %s\n", cfg->mod_key);
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	if (*s == '\0')
		return s;
	char *end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		end--;
	*(end + 1) = '\0';
	return s;
}

static bool parse_line(struct hsdwl_config *cfg, const char *line)
{
	char key[128], val[1024];
	if (sscanf(line, "%127s = %1023[^\n]", key, val) < 2)
		return false;

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

	return true;
}

bool hsdwl_config_load(struct hsdwl_config *cfg)
{
	set_defaults(cfg);

	char *path = config_path();
	if (!path)
		return true;

	FILE *f = fopen(path, "r");
	if (!f)
	{
		if (ensure_dir(path))
		{
			f = fopen(path, "w");
			if (f)
			{
				write_default(f, cfg);
				fclose(f);
			}
		}
		free(path);
		return true;
	}

	char *line = NULL;
	size_t len = 0;
	while (getline(&line, &len, f) >= 0)
	{
		char *t = trim(line);
		if (t[0] == '\0' || t[0] == '#')
			continue;
		parse_line(cfg, t);
	}
	free(line);
	fclose(f);
	free(path);
	return true;
}
