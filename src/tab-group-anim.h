#ifndef HSDWL_TAB_GROUP_ANIM_H
#define HSDWL_TAB_GROUP_ANIM_H

#include "tab-group.h"

struct hsdwl_server;
struct wlr_buffer;


void hsdwl_tab_group_zoom(struct hsdwl_tab_group *group,
		struct hsdwl_server *server);
void hsdwl_tab_group_maximize(struct hsdwl_tab_group *group,
		struct hsdwl_server *server);
void hsdwl_tab_group_restore(struct hsdwl_tab_group *group);
void tab_group_demaximize_to_zoomed(struct hsdwl_tab_group *group,
		struct hsdwl_server *server);
struct wlr_buffer *tab_group_capture_full(
	struct hsdwl_server *server,
	struct hsdwl_tab_group *group,
	int content_w, int content_h);

#endif
