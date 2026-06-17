#include "animation.h"
#include "server.h"
#include <wlr/types/wlr_scene.h>
#include <stdlib.h>
#include <math.h>

static inline double ease_linear(double t)
{
	return t;
}

static inline double ease_out_quad(double t)
{
	return t * (2.0 - t);
}

static inline double ease_out_cubic(double t)
{
	double t1 = t - 1.0;
	return t1 * t1 * t1 + 1.0;
}

void animation_create(struct hsdwl_server *server,
	struct wlr_scene_buffer *buffer,
	int duration_ms, enum hsdwl_easing easing,
	double from_x, double from_y, int from_w, int from_h,
	double to_x, double to_y, int to_w, int to_h,
	void (*on_finish)(struct hsdwl_server *, void *),
	void *user_data)
{
	struct hsdwl_animation *anim = calloc(1, sizeof(*anim));
	if (!anim)
		return;

	anim->buffer = buffer;
	anim->duration_ms = duration_ms;
	anim->easing = easing;
	anim->from_x = from_x;
	anim->from_y = from_y;
	anim->to_x = to_x;
	anim->to_y = to_y;
	anim->from_w = from_w;
	anim->from_h = from_h;
	anim->to_w = to_w;
	anim->to_h = to_h;
	anim->on_finish = on_finish;
	anim->user_data = user_data;

	clock_gettime(CLOCK_MONOTONIC, &anim->start);

	wl_list_insert(&server->animations, &anim->link);
}

void animation_create_node_pos(struct hsdwl_server *server,
	struct wlr_scene_node *node,
	int duration_ms, enum hsdwl_easing easing,
	double from_x, double from_y,
	double to_x, double to_y,
	void (*on_finish)(struct hsdwl_server *, void *),
	void *user_data)
{
	struct hsdwl_animation *anim = calloc(1, sizeof(*anim));
	if (!anim)
		return;
	anim->pos_node = node;
	anim->buffer = NULL;
	anim->duration_ms = duration_ms;
	anim->easing = easing;
	anim->from_x = from_x;
	anim->from_y = from_y;
	anim->to_x = to_x;
	anim->to_y = to_y;
	anim->on_finish = on_finish;
	anim->user_data = user_data;
	clock_gettime(CLOCK_MONOTONIC, &anim->start);
	wl_list_insert(&server->animations, &anim->link);
}

void animation_tick(struct hsdwl_server *server, struct timespec *now)
{
	struct hsdwl_animation *anim, *tmp;
	wl_list_for_each_safe(anim, tmp, &server->animations, link) {
		double elapsed_ms =
			(double)(now->tv_sec - anim->start.tv_sec) * 1000.0 +
			(double)(now->tv_nsec - anim->start.tv_nsec) / 1000000.0;

		double t = elapsed_ms / (double)anim->duration_ms;
		if (t < 0.0)
			t = 0.0;
		if (t > 1.0)
			t = 1.0;

		double eased_t;
		switch (anim->easing) {
		case HSDWL_EASE_OUT_QUAD:
			eased_t = ease_out_quad(t);
			break;
		case HSDWL_EASE_OUT_CUBIC:
			eased_t = ease_out_cubic(t);
			break;
		default:
			eased_t = ease_linear(t);
			break;
		}

		double x = anim->from_x + (anim->to_x - anim->from_x) * eased_t;
		double y = anim->from_y + (anim->to_y - anim->from_y) * eased_t;

		if (anim->pos_node) {
			wlr_scene_node_set_position(anim->pos_node,
				(int)round(x), (int)round(y));
		} else {
			double w = anim->from_w + (anim->to_w - anim->from_w) * eased_t;
			double h = anim->from_h + (anim->to_h - anim->from_h) * eased_t;
			wlr_scene_node_set_position(&anim->buffer->node,
				(int)round(x), (int)round(y));
			wlr_scene_buffer_set_dest_size(anim->buffer,
				(int)round(w), (int)round(h));
		}

		if (t >= 1.0) {
			if (anim->pos_node) {
				wlr_scene_node_set_position(anim->pos_node,
					(int)round(anim->to_x), (int)round(anim->to_y));
			} else {
				wlr_scene_node_set_position(&anim->buffer->node,
					(int)round(anim->to_x), (int)round(anim->to_y));
				wlr_scene_buffer_set_dest_size(anim->buffer,
					anim->to_w, anim->to_h);
			}

			wl_list_remove(&anim->link);
			if (anim->on_finish)
				anim->on_finish(server, anim->user_data);
			free(anim);
		}
	}
}

void animation_cancel_all(struct hsdwl_server *server)
{
	struct hsdwl_animation *anim, *tmp;
	wl_list_for_each_safe(anim, tmp, &server->animations, link) {
		wl_list_remove(&anim->link);
		free(anim);
	}
}

void animation_cancel_buffer(struct hsdwl_server *server,
	struct wlr_scene_buffer *buffer)
{
	struct hsdwl_animation *anim, *tmp;
	wl_list_for_each_safe(anim, tmp, &server->animations, link) {
		if (anim->buffer == buffer) {
			wl_list_remove(&anim->link);
			free(anim);
		}
	}
}
