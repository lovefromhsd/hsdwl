#define _GNU_SOURCE
#define WLR_USE_UNSTABLE

#include "expo.h"
#include "config.h"
#include "layer-shell.h"
#include "server.h"

#include <drm_fourcc.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xkbcommon/xkbcommon.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

static double expo_openness(const struct hsdwl_expo *e);
static double expo_gap(const struct hsdwl_expo *e);
static double expo_pitch(const struct hsdwl_expo *e);
static double expo_scale(const struct hsdwl_expo *e);
static double expo_center(const struct hsdwl_expo *e);
static double expo_lap(const struct hsdwl_expo *e);
static double expo_radius(const struct hsdwl_expo *e);
static double expo_dist(const struct hsdwl_expo *e);
static double expo_focal(const struct hsdwl_expo *e);
static void expo_facet_ends(const struct hsdwl_expo *e, int desktop,
		struct expo_pt *a, struct expo_pt *b);
static int expo_view_desktop(const struct hsdwl_expo *e);
static bool expo_can_orbit(const struct hsdwl_expo *e);
static void expo_teardown(struct hsdwl_server *server);
static void expo_layout(struct hsdwl_expo *e);

/*
 * Ring geometry.  Everything the strip needs to know about the curve
 * lives here.  Input (expo_point) is the single place that converts
 * screen coordinates back into world space.
 */

static double expo_openness(const struct hsdwl_expo *e)
{
	double t = (e->zoom - 1.0) / (HSDWL_EXPO_ZOOM_FAR - 1.0);
	if (t < 0)
		return 0;
	if (t > 1)
		return 1;
	return t;
}

static double expo_gap(const struct hsdwl_expo *e)
{
	return HSDWL_EXPO_GAP_FRAC * e->sw * expo_openness(e);
}

static double expo_pitch(const struct hsdwl_expo *e)
{
	return e->sw + expo_gap(e);
}

static double expo_scale(const struct hsdwl_expo *e)
{
	return e->sw / (e->zoom * expo_pitch(e));
}

static double expo_camera_x(const struct hsdwl_expo *e)
{
	return (double)e->home * e->sw;
}

static double expo_center(const struct hsdwl_expo *e)
{
	return expo_camera_x(e) + e->sw / 2.0
		+ e->home * expo_gap(e) + e->pan;
}

static double expo_lap(const struct hsdwl_expo *e)
{
	/* seam is fixed at 1.0: the ring never closes in this port */
	return HSDWL_NUM_WORKSPACES * expo_pitch(e)
		+ HSDWL_EXPO_SEAM_PITCHES * expo_pitch(e);
}

static double expo_radius(const struct hsdwl_expo *e)
{
	if (!e->curved)
		return 0;
	double o = expo_openness(e);
	if (o < 0.02)
		return 0;
	return expo_lap(e) / (2 * M_PI) / o;
}

static double expo_dist(const struct hsdwl_expo *e)
{
	return HSDWL_EXPO_CAM_DIST * expo_pitch(e) * e->dist
		* (1 + HSDWL_EXPO_TILT_PULLBACK * fabs(e->tilt));
}

static double expo_focal(const struct hsdwl_expo *e)
{
	return expo_scale(e) * HSDWL_EXPO_CAM_DIST * expo_pitch(e);
}

static struct expo_pt expo_ring_point(const struct hsdwl_expo *e,
		double u, double wy)
{
	struct expo_pt p;
	double r = expo_radius(e);
	p.y = wy - e->sh / 2.0;
	if (r <= 0)
	{
		p.x = u;
		p.z = 0;
	}
	else
	{
		double phi = u / r;
		p.x = r * sin(phi);
		p.z = r * cos(phi) - r;
	}
	return p;
}

static struct expo_pt expo_facet_point(const struct hsdwl_expo *e,
		struct expo_pt a, struct expo_pt b, double s, double wy)
{
	struct expo_pt p;
	p.x = a.x + (b.x - a.x) * s;
	p.z = a.z + (b.z - a.z) * s;
	p.y = wy - e->sh / 2.0;
	return p;
}

static struct expo_vert expo_project(const struct hsdwl_expo *e,
		struct expo_pt p, double u, double v)
{
	struct expo_vert o;
	double ct = cos(e->tilt), st = sin(e->tilt);
	double r = expo_radius(e);
	double pz = p.z + r;
	double y = p.y * ct + pz * st;
	double z = -p.y * st + pz * ct;
	double w = expo_dist(e) + r - z;
	double f = expo_focal(e);
	double ww = fmax(w, 1);
	o.x = e->sw / 2.0 + f * p.x / ww;
	o.y = e->sh / 2.0 + f * y / ww;
	o.w = ww;
	o.u = u;
	o.v = v;
	return o;
}

static double expo_desktop_strip_x(const struct hsdwl_expo *e, int desktop)
{
	double x = desktop * expo_pitch(e);
	double lap = expo_lap(e);
	double centre = expo_center(e) - e->sw / 2.0;
	double half = lap / 2.0;
	double diff = x - centre;
	if (diff > half)
		diff -= lap;
	else if (diff < -half)
		diff += lap;
	return centre + diff;
}

static double expo_offset(const struct hsdwl_expo *e, double wx, int desktop)
{
	return expo_desktop_strip_x(e, desktop)
		+ (wx - desktop * e->sw) - expo_center(e);
}

static void expo_facet_ends(const struct hsdwl_expo *e, int desktop,
		struct expo_pt *a, struct expo_pt *b)
{
	*a = expo_ring_point(e,
		expo_offset(e, desktop * e->sw, desktop), 0);
	*b = expo_ring_point(e,
		expo_offset(e, (desktop + 1) * e->sw, desktop), 0);
}

static struct expo_vert expo_to_screen(const struct hsdwl_expo *e,
		double wx, double wy, int desktop)
{
	double s = (wx - desktop * e->sw) / e->sw;
	struct expo_pt a, b;
	expo_facet_ends(e, desktop, &a, &b);
	return expo_project(e, expo_facet_point(e, a, b, s, wy), 0, 0);
}

static void expo_ray(const struct hsdwl_expo *e, double sx, double sy,
		struct expo_pt *origin, struct expo_pt *dir)
{
	double f = expo_focal(e);
	double d = expo_dist(e);
	double ct = cos(e->tilt), st = sin(e->tilt);
	double r = expo_radius(e);
	double X = (sx - e->sw / 2.0) / f;
	double Y = (sy - e->sh / 2.0) / f;
	origin->x = 0;
	origin->y = -(d + r) * st;
	origin->z = (d + r) * ct - r;
	dir->x = X;
	dir->y = Y * ct + st;
	dir->z = Y * st - ct;
}

