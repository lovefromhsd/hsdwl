#ifndef HSDWL_TAB_GROUP_H
#define HSDWL_TAB_GROUP_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct hsdwl_server;
struct hsdwl_view;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_buffer;

enum hsdwl_tab_orientation
{
	HSDWL_TAB_HORIZONTAL,
	HSDWL_TAB_VERTICAL,
};

struct hsdwl_tab_button
{
	struct wl_list link;
	struct hsdwl_view *view;
	struct wlr_scene_buffer *text;
};

struct hsdwl_tab_group
{
	struct wl_list link;
	struct hsdwl_server *server;
	struct wl_list views;
	struct hsdwl_view *active;
	struct wlr_scene_tree *scene_tree;
	struct wlr_scene_buffer *tab_bar_bg;
	struct wl_list tab_buttons;
	struct wlr_scene_tree *content_area;
	struct wlr_box content_area_box;
	int tab_bar_thickness;
	enum hsdwl_tab_orientation orientation;
};

void hsdwl_tab_group_init(struct hsdwl_server *server);
void hsdwl_tab_group_finish(struct hsdwl_server *server);
struct hsdwl_tab_group *hsdwl_tab_group_create(struct hsdwl_server *server,
		struct hsdwl_view *a, struct hsdwl_view *b,
		enum hsdwl_tab_orientation orientation);
void hsdwl_tab_group_add_view(struct hsdwl_tab_group *group,
		struct hsdwl_view *view);
void hsdwl_tab_group_remove_view(struct hsdwl_tab_group *group,
		struct hsdwl_view *view);
void hsdwl_tab_group_set_active(struct hsdwl_tab_group *group,
		struct hsdwl_view *view);
void hsdwl_tab_group_destroy(struct hsdwl_tab_group *group);
bool hsdwl_tab_group_is_member(struct hsdwl_view *view);
void hsdwl_tab_group_update_layout(struct hsdwl_tab_group *group);
void hsdwl_tab_group_reorder(struct hsdwl_tab_group *group,
		struct hsdwl_view *view, int new_index);
struct hsdwl_view *hsdwl_tab_group_view_at(struct hsdwl_server *server,
		double lx, double ly);
struct hsdwl_view *hsdwl_tab_group_next(struct hsdwl_tab_group *group,
		struct hsdwl_view *current, bool reverse);
struct hsdwl_tab_group *hsdwl_tab_group_at(struct hsdwl_server *server,
		double lx, double ly);
void hsdwl_tab_group_show_preview(struct hsdwl_server *server,
		struct hsdwl_view *target, double cursor_x, double cursor_y);
void hsdwl_tab_group_hide_preview(struct hsdwl_server *server);

#endif
