/*
 * hsdwl — a Wayland compositor
 *
 * Built-in wallpaper: the picture from config (background_wallpaper)
 * shown on every desktop, under everything else.  The image is
 * decoded at startup (most formats via stb_image), uploaded once, and
 * laid out cover-fit into the output as a scene buffer in the
 * background layer, so the expo cards pick it up automatically.
 */

#include "wallpaper.h"
#include "config.h"
#include "server.h"
#include "stb_image.h"

#define WLR_USE_UNSTABLE

#include <drm_fourcc.h>
#include <math.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

static struct wlr_buffer *wallpaper_alloc(struct hsdwl_server *server,
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

void hsdwl_wallpaper_load(struct hsdwl_server *server)
{
	const char *path = server->config.background_wallpaper;
	if (!path[0])
		return;

	/* stb_image decodes most formats (PNG, JPEG, BMP, GIF, TGA,
	 * PSD, PNM, HDR) into tightly-packed straight-alpha RGBA. */
	int w = 0, h = 0, ch = 0;
	uint8_t *data = stbi_load(path, &w, &h, &ch, 4);
	if (!data)
	{
		wlr_log(WLR_ERROR, "wallpaper: cannot load '%s': %s",
			path, stbi_failure_reason());
		return;
	}
	if (w < 1 || h < 1)
	{
		wlr_log(WLR_ERROR, "wallpaper: '%s' has no pixels", path);
		stbi_image_free(data);
		return;
	}

	/* stb returns straight-alpha RGBA (R,G,B,A byte order).  The
	 * renderer's wlr_texture_from_pixels accepts DRM_FORMAT_ARGB8888
	 * (which wlroots uploads as GL_BGRA_EXT, i.e. B,G,R,A), so swap
	 * the red and blue channels in place. */
	uint8_t *px = data;
	for (int i = 0; i < w * h; i++, px += 4)
	{
		uint8_t r = px[0];
		px[0] = px[2];
		px[2] = r;
	}

	struct wlr_texture *tex = wlr_texture_from_pixels(server->renderer,
		DRM_FORMAT_ARGB8888, (uint32_t)(w * 4),
		w, h, data);
	stbi_image_free(data);
	if (!tex)
	{
		wlr_log(WLR_ERROR, "wallpaper: upload of '%s' failed", path);
		return;
	}

	/* bake the texture into a buffer the scene can hold */
	struct wlr_buffer *buf = wallpaper_alloc(server, w, h);
	if (!buf)
	{
		wlr_texture_destroy(tex);
		return;
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, buf, NULL);
	if (!pass)
	{
		wlr_texture_destroy(tex);
		wlr_buffer_drop(buf);
		return;
	}
	/* clear to transparent so the opaque image composites cleanly */
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .x = 0, .y = 0, .width = w, .height = h },
		.color = { .r = 0, .g = 0, .b = 0, .a = 0 },
	});
	float alpha = 1.0f;
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = tex,
		.dst_box = { .x = 0, .y = 0, .width = w, .height = h },
		.alpha = &alpha,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
	});
	wlr_render_pass_submit(pass);
	wlr_texture_destroy(tex);

	server->wallpaper_buf = wlr_scene_buffer_create(
		server->layer_trees[0], buf);
	wlr_buffer_drop(buf);
	if (!server->wallpaper_buf)
		return;
	server->wallpaper_tex_w = w;
	server->wallpaper_tex_h = h;
	server->wallpaper_w = 0;
	server->wallpaper_h = 0;
	wlr_log(WLR_INFO, "wallpaper: '%s' loaded (%dx%d)", path, w, h);
}

void hsdwl_wallpaper_update(struct hsdwl_server *server,
		struct wlr_output *output)
{
	if (!server->wallpaper_buf || !output)
		return;
	double scale_factor = output->scale > 0 ? output->scale : 1.0;
	int ow = (int)(output->width / scale_factor);
	int oh = (int)(output->height / scale_factor);
	if (ow < 1 || oh < 1)
		return;
	if (ow == server->wallpaper_w && oh == server->wallpaper_h)
		return;
	server->wallpaper_w = ow;
	server->wallpaper_h = oh;

	/* cover-fit crop, centered */
	double scale = fmax((double)ow / server->wallpaper_tex_w,
		(double)oh / server->wallpaper_tex_h);
	double cw = ow / scale, ch = oh / scale;
	wlr_scene_buffer_set_source_box(server->wallpaper_buf,
		&(struct wlr_fbox){
			.x = (server->wallpaper_tex_w - cw) / 2.0,
			.y = (server->wallpaper_tex_h - ch) / 2.0,
			.width = cw,
			.height = ch,
		});
	wlr_scene_buffer_set_dest_size(server->wallpaper_buf, ow, oh);
}

void hsdwl_wallpaper_destroy(struct hsdwl_server *server)
{
	if (server->wallpaper_buf)
		wlr_scene_node_destroy(&server->wallpaper_buf->node);
	server->wallpaper_buf = NULL;
}