/* The one place screen coordinates become world coordinates. */
static bool expo_point(const struct hsdwl_expo *e, double sx, double sy,
		int *desktop, double *wx, double *wy)
{
	struct expo_pt o, dir;
	expo_ray(e, sx, sy, &o, &dir);
	bool found = false;
	double best_t = 0;
	int best_d = 0;
	for (int d = 0; d < HSDWL_NUM_WORKSPACES; d++)
	{
		struct expo_pt a, b;
		expo_facet_ends(e, d, &a, &b);
		/* outward normal, unnormalized (sign and t scale invariant) */
		double nx = -(b.z - a.z);
		double nz = (b.x - a.x);
		double denom = nx * dir.x + nz * dir.z;
		if (denom >= -1e-9)
			continue;  /* backface or edge-on */
		double t = (nx * (a.x - o.x) + nz * (a.z - o.z)) / denom;
		if (t <= 0 || (found && t >= best_t))
			continue;
		struct expo_pt h = {
			.x = o.x + dir.x * t,
			.y = o.y + dir.y * t,
			.z = o.z + dir.z * t,
		};
		double dx = b.x - a.x, dz = b.z - a.z;
		double len2 = dx * dx + dz * dz;
		double s = ((h.x - a.x) * dx + (h.z - a.z) * dz) / len2;
		if (s < 0 || s > 1)
			continue;
		found = true;
		best_t = t;
		best_d = d;
		if (wx)
			*wx = d * e->sw + s * e->sw;
		if (wy)
			*wy = o.y + dir.y * t + e->sh / 2.0;
	}
	if (desktop)
		*desktop = best_d;
	return found;
}

static int expo_view_desktop(const struct hsdwl_expo *e)
{
	double x = (expo_center(e) - e->sw / 2.0) / expo_pitch(e);
	int d = (int)lround(x);
	if (d < 0)
		d = 0;
	if (d >= HSDWL_NUM_WORKSPACES)
		d = HSDWL_NUM_WORKSPACES - 1;
	return d;
}

static double expo_mid_w(const struct hsdwl_expo *e, int d)
{
	struct expo_pt a, b;
	expo_facet_ends(e, d, &a, &b);
	struct expo_vert v = expo_project(e,
		expo_facet_point(e, a, b, 0.5, e->sh / 2.0), 0, 0);
	return v.w;
}

static void expo_clamp_pan(struct hsdwl_expo *e)
{
	double half = e->sw / (2.0 * expo_scale(e));
	double lo = -half - e->home * expo_pitch(e);
	double hi = (HSDWL_NUM_WORKSPACES - 1 - e->home)
		* expo_pitch(e) + half;
	if (e->pan_target < lo)
		e->pan_target = lo;
	if (e->pan_target > hi)
		e->pan_target = hi;
}

static bool expo_can_orbit(const struct hsdwl_expo *e)
{
	return e->curved && !e->leaving
		&& e->zoom_target
			> (HSDWL_EXPO_ZOOM_NEAR + HSDWL_EXPO_ZOOM_FAR) / 2.0;
}

static void expo_camera_home(struct hsdwl_expo *e)
{
	e->tilt_target = 0;
	e->dist_target = 1;
	e->orbiting = 0;
	e->spin = 0;
}

/*
 * Rasterizer.  The renderer only draws axis-aligned rects, so every
 * quad is subdivided into an nx by ny grid of cells and each cell is
 * drawn as a rect.  Cell corners are computed in facet parameter space
 * so the perspective is exact; with 16x16 cells per card the
 * rect-vs-quad error stays under a pixel.  Every cell rect is inflated
 * by one pixel to kill hairline seams between neighbours.
 */

enum expo_quad_corner
{
	QTL, QBL, QTR, QBR
};

static void expo_quad(const struct hsdwl_expo *e,
		struct expo_pt a, struct expo_pt b,
		double s0, double s1, double wy0, double wy1,
		double u0, double u1, double v0, double v1,
		struct expo_vert q[4])
{
	q[QTL] = expo_project(e, expo_facet_point(e, a, b, s0, wy0), u0, v0);
	q[QTR] = expo_project(e, expo_facet_point(e, a, b, s1, wy0), u1, v0);
	q[QBL] = expo_project(e, expo_facet_point(e, a, b, s0, wy1), u0, v1);
	q[QBR] = expo_project(e, expo_facet_point(e, a, b, s1, wy1), u1, v1);
}

static double expo_quad_area(const struct expo_vert q[4])
{
	/* shoelace over TL, TR, BR, BL; y is down, so a front-facing
	 * quad has positive area */
	double a = 0;
	a += q[QTL].x * q[QTR].y - q[QTR].x * q[QTL].y;
	a += q[QTR].x * q[QBR].y - q[QBR].x * q[QTR].y;
	a += q[QBR].x * q[QBL].y - q[QBL].x * q[QBR].y;
	a += q[QBL].x * q[QTL].y - q[QTL].x * q[QBL].y;
	return a / 2.0;
}

static bool expo_quad_front(const struct expo_vert q[4])
{
	return expo_quad_area(q) > 0;
}

static bool expo_quad_onscreen(const struct expo_vert q[4],
		int sw, int sh)
{
	double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
	for (int i = 0; i < 4; i++)
	{
		if (q[i].w <= 1)
			return false;
		x0 = fmin(x0, q[i].x);
		x1 = fmax(x1, q[i].x);
		y0 = fmin(y0, q[i].y);
		y1 = fmax(y1, q[i].y);
	}
	const double M = 8;
	if (x1 + M < 0 || x0 - M > sw || y1 + M < 0 || y0 - M > sh)
		return false;
	return true;
}

static struct expo_vert expo_blerp(const struct expo_vert q[4],
		double u, double v)
{
	struct expo_vert r;
	for (int f = 0; f < 5; f++)
	{
		const double *q0 = (const double *)&q[0] + f;
		const double *q1 = (const double *)&q[1] + f;
		const double *q2 = (const double *)&q[2] + f;
		const double *q3 = (const double *)&q[3] + f;
		double top = *q0 + (*q1 - *q0) * u;
		double bot = *q2 + (*q3 - *q2) * u;
		((double *)&r)[f] = top + (bot - top) * v;
	}
	return r;
}

static void expo_inflate(const struct expo_vert q[4], double m,
		struct expo_vert out[4])
{
	double cx = 0, cy = 0;
	for (int i = 0; i < 4; i++)
	{
		cx += q[i].x;
		cy += q[i].y;
	}
	cx /= 4;
	cy /= 4;
	for (int i = 0; i < 4; i++)
	{
		double dx = q[i].x - cx, dy = q[i].y - cy;
		double len = hypot(dx, dy);
		if (len < 1e-6)
		{
			out[i] = q[i];
			continue;
		}
		out[i] = q[i];
		out[i].x = cx + dx / len * (len + m);
		out[i].y = cy + dy / len * (len + m);
	}
}

/* Draw a facet quad as a grid of rects.  tex != NULL textures it,
 * otherwise color (non-premultiplied rgb) fills it. */
