#ifndef HSDWL_OUTPUT_MANAGEMENT_H
#define HSDWL_OUTPUT_MANAGEMENT_H

#include <stdbool.h>

struct hsdwl_server;

bool output_manager_init(struct hsdwl_server *server);
void output_manager_finish(struct hsdwl_server *server);
void output_manager_update(struct hsdwl_server *server);

#endif
