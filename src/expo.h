#ifndef HSDWL_EXPO_H
#define HSDWL_EXPO_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "server.h"

struct wlr_output;
struct wlr_texture;
struct wlr_buffer;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_buffer;
struct wlr_keyboard;
struct wlr_keyboard_key_event;
struct wlr_pointer_button_event;
struct wlr_pointer_axis_event;

/* Ring camera + strip constants (reimplementation of fwm's expo design) */
#define HSDWL_EXPO_SNAP_SCALE 1.0
#define HSDWL_EXPO_ZOOM_NEAR 1.3
#define HSDWL_EXPO_ZOOM_FAR 3.0
#define HSDWL_EXPO_ZOOM_SPEED 11.0
#define HSDWL_EXPO_GAP_FRAC 0.04
#define HSDWL_EXPO_EDGE_PX 2.0
#define HSDWL_EXPO_EDGE_GREY 0.34f
#define HSDWL_EXPO_CARD_GREY 0.13f
#define HSDWL_EXPO_INNER_ALPHA 0.88f
#define HSDWL_EXPO_INNER_IMAGE 0.22f
#define HSDWL_EXPO_TILT_PULLBACK 1.0
#define HSDWL_EXPO_PAN_SPEED 14.0
#define HSDWL_EXPO_SEAM_PITCHES 3.0
#define HSDWL_EXPO_TILT_MAX 1.05
#define HSDWL_EXPO_TILT_STEP 0.12
#define HSDWL_EXPO_DIST_MIN 0.6
#define HSDWL_EXPO_DIST_MAX 3.0
#define HSDWL_EXPO_DIST_STEP 1.18
#define HSDWL_EXPO_ORBIT_SPEED 9.0
#define HSDWL_EXPO_ORBIT_HOME_SPEED 18.0
#define HSDWL_EXPO_SPIN_DECAY 2.4
#define HSDWL_EXPO_SPIN_MIN 40.0
#define HSDWL_EXPO_CAM_DIST 2.2
#define HSDWL_EXPO_BACKDROP_DARK 0.55
#define HSDWL_EXPO_HILIGHT_PX 6.0
#define HSDWL_EXPO_CARD_CELLS 16
#define HSDWL_EXPO_FRONT_REFRESH_S 0.1
#define HSDWL_EXPO_ALL_REFRESH_S 1.0

/* A point in ring space: x right, y down, z toward the viewer.
 * The origin sits at the front of the strip. */
struct expo_pt
{
	double x, y, z;
};

/* A projected vertex: screen x/y plus a real perspective w, plus
 * texture coordinates. */
struct expo_vert
{
	double x, y, w, u, v;
};

struct hsdwl_expo
{
	struct hsdwl_server *server;
	struct wlr_output *out;
	int sw, sh;        /* logical output size */
	int ox, oy;        /* output position in the layout */

	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *backdrop;
	struct wlr_scene_buffer *canvas;
	struct wlr_buffer *canvas_buf[2];
	int canvas_i;

	struct wlr_texture *wp_tex;  /* wallpaper composite, full screen */
	struct wlr_texture *bg_tex;  /* blurred, darkened backdrop */
	struct wlr_buffer *bg_buf;   /* backdrop buffer (scene owns it) */

	/* one snapshot per desktop, at HSDWL_EXPO_SNAP_SCALE */
	struct wlr_texture *card_tex[HSDWL_NUM_WORKSPACES];
	struct wlr_buffer *card_buf[HSDWL_NUM_WORKSPACES];
	int generation;      /* bumped on every capture; canvas cache key */

	bool curved;         /* ring camera enabled (stage_3d_enabled) */

	double zoom, zoom_target;
	double pan, pan_target;
	double tilt, tilt_target;
	double dist, dist_target;

	bool orbiting;
	double orbit_x, orbit_y;
	double orbit_dt;
	struct timespec orbit_time;
	double spin;         /* strip px/s coast */

	double clock;        /* seconds the strip has been open */

	int home;            /* desktop the strip was opened from */
	bool leaving;

	bool canvas_dirty;
	double drawn_zoom, drawn_pan, drawn_tilt, drawn_dist;
	int drawn_generation;
	int drawn_hover_d;

	int hover_d;         /* desktop under the cursor, -1 none */

	double lx, ly;       /* last pointer position in output coords */

	struct timespec last_front_refresh;
	struct timespec last_all_refresh;
	struct timespec last_tick;
	bool have_last_tick;
};

/* Public API */
bool expo_open(struct hsdwl_server *server);
void expo_close(struct hsdwl_server *server, int desktop);
void expo_toggle(struct hsdwl_server *server);
void expo_tick(struct hsdwl_server *server, const struct timespec *now);
bool expo_needs_frame(struct hsdwl_server *server);

bool expo_handle_key(struct hsdwl_expo *e, struct wlr_keyboard *kb,
		struct wlr_keyboard_key_event *event);
bool expo_handle_motion(struct hsdwl_expo *e, double gx, double gy);
bool expo_handle_button(struct hsdwl_expo *e,
		struct wlr_pointer_button_event *event);
bool expo_handle_axis(struct hsdwl_expo *e,
		struct wlr_pointer_axis_event *event);

void expo_output_gone(struct hsdwl_server *server, struct wlr_output *out);
void expo_destroy(struct hsdwl_server *server);

#endif