static void expo_draw_grid(struct hsdwl_expo *e,
		struct wlr_render_pass *pass,
		struct wlr_texture *tex, const float color[3],
		struct expo_pt a, struct expo_pt b,
		double s0, double s1, double wy0, double wy1,
		double u0, double u1, double v0, double v1,
		int nx, int ny, float alpha, bool cull_back)
{
	struct expo_vert q[4];
	expo_quad(e, a, b, s0, s1, wy0, wy1, u0, u1, v0, v1, q);
	if (!expo_quad_onscreen(q, e->sw, e->sh))
		return;
	if (cull_back && !expo_quad_front(q))
		return;

	for (int j = 0; j < ny; j++)
	{
		double wyj0 = wy0 + (wy1 - wy0) * j / ny;
		double wyj1 = wy0 + (wy1 - wy0) * (j + 1) / ny;
		double vj0 = v0 + (v1 - v0) * j / ny;
		double vj1 = v0 + (v1 - v0) * (j + 1) / ny;
		for (int i = 0; i < nx; i++)
		{
			double si0 = s0 + (s1 - s0) * i / nx;
			double si1 = s0 + (s1 - s0) * (i + 1) / nx;
			double ui0 = u0 + (u1 - u0) * i / nx;
			double ui1 = u0 + (u1 - u0) * (i + 1) / nx;
			struct expo_vert c[4];
			expo_quad(e, a, b, si0, si1, wyj0, wyj1,
				ui0, ui1, vj0, vj1, c);
			if (cull_back && !expo_quad_front(c))
				continue;
			if (c[QTL].w <= 1 || c[QTR].w <= 1
				|| c[QBL].w <= 1 || c[QBR].w <= 1)
				continue;
			int x0 = INT_MAX, y0 = INT_MAX;
			int x1 = INT_MIN, y1 = INT_MIN;
			for (int k = 0; k < 4; k++)
			{
				x0 = MIN(x0, (int)floor(c[k].x));
				y0 = MIN(y0, (int)floor(c[k].y));
				x1 = MAX(x1, (int)ceil(c[k].x));
				y1 = MAX(y1, (int)ceil(c[k].y));
			}
			x0--; y0--; x1++; y1++;  /* kill hairline seams */
			if (x1 < 0 || y1 < 0 || x0 > e->sw || y0 > e->sh)
				continue;
			struct wlr_box dst = {
				.x = x0, .y = y0,
				.width = x1 - x0, .height = y1 - y0,
			};
			if (tex)
			{
				struct wlr_fbox src = {
					.x = (float)(ui0 * tex->width),
					.y = (float)(vj0 * tex->height),
					.width = (float)((ui1 - ui0) * tex->width),
					.height = (float)((vj1 - vj0) * tex->height),
				};
				wlr_render_pass_add_texture(pass,
					&(struct wlr_render_texture_options){
						.texture = tex,
						.src_box = src,
						.dst_box = dst,
						.alpha = &alpha,
						.transform = WL_OUTPUT_TRANSFORM_NORMAL,
					});
			}
			else
			{
				float rr = color[0] * alpha;
				float gg = color[1] * alpha;
				float bb = color[2] * alpha;
				wlr_render_pass_add_rect(pass,
					&(struct wlr_render_rect_options){
						.box = dst,
						.color = { .r = rr, .g = gg, .b = bb, .a = alpha },
					});
			}
		}
	}
}

/* Solid fill of an arbitrary projected quad (bilinear in screen
 * space).  Used for the inflated edge/hilight frames where
 * perspective-correct subdivision does not matter. */
static void expo_draw_grid_solid(struct hsdwl_expo *e,
		struct wlr_render_pass *pass, const struct expo_vert q[4],
		const float color[3], float alpha, int nx, int ny)
{
	if (!expo_quad_onscreen(q, e->sw, e->sh))
		return;
	if (!expo_quad_front(q))
		return;
	for (int j = 0; j < ny; j++)
	{
		for (int i = 0; i < nx; i++)
		{
			double u0 = (double)i / nx, u1 = (double)(i + 1) / nx;
			double v0 = (double)j / ny, v1 = (double)(j + 1) / ny;
			struct expo_vert c[4] = {
				expo_blerp(q, u0, v0),
				expo_blerp(q, u1, v0),
				expo_blerp(q, u0, v1),
				expo_blerp(q, u1, v1),
			};
			int x0 = INT_MAX, y0 = INT_MAX;
			int x1 = INT_MIN, y1 = INT_MIN;
			for (int k = 0; k < 4; k++)
			{
				x0 = MIN(x0, (int)floor(c[k].x));
				y0 = MIN(y0, (int)floor(c[k].y));
				x1 = MAX(x1, (int)ceil(c[k].x));
				y1 = MAX(y1, (int)ceil(c[k].y));
			}
			x0--; y0--; x1++; y1++;
			if (x1 < 0 || y1 < 0 || x0 > e->sw || y0 > e->sh)
				continue;
			struct wlr_box dst = {
				.x = x0, .y = y0,
				.width = x1 - x0, .height = y1 - y0,
			};
			float rr = color[0] * alpha;
			float gg = color[1] * alpha;
			float bb = color[2] * alpha;
			wlr_render_pass_add_rect(pass,
				&(struct wlr_render_rect_options){
					.box = dst,
					.color = { .r = rr, .g = gg, .b = bb, .a = alpha },
				});
		}
	}
}

/*
 * Drawing.  The whole strip is composed into a full-screen canvas
 * buffer; the scene just shows that canvas, plus the backdrop that
 * fades in under it.
 */

static void expo_draw_card(struct hsdwl_expo *e,
		struct wlr_render_pass *pass, int d, int looking_at,
		double open)
{
	struct expo_pt a, b;
	expo_facet_ends(e, d, &a, &b);
	struct expo_vert q[4];
	expo_quad(e, a, b, 0, 1, 0, e->sh, 0, 1, 0, 1, q);
	if (!expo_quad_onscreen(q, e->sw, e->sh))
		return;
	if (!expo_quad_front(q))
		return;

	bool hilight = (d == e->hover_d);

	/* edge frame: an inflated solid under the base, so only the
	 * margin around the card shows.  Hover widens the accent. */
	double m = (hilight ? HSDWL_EXPO_HILIGHT_PX : HSDWL_EXPO_EDGE_PX)
		* open;
	if (m > 0.1)
	{
		struct expo_vert ei[4];
		expo_inflate(q, m, ei);
		float ec[3];
		if (d == looking_at || hilight)
		{
			ec[0] = e->server->config.border_color_focused[0];
			ec[1] = e->server->config.border_color_focused[1];
			ec[2] = e->server->config.border_color_focused[2];
		}
		else
		{
			ec[0] = ec[1] = ec[2] = HSDWL_EXPO_EDGE_GREY;
		}
		expo_draw_grid_solid(e, pass, ei, ec, 1.0f,
			HSDWL_EXPO_CARD_CELLS, HSDWL_EXPO_CARD_CELLS);
	}

	/* card base */
	float base[3] = {
		HSDWL_EXPO_CARD_GREY,
		HSDWL_EXPO_CARD_GREY,
		HSDWL_EXPO_CARD_GREY,
	};
	expo_draw_grid(e, pass, NULL, base, a, b,
		0, 1, 0, e->sh, 0, 1, 0, 1,
		HSDWL_EXPO_CARD_CELLS, HSDWL_EXPO_CARD_CELLS, 1.0f, true);

	/* workspace snapshot: a full composite of the desktop, so it
	 * includes the wallpaper and any bars that would be on screen */
	if (e->card_tex[d])
		expo_draw_grid(e, pass, e->card_tex[d], NULL, a, b,
			0, 1, 0, e->sh, 0, 1, 0, 1,
			HSDWL_EXPO_CARD_CELLS, HSDWL_EXPO_CARD_CELLS, 1.0f, true);
}

