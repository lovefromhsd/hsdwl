#ifndef HSDWL_STAGE_ANIM_H
#define HSDWL_STAGE_ANIM_H

#include <stddef.h>

struct hsdwl_server;
struct custom_stage;

void stage_manager_switch(struct hsdwl_server *server,
		struct custom_stage *target, size_t ws);
void stage_manager_cycle(struct hsdwl_server *server,
		size_t ws, bool reverse);
void stage_manager_merge(struct hsdwl_server *server,
		struct custom_stage *source, size_t ws);

#endif
