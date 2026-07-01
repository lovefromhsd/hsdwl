#ifndef HSDWL_XWAYLAND_H
#define HSDWL_XWAYLAND_H

#include <stdbool.h>

struct hsdwl_server;
struct hsdwl_view;

bool hsdwl_xwayland_init(struct hsdwl_server *server);
void hsdwl_xwayland_finish(struct hsdwl_server *server);
bool xwayland_view_is_popup(struct hsdwl_view *view);

#endif