/* The far wall of a backfacing card: the backdrop picture, so the
 * ring reads as one continuous blurred wallpaper and the strip ends
 * don't show anything mirrored or stale. */
static void expo_draw_inner(struct hsdwl_expo *e,
		struct wlr_render_pass *pass, const struct expo_vert q[4],
		struct expo_pt a, struct expo_pt b)
{
	(void)q;
	if (e->bg_tex)
	{
		/* re-project the facet, no backface culling */
		expo_draw_grid(e, pass, e->bg_tex, NULL, a, b,
			0, 1, 0, e->sh, 0, 1, 0, 1,
			HSDWL_EXPO_CARD_CELLS, HSDWL_EXPO_CARD_CELLS, 1.0f, false);
	}
}

static bool expo_canvas_current(struct hsdwl_expo *e)
{
	if (e->canvas_dirty)
		return false;
	if (e->drawn_zoom != e->zoom || e->drawn_pan != e->pan
		|| e->drawn_tilt != e->tilt || e->drawn_dist != e->dist)
		return false;
	if (e->drawn_generation != e->generation)
		return false;
	if (e->drawn_hover_d != e->hover_d)
		return false;
	return true;
}

static void expo_cover_draw(struct wlr_render_pass *pass,
		struct wlr_texture *tex, int w, int h, double zoom);

static void expo_draw_canvas(struct hsdwl_expo *e)
{
	if (expo_canvas_current(e))
		return;
	struct hsdwl_server *server = e->server;
	int idx = e->canvas_i ^ 1;
	struct wlr_buffer *buf = e->canvas_buf[idx];
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, buf, NULL);
	if (!pass)
		return;

	if (e->bg_tex)
		expo_cover_draw(pass, e->bg_tex, e->sw, e->sh, 1.0);
	else
		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
			.box = { .x = 0, .y = 0, .width = e->sw, .height = e->sh },
			.color = { .r = 0, .g = 0, .b = 0, .a = 1 },
		});

	/* depth order: far to near by mid-point depth */
	int order[HSDWL_NUM_WORKSPACES];
	double midw[HSDWL_NUM_WORKSPACES];
	for (int i = 0; i < HSDWL_NUM_WORKSPACES; i++)
	{
		order[i] = i;
		midw[i] = expo_mid_w(e, i);
	}
	for (int i = 1; i < HSDWL_NUM_WORKSPACES; i++)
	{
		double wi = midw[order[i]];
		int j = i - 1;
		while (j >= 0 && midw[order[j]] < wi)
		{
			int t = order[j];
			order[j] = order[j + 1];
			order[j + 1] = t;
			j--;
		}
	}

	double open = expo_openness(e);
	int looking_at = expo_view_desktop(e);

	/* far wall inners, backfaces only */
	for (int k = 0; k < HSDWL_NUM_WORKSPACES; k++)
	{
		int d = order[k];
		struct expo_pt a, b;
		expo_facet_ends(e, d, &a, &b);
		struct expo_vert q[4];
		expo_quad(e, a, b, 0, 1, 0, e->sh, 0, 1, 0, 1, q);
		if (!expo_quad_onscreen(q, e->sw, e->sh))
			continue;
		if (expo_quad_front(q))
			continue;
		expo_draw_inner(e, pass, q, a, b);
	}

	/* cards far to near */
	for (int k = 0; k < HSDWL_NUM_WORKSPACES; k++)
	{
		int d = order[k];
		expo_draw_card(e, pass, d, looking_at, open);
	}

	wlr_render_pass_submit(pass);
	wlr_scene_buffer_set_buffer_with_damage(e->canvas, buf, NULL);
	e->canvas_i = idx;
	e->drawn_zoom = e->zoom;
	e->drawn_pan = e->pan;
	e->drawn_tilt = e->tilt;
	e->drawn_dist = e->dist;
	e->drawn_generation = e->generation;
	e->drawn_hover_d = e->hover_d;
	e->canvas_dirty = false;
}

static void expo_layout(struct hsdwl_expo *e)
{
	/* the backdrop is an opaque picture now; no fade needed — the
	 * front card covers the screen exactly at zoom 1.0, so the
	 * backdrop only shows where the strip doesn't reach */
	expo_draw_canvas(e);
}

/*
 * Capture.  Window snapshots are scaled-down captures of the full
 * window (titlebar + border + content); the wallpaper is a single
 * composite of the background/bottom layer surfaces.
 */

static struct wlr_buffer *expo_alloc_buffer(struct hsdwl_server *server,
		int w, int h)
{
	if (w < 1 || h < 1)
		return NULL;
	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format fmt = {
		.format = DRM_FORMAT_ARGB8888,
		.len = 1,
		.modifiers = mods,
	};
	return wlr_allocator_create_buffer(server->allocator, w, h, &fmt);
}

/*
 * Capture.  Each card is an offscreen render of everything a desktop
 * would show, at full resolution: the background and bottom layers
 * (wallpaper, docks), the workspace itself (windows, stage canvas,
 * sidebar), and the top and overlay layers (bars, panels).  The
 * wallpaper composite kept for the backs of the cards comes from
 * expo_capture_wallpaper above; the strip backdrop is a blurred and
 * darkened version of it (expo_build_backdrop).
 */

struct expo_capture_ctx
{
	struct hsdwl_expo *e;
	struct wlr_render_pass *pass;
	int buffers;
};

static void expo_capture_buffer(struct wlr_scene_buffer *sb,
		int sx, int sy, void *data)
{
	struct expo_capture_ctx *ctx = data;
	struct hsdwl_expo *e = ctx->e;
	if (!sb->buffer)
		return;
	/* the scene's own cached texture when it has one (avoids an
	 * upload per capture); fall back to creating one on demand */
	struct wlr_texture *tex = sb->WLR_PRIVATE.texture;
	bool owned = false;
	if (!tex)
	{
		tex = wlr_texture_from_buffer(e->server->renderer, sb->buffer);
		owned = true;
	}
	if (!tex)
		return;
	int x = (int)lround(sx * HSDWL_EXPO_SNAP_SCALE);
	int y = (int)lround(sy * HSDWL_EXPO_SNAP_SCALE);
	int w = MAX((int)lround(sb->dst_width * HSDWL_EXPO_SNAP_SCALE), 1);
	int h = MAX((int)lround(sb->dst_height * HSDWL_EXPO_SNAP_SCALE), 1);
	int bw = e->sw, bh = e->sh;
	if (x + w > bw)
		w = bw - x;
	if (y + h > bh)
		h = bh - y;
	if (w < 1 || h < 1)
	{
		if (owned)
			wlr_texture_destroy(tex);
		return;
	}
	float alpha = 1.0f;
	wlr_render_pass_add_texture(ctx->pass,
		&(struct wlr_render_texture_options){
			.texture = tex,
			.dst_box = { .x = x, .y = y, .width = w, .height = h },
			.alpha = &alpha,
			.transform = WL_OUTPUT_TRANSFORM_NORMAL,
		});
	ctx->buffers++;
	if (owned)
		wlr_texture_destroy(tex);
}

