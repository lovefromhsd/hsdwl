#ifndef HSDWL_TAB_GROUP_LAYOUT_H
#define HSDWL_TAB_GROUP_LAYOUT_H

#include "tab-group.h"

void view_configure_size(struct hsdwl_view *view, int w, int h);
void hsdwl_tab_group_update_layout(struct hsdwl_tab_group *group);
void hsdwl_tab_group_reorder(struct hsdwl_tab_group *group,
		struct hsdwl_view *view, int new_index);
void hsdwl_tab_group_show_preview(struct hsdwl_server *server,
		struct hsdwl_view *target, double cursor_x, double cursor_y);
void hsdwl_tab_group_hide_preview(struct hsdwl_server *server);
struct hsdwl_tab_button *tab_button_create(
		struct hsdwl_tab_group *group, struct hsdwl_view *view);
void tab_button_destroy(struct hsdwl_tab_button *btn);

#endif
