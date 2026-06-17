#ifndef HSDWL_TAB_GROUP_ANIM_H
#define HSDWL_TAB_GROUP_ANIM_H

#include "tab-group.h"

struct hsdwl_server;

void hsdwl_tab_group_zoom(struct hsdwl_tab_group *group,
		struct hsdwl_server *server);
void hsdwl_tab_group_maximize(struct hsdwl_tab_group *group,
		struct hsdwl_server *server);
void hsdwl_tab_group_restore(struct hsdwl_tab_group *group);

#endif