/* Composite the wallpaper layers (background + bottom, which covers
 * both the built-in wallpaper and external layer-shell clients) into
 * one full-screen texture. */
static struct wlr_texture *expo_capture_wallpaper(struct hsdwl_expo *e)
{
	struct hsdwl_server *server = e->server;
	struct wlr_buffer *buf = expo_alloc_buffer(server, e->sw, e->sh);
	if (!buf)
		return NULL;
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, buf, NULL);
	if (!pass)
	{
		wlr_buffer_drop(buf);
		return NULL;
	}
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .x = 0, .y = 0, .width = e->sw, .height = e->sh },
		.color = { .r = 0, .g = 0, .b = 0, .a = 0 },
	});
	struct expo_capture_ctx ctx = { .e = e, .pass = pass };
	for (int i = 0; i < 2; i++)
		wlr_scene_node_for_each_buffer(&server->layer_trees[i]->node,
			expo_capture_buffer, &ctx);
	wlr_render_pass_submit(pass);
	struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buf);
	wlr_buffer_drop(buf);
	return tex;
}

/* Draw `tex` cover-fit (with an extra zoom) into the pass, centered
 * in a w×h area, cropping the overflow. */
static void expo_cover_draw(struct wlr_render_pass *pass,
		struct wlr_texture *tex, int w, int h, double zoom)
{
	double scale = fmax((double)w / tex->width,
		(double)h / tex->height) * zoom;
	int dw = (int)lround(tex->width * scale);
	int dh = (int)lround(tex->height * scale);
	float alpha = 1.0f;
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = tex,
		.dst_box = { .x = (w - dw) / 2, .y = (h - dh) / 2,
			.width = dw, .height = dh },
		.alpha = &alpha,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
	});
}

/* Build the strip's backdrop: the wallpaper, zoomed in, darkened, and
 * blurred by downsampling to an eighth and stretching back up.  The
 * result is opaque, so nothing behind the strip (like the previous
 * frame) can leak through.  With no wallpaper at all it is just a
 * dark plane.  On success stores the buffer in *out_buf and returns
 * a texture of it; the caller gives the buffer to a scene node. */
static struct wlr_texture *expo_build_backdrop(struct hsdwl_expo *e,
		struct wlr_buffer **out_buf)
{
	struct hsdwl_server *server = e->server;
	int bw = MAX(e->sw / 8, 1);
	int bh = MAX(e->sh / 8, 1);

	struct wlr_buffer *small = NULL;
	if (e->wp_tex)
	{
		small = expo_alloc_buffer(server, bw, bh);
		if (!small)
			return NULL;
		struct wlr_render_pass *p = wlr_renderer_begin_buffer_pass(
			server->renderer, small, NULL);
		if (!p)
		{
			wlr_buffer_drop(small);
			return NULL;
		}
		wlr_render_pass_add_rect(p, &(struct wlr_render_rect_options){
			.box = { .x = 0, .y = 0, .width = bw, .height = bh },
			.color = { .r = 0, .g = 0, .b = 0, .a = 0 },
		});
		expo_cover_draw(p, e->wp_tex, bw, bh, 1.2);
		wlr_render_pass_submit(p);
	}

	struct wlr_buffer *big = expo_alloc_buffer(server, e->sw, e->sh);
	if (!big)
	{
		if (small)
			wlr_buffer_drop(small);
		return NULL;
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, big, NULL);
	if (!pass)
	{
		if (small)
			wlr_buffer_drop(small);
		wlr_buffer_drop(big);
		return NULL;
	}
	if (small)
	{
		struct wlr_texture *s_tex = wlr_texture_from_buffer(
			server->renderer, small);
		wlr_buffer_drop(small);
		if (!s_tex)
		{
			wlr_render_pass_submit(pass);
			wlr_buffer_drop(big);
			return NULL;
		}
		/* stretching the eighth-size picture back up blurs it */
		expo_cover_draw(pass, s_tex, e->sw, e->sh, 1.0);
		wlr_texture_destroy(s_tex);
	}
	/* darken the picture; with no picture at all the plane is
	 * opaque black, so nothing stale shows through the strip */
	float dark = small ? (float)HSDWL_EXPO_BACKDROP_DARK : 1.0f;
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .x = 0, .y = 0, .width = e->sw, .height = e->sh },
		.color = { .r = 0, .g = 0, .b = 0, .a = dark },
	});
	wlr_render_pass_submit(pass);

	struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, big);
	if (!tex)
	{
		wlr_buffer_drop(big);
		return NULL;
	}
	*out_buf = big;
	return tex;
}

/* Render desktop d into its card buffer and rebuild its texture.
 * Everything that would be on screen for that desktop is composited:
 * wallpaper layers, the desktop's own tree, and the bars on top.
 * The hidden trees are temporarily re-enabled so they still capture;
 * nothing renders mid-capture (single event loop). */
static bool expo_capture_desktop(struct hsdwl_expo *e, int d)
{
	struct hsdwl_server *server = e->server;
	struct wlr_buffer *buf = e->card_buf[d];
	if (!buf)
		return false;
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, buf, NULL);
	if (!pass)
		return false;
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .x = 0, .y = 0, .width = e->sw, .height = e->sh },
		.color = { .r = 0, .g = 0, .b = 0, .a = 0 },
	});
	struct expo_capture_ctx ctx = { .e = e, .pass = pass };
	/* wallpaper and docks, shared by every desktop */
	for (int i = 0; i < 2; i++)
	{
		wlr_scene_node_set_enabled(&server->layer_trees[i]->node, true);
		wlr_scene_node_for_each_buffer(&server->layer_trees[i]->node,
			expo_capture_buffer, &ctx);
	}
	/* the desktop itself */
	struct wlr_scene_node *node = &server->workspaces[d]->node;
	wlr_scene_node_set_enabled(node, true);
	wlr_scene_node_for_each_buffer(node, expo_capture_buffer, &ctx);
	wlr_scene_node_set_enabled(node, false);
	/* bars and overlays */
	for (int i = 2; i < 4; i++)
		wlr_scene_node_for_each_buffer(&server->layer_trees[i]->node,
			expo_capture_buffer, &ctx);
	for (int i = 0; i < 2; i++)
		wlr_scene_node_set_enabled(&server->layer_trees[i]->node, false);
	if (!wlr_render_pass_submit(pass))
		return false;
	wlr_log(WLR_DEBUG, "expo: desktop %d: %d buffers captured",
		d, ctx.buffers);
	struct wlr_texture *tex =
		wlr_texture_from_buffer(server->renderer, buf);
	if (!tex)
		return false;
	if (e->card_tex[d])
		wlr_texture_destroy(e->card_tex[d]);
	e->card_tex[d] = tex;
	e->generation++;
	return true;
}

/* Refresh cadence while open: the front card at ~10Hz, all cards
 * every second.  Every capture bumps the generation so the canvas
 * cache redraws with the fresh texture. */

struct expo_frame_done_data
{
	struct timespec when;
};

static void expo_send_frame_done(struct wlr_scene_buffer *sb,
		int sx, int sy, void *data)
{
	(void)sx;
	(void)sy;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
	if (ss)
		wlr_surface_send_frame_done(ss->surface, data);
}

static void expo_refresh_cards(struct hsdwl_expo *e,
		const struct timespec *now)
{
	double front_dt = (now->tv_sec - e->last_front_refresh.tv_sec)
		+ (now->tv_nsec - e->last_front_refresh.tv_nsec) / 1e9;
	double all_dt = (now->tv_sec - e->last_all_refresh.tv_sec)
		+ (now->tv_nsec - e->last_all_refresh.tv_nsec) / 1e9;
	int front = expo_view_desktop(e);
	if (all_dt >= HSDWL_EXPO_ALL_REFRESH_S)
	{
		for (int d = 0; d < HSDWL_NUM_WORKSPACES; d++)
		{
			if (d != front)
				expo_capture_desktop(e, d);
		}
		e->last_all_refresh = *now;
	}
	if (front_dt >= HSDWL_EXPO_FRONT_REFRESH_S)
	{
		/* keep the front desktop's clients animating: they get no
		 * frame_done while hidden, so nudge them before the shot */
		struct expo_frame_done_data data = { .when = *now };
		wlr_scene_node_for_each_buffer(
			&e->server->workspaces[front]->node,
			expo_send_frame_done, &data);
		expo_capture_desktop(e, front);
		e->last_front_refresh = *now;
	}
}



/*
 * State machine: open, close (animated collapse), teardown.
 */

static void expo_set_world_visible(struct hsdwl_expo *e, bool visible)
{
	struct hsdwl_server *server = e->server;
	for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
		wlr_scene_node_set_enabled(&server->workspaces[i]->node,
			visible && i == server->current_workspace);
	for (int i = 0; i < 2; i++)
		wlr_scene_node_set_enabled(&server->layer_trees[i]->node,
			visible);
}

static void expo_teardown(struct hsdwl_server *server)
{
	struct hsdwl_expo *e = server->expo;
	if (!e)
		return;
	server->expo = NULL;

	for (int i = 0; i < HSDWL_NUM_WORKSPACES; i++)
	{
		if (e->card_tex[i])
			wlr_texture_destroy(e->card_tex[i]);
		if (e->card_buf[i])
			wlr_buffer_unlock(e->card_buf[i]);
	}
	if (e->wp_tex)
		wlr_texture_destroy(e->wp_tex);
	if (e->bg_tex)
		wlr_texture_destroy(e->bg_tex);
	for (int i = 0; i < 2; i++)
	{
		if (e->canvas_buf[i])
			wlr_buffer_unlock(e->canvas_buf[i]);
	}
	wlr_scene_node_destroy(&e->tree->node);
	expo_set_world_visible(e, true);
	free(e);
}

static void expo_selftest(struct hsdwl_expo *e)
{
	if (!getenv("HSDWL_EXPO_TEST"))
		return;
	int tested = 0, hidden = 0;
	double worst = 0;
	for (int d = 0; d < HSDWL_NUM_WORKSPACES; d++)
	{
		for (int gy = 0; gy < 5; gy++)
		{
			for (int gx = 0; gx < 5; gx++)
			{
				double wx = d * e->sw + e->sw * gx / 4.0;
				double wy = e->sh * gy / 4.0;
				struct expo_vert v = expo_to_screen(e, wx, wy, d);
				if (v.w <= 1 || v.x < -8 || v.x > e->sw + 8
					|| v.y < -8 || v.y > e->sh + 8)
				{
					hidden++;
					continue;
				}
				tested++;
				int rd;
				double rx, ry;
				if (!expo_point(e, v.x, v.y, &rd, &rx, &ry))
				{
					wlr_log(WLR_ERROR,
						"expo: ray miss for world %g,%g on %d at screen %g,%g",
						wx, wy, d, v.x, v.y);
					continue;
				}
				if (rd != d)
				{
					wlr_log(WLR_ERROR,
						"expo: round trip %d -> %d at %g,%g",
						d, rd, wx, wy);
				}
				double err = hypot(rx - wx, ry - wy);
				if (err > worst)
					worst = err;
			}
		}
	}
	wlr_log(WLR_INFO,
		"expo: ray round-trip over %d points: worst %.3f px, %d hidden",
		tested, worst, hidden);
}

bool expo_open(struct hsdwl_server *server)
{
	if (server->expo)
		return false;
	if (!server->config.expo_enabled)
	{
		wlr_log(WLR_INFO, "%s", "expo: open refused (disabled in config)");
		return false;
	}
	struct wlr_output *out =
		wlr_output_layout_get_center_output(server->output_layout);
	if (!out)
	{
		wlr_log(WLR_INFO, "%s", "expo: open refused (no output)");
		return false;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, out, &box);
	int sw = box.width;
	int sh = box.height;
	if (sw <= 0 || sh <= 0)
	{
		wlr_log(WLR_INFO, "expo: open refused (bad output size %dx%d)",
			sw, sh);
		return false;
	}

	struct hsdwl_expo *e = calloc(1, sizeof(*e));
	if (!e)
		return false;
	e->server = server;
	e->out = out;
	e->sw = sw;
	e->sh = sh;
	e->ox = box.x;
	e->oy = box.y;
	e->zoom = 1.0;
	e->zoom_target = HSDWL_EXPO_ZOOM_NEAR;
	e->curved = server->config.stage_3d_enabled;
	e->dist = 1.0;
	e->home = (int)server->current_workspace;
	e->canvas_dirty = true;

	const char *test = getenv("HSDWL_EXPO_TEST");
	if (test)
	{
		e->zoom = HSDWL_EXPO_ZOOM_FAR;
		e->zoom_target = HSDWL_EXPO_ZOOM_FAR;
		e->tilt = atof(test);
		e->tilt_target = e->tilt;
		const char *dstr = getenv("HSDWL_EXPO_TEST_DIST");
		if (dstr)
		{
			e->dist = atof(dstr);
			e->dist_target = e->dist;
		}
	}

	e->tree = wlr_scene_tree_create(&server->scene->tree);
	if (!e->tree)
	{
		free(e);
		return false;
	}
	wlr_scene_node_set_position(&e->tree->node, e->ox, e->oy);
	wlr_scene_node_place_below(&e->tree->node,
		&server->layer_trees[2]->node);

	/* hand the pointer to the server now, so failures can teardown */
	server->expo = e;

	e->canvas = wlr_scene_buffer_create(e->tree, NULL);
	if (!e->canvas)
	{
		expo_teardown(server);
		return false;
	}
	wlr_scene_buffer_set_dest_size(e->canvas, e->sw, e->sh);
	for (int i = 0; i < 2; i++)
	{
		struct wlr_buffer *buf = expo_alloc_buffer(server, e->sw, e->sh);
		if (!buf)
		{
			expo_teardown(server);
			return false;
		}
		e->canvas_buf[i] = wlr_buffer_lock(buf);
		wlr_buffer_drop(buf);
	}

	e->wp_tex = expo_capture_wallpaper(e);

	/* backdrop: an opaque blurred/darkened wallpaper picture; with no
	 * wallpaper it is a plain dark plane.  Either way nothing behind
	 * the strip can leak through. */
	e->bg_tex = expo_build_backdrop(e, &e->bg_buf);
	e->backdrop = wlr_scene_buffer_create(e->tree, e->bg_buf);
	if (e->bg_buf)
		wlr_buffer_drop(e->bg_buf);
	e->bg_buf = NULL;  /* the scene holds it now */
	if (!e->backdrop)
	{
		expo_teardown(server);
		return false;
	}
	wlr_scene_buffer_set_dest_size(e->backdrop, e->sw, e->sh);
	/* the backdrop must sit under the canvas; it was created after
	 * it (the wallpaper had to be captured first) */
	wlr_scene_node_lower_to_bottom(&e->backdrop->node);

	/* card buffers, one full-resolution snapshot per desktop */
	for (int d = 0; d < HSDWL_NUM_WORKSPACES; d++)
	{
		struct wlr_buffer *buf = expo_alloc_buffer(server,
			e->sw, e->sh);
		if (!buf)
			continue;
		e->card_buf[d] = wlr_buffer_lock(buf);
		wlr_buffer_drop(buf);
	}
	for (int d = 0; d < HSDWL_NUM_WORKSPACES; d++)
		expo_capture_desktop(e, d);

	wlr_log(WLR_INFO, "expo: open %dx%d curved=%d (%d desktops)",
		sw, sh, e->curved, HSDWL_NUM_WORKSPACES);

	expo_set_world_visible(e, false);
	expo_layout(e);
	expo_selftest(e);

	wlr_seat_pointer_notify_clear_focus(server->seat);
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	wlr_output_schedule_frame(e->out);
	return true;
}

void expo_close(struct hsdwl_server *server, int desktop)
{
	struct hsdwl_expo *e = server->expo;
	if (!e || e->leaving)
		return;
	if (desktop < 0)
		desktop = 0;
	if (desktop >= HSDWL_NUM_WORKSPACES)
		desktop = HSDWL_NUM_WORKSPACES - 1;

	e->leaving = true;
	e->zoom_target = 1.0;
	e->hover_d = -1;
	double was = expo_center(e);
	e->home = desktop;
	/* position the strip so the target desktop lands centered and
	 * the camera motion stays continuous */
	e->pan = was - (e->home * e->sw + e->sw / 2.0
		+ e->home * expo_gap(e));
	e->pan_target = 0;
	e->spin = 0;
	expo_camera_home(e);

	/* reveal the target workspace behind the collapsing backdrop */
	if (desktop != (int)server->current_workspace)
		hsdwl_server_switch_workspace(server, (size_t)desktop);
	else
	{
		for (size_t i = 0; i < HSDWL_NUM_WORKSPACES; i++)
			wlr_scene_node_set_enabled(&server->workspaces[i]->node,
				i == (size_t)desktop);
	}
	wlr_output_schedule_frame(e->out);
}

void expo_toggle(struct hsdwl_server *server)
{
	if (!server->expo)
	{
		expo_open(server);
		return;
	}
	struct hsdwl_expo *e = server->expo;
	if (e->leaving)
	{
		e->leaving = false;
		e->zoom_target = HSDWL_EXPO_ZOOM_NEAR;
		wlr_output_schedule_frame(e->out);
		return;
	}
	expo_close(server, expo_view_desktop(e));
}

static void expo_zoom_step(struct hsdwl_expo *e)
{
	double mid = (HSDWL_EXPO_ZOOM_NEAR + HSDWL_EXPO_ZOOM_FAR) / 2.0;
	e->zoom_target = (e->zoom_target > mid)
		? HSDWL_EXPO_ZOOM_NEAR : HSDWL_EXPO_ZOOM_FAR;
	wlr_output_schedule_frame(e->out);
}

void expo_tick(struct hsdwl_server *server, const struct timespec *now)
{
	struct hsdwl_expo *e = server->expo;
	if (!e)
		return;

	double dt = 1.0 / 60.0;
	if (e->have_last_tick)
	{
		dt = (now->tv_sec - e->last_tick.tv_sec)
			+ (now->tv_nsec - e->last_tick.tv_nsec) / 1e9;
		if (dt < 0)
			dt = 0;
		if (dt > 1.0 / 60.0)
			dt = 1.0 / 60.0;
	}
	e->last_tick = *now;
	e->have_last_tick = true;

	/* zoom */
	double gz = e->zoom_target - e->zoom;
	if (fabs(gz) < 0.0005)
		e->zoom = e->zoom_target;
	else
		e->zoom += gz * (1 - exp(-HSDWL_EXPO_ZOOM_SPEED * dt));

	/* pan */
	if (e->spin != 0)
	{
		double before = e->pan_target;
		e->pan_target += e->spin * dt;
		expo_clamp_pan(e);
		if (e->pan_target != before)
			e->spin = 0;
		double decay = exp(-HSDWL_EXPO_SPIN_DECAY * dt);
		e->spin *= decay;
		if (fabs(e->spin) < HSDWL_EXPO_SPIN_MIN)
			e->spin = 0;
		e->pan = e->pan_target;
	}
	else
	{
		double gp = e->pan_target - e->pan;
		if (fabs(gp) < 0.5)
			e->pan = e->pan_target;
		else
			e->pan += gp * (1 - exp(-HSDWL_EXPO_PAN_SPEED * dt));
	}

	if (!e->leaving)
	{
		e->clock += dt;
		expo_refresh_cards(e, now);
	}

	if (!expo_can_orbit(e))
	{
		e->tilt_target = 0;
		e->dist_target = 1;
		e->orbiting = 0;
	}

	bool homing = (e->tilt_target == 0 && e->dist_target == 1);
	double ospeed = homing ? HSDWL_EXPO_ORBIT_HOME_SPEED : HSDWL_EXPO_ORBIT_SPEED;
	double gt = e->tilt_target - e->tilt;
	if (fabs(gt) < 0.0005)
		e->tilt = e->tilt_target;
	else
		e->tilt += gt * (1 - exp(-ospeed * dt));
	double gd = e->dist_target - e->dist;
	if (fabs(gd) < 0.0005)
		e->dist = e->dist_target;
	else
		e->dist += gd * (1 - exp(-ospeed * dt));

	/* collapse when the camera reaches home */
	if (e->leaving && e->zoom <= 1.0005
		&& fabs(e->tilt) < 0.002 && fabs(e->dist - 1) < 0.002)
	{
		expo_teardown(server);
		return;
	}

	expo_layout(e);
}

bool expo_needs_frame(struct hsdwl_server *server)
{
	return server->expo != NULL;
}

/*
 * Input.  Every handler returns true when the strip consumed the
 * event; while the strip is open it consumes everything.
 */

static void expo_pan_by(struct hsdwl_expo *e, double strip_px)
{
	e->pan_target += strip_px;
	expo_clamp_pan(e);
	wlr_output_schedule_frame(e->out);
}


bool expo_handle_key(struct hsdwl_expo *e, struct wlr_keyboard *kb,
		struct wlr_keyboard_key_event *event)
{
	if (!e)
		return false;
	if (event->state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return true;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(kb->xkb_state,
		event->keycode + 8);
	switch (sym)
	{
	case XKB_KEY_Escape:
		expo_toggle(e->server);
		break;
	case XKB_KEY_z:
	case XKB_KEY_Z:
		expo_zoom_step(e);
		break;
	case XKB_KEY_Left:
	case XKB_KEY_h:
		expo_pan_by(e, -expo_pitch(e));
		break;
	case XKB_KEY_Right:
	case XKB_KEY_l:
		expo_pan_by(e, expo_pitch(e));
		break;
	case XKB_KEY_Up:
	case XKB_KEY_k:
		if (expo_can_orbit(e))
		{
			e->tilt_target += HSDWL_EXPO_TILT_STEP;
			if (e->tilt_target > HSDWL_EXPO_TILT_MAX)
				e->tilt_target = HSDWL_EXPO_TILT_MAX;
			wlr_output_schedule_frame(e->out);
		}
		break;
	case XKB_KEY_Down:
	case XKB_KEY_j:
		if (expo_can_orbit(e))
		{
			e->tilt_target -= HSDWL_EXPO_TILT_STEP;
			if (e->tilt_target < -HSDWL_EXPO_TILT_MAX)
				e->tilt_target = -HSDWL_EXPO_TILT_MAX;
			wlr_output_schedule_frame(e->out);
		}
		break;
	case XKB_KEY_Page_Up:
		if (expo_can_orbit(e))
		{
			e->dist_target /= HSDWL_EXPO_DIST_STEP;
			if (e->dist_target < HSDWL_EXPO_DIST_MIN)
				e->dist_target = HSDWL_EXPO_DIST_MIN;
			wlr_output_schedule_frame(e->out);
		}
		break;
	case XKB_KEY_Page_Down:
		if (expo_can_orbit(e))
		{
			e->dist_target *= HSDWL_EXPO_DIST_STEP;
			if (e->dist_target > HSDWL_EXPO_DIST_MAX)
				e->dist_target = HSDWL_EXPO_DIST_MAX;
			wlr_output_schedule_frame(e->out);
		}
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		expo_close(e->server, expo_view_desktop(e));
		break;
	default:
		break;
	}
	return true;
}

bool expo_handle_motion(struct hsdwl_expo *e, double gx, double gy)
{
	if (!e)
		return false;
	double lx = gx - e->ox;
	double ly = gy - e->oy;
	e->lx = lx;
	e->ly = ly;
	if (e->leaving)
		return true;
	if (e->orbiting)
	{
		double dx = lx - e->orbit_x;
		double dy = ly - e->orbit_y;
		e->orbit_x = lx;
		e->orbit_y = ly;
		double step = hypot(dx, dy);
		if (step > 0)
		{
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double dt = (now.tv_sec - e->orbit_time.tv_sec)
				+ (now.tv_nsec - e->orbit_time.tv_nsec) / 1e9;
			if (dt > 0)
				e->spin = e->spin * 0.6 + (step / dt) * 0.4;
			e->orbit_time = now;
		}
		e->pan -= dx / expo_scale(e);
		expo_clamp_pan(e);
		e->pan_target = e->pan;
		e->tilt_target += dy * HSDWL_EXPO_TILT_STEP / 40.0;
		if (e->tilt_target > HSDWL_EXPO_TILT_MAX)
			e->tilt_target = HSDWL_EXPO_TILT_MAX;
		if (e->tilt_target < -HSDWL_EXPO_TILT_MAX)
			e->tilt_target = -HSDWL_EXPO_TILT_MAX;
		expo_layout(e);
		wlr_output_schedule_frame(e->out);
		return true;
	}
	/* hover: the desktop under the cursor, for the card hilight */
	int d;
	double wx, wy;
	if (expo_point(e, lx, ly, &d, &wx, &wy))
	{
		if (d != e->hover_d)
		{
			e->hover_d = d;
			expo_layout(e);
			wlr_output_schedule_frame(e->out);
		}
	}
	else if (e->hover_d != -1)
	{
		e->hover_d = -1;
		expo_layout(e);
		wlr_output_schedule_frame(e->out);
	}
	return true;
}

bool expo_handle_button(struct hsdwl_expo *e,
		struct wlr_pointer_button_event *event)
{
	if (!e)
		return false;
	if (e->leaving)
		return true;

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED)
	{
		if (e->orbiting && event->button == BTN_MIDDLE)
			e->orbiting = 0;
		return true;
	}

	/* pressed */
	if (event->button == BTN_MIDDLE)
	{
		if (expo_can_orbit(e))
		{
			e->orbiting = 1;
			e->orbit_x = e->lx;
			e->orbit_y = e->ly;
			clock_gettime(CLOCK_MONOTONIC, &e->orbit_time);
			e->spin = 0;
		}
		return true;
	}
	if (event->button != BTN_LEFT)
		return true;

	/* plain left click: collapse into the desktop under the cursor */
	int d;
	double wx, wy;
	if (expo_point(e, e->lx, e->ly, &d, &wx, &wy))
		expo_close(e->server, d);
	return true;
}

bool expo_handle_axis(struct hsdwl_expo *e,
		struct wlr_pointer_axis_event *event)
{
	if (!e)
		return false;
	if (event->orientation != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return true;
	if (e->leaving)
		return true;
	if (e->orbiting)
	{
		if (event->delta > 0)
		{
			e->dist_target *= HSDWL_EXPO_DIST_STEP;
			if (e->dist_target > HSDWL_EXPO_DIST_MAX)
				e->dist_target = HSDWL_EXPO_DIST_MAX;
		}
		else if (event->delta < 0)
		{
			e->dist_target /= HSDWL_EXPO_DIST_STEP;
			if (e->dist_target < HSDWL_EXPO_DIST_MIN)
				e->dist_target = HSDWL_EXPO_DIST_MIN;
		}
		wlr_output_schedule_frame(e->out);
		return true;
	}
	expo_pan_by(e, event->delta * expo_pitch(e) / 45.0);
	return true;
}

void expo_output_gone(struct hsdwl_server *server, struct wlr_output *out)
{
	struct hsdwl_expo *e = server->expo;
	if (!e || e->out != out)
		return;
	expo_teardown(server);
}

void expo_destroy(struct hsdwl_server *server)
{
	if (server->expo)
		expo_teardown(server);
}
