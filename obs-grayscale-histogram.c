#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include "obs-grayscale-histogram.h"

extern struct obs_source_info grayscale_histogram_filter;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-grayscale-histogram", "en-US")

struct histogram_shared_state g_histogram_state;

struct histogram_filter_data {
	obs_source_t *context;          /* 滤镜自身 */
	uint32_t cx, cy;                /* 源画面尺寸 */
	uint32_t frame_count;           /* 帧计数器（用于频率限制） */
	uint32_t sample_interval;       /* 每 N 帧采样一次 */
	bool readback_pending;          /* 是否有待读取的 GPU 数据 */
	gs_texrender_t *texrender;      /* GPU 离屏渲染目标 */
	gs_stagesurf_t *stagesurface;   /* GPU → CPU 回读缓冲区 */
};

extern void create_histogram_dock(void);

MODULE_EXPORT bool obs_module_load(void)
{
	pthread_mutex_init(&g_histogram_state.mutex, NULL);
	g_histogram_state.data.valid = false;

	obs_register_source(&grayscale_histogram_filter);
	create_histogram_dock();

	return true;
}

MODULE_EXPORT void obs_module_unload(void)
{
	pthread_mutex_destroy(&g_histogram_state.mutex);
}

static const char *gf_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("GrayscaleHistogramFilter");
}

static void *gf_create(obs_data_t *settings, obs_source_t *context)
{
	struct histogram_filter_data *f = bzalloc(sizeof(*f));

	f->context = context;
	f->sample_interval = 15;
	f->frame_count = 0;
	f->readback_pending = false;
	f->cx = 0;
	f->cy = 0;

	obs_enter_graphics();
	f->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

	UNUSED_PARAMETER(settings);
	return f;
}

static void gf_destroy(void *data)
{
	struct histogram_filter_data *f = data;

	obs_enter_graphics();
	gs_stagesurface_destroy(f->stagesurface);
	gs_texrender_destroy(f->texrender);
	obs_leave_graphics();

	bfree(f);
}

static void gf_update(void *data, obs_data_t *settings)
{
	struct histogram_filter_data *f = data;
	f->sample_interval = (uint32_t)obs_data_get_int(settings, "sample_interval");
	if (f->sample_interval < 1)
		f->sample_interval = 1;
}

static void gf_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "sample_interval", 15);
}

static obs_properties_t *gf_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_int_slider(props,
	    "sample_interval",                           // key
	    obs_module_text("SampleInterval"),           // 显示名（从 locale 读）
	    1, 60, 1);                                   // min, max, step

	return props;
}

static void gf_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct histogram_filter_data *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);

	uint32_t cx = target ? obs_source_get_base_width(target) : 0;
	uint32_t cy = target ? obs_source_get_base_height(target) : 0;

	if (cx != f->cx || cy != f->cy) {
		f->cx = cx;
		f->cy = cy;

		if (cx > 0 && cy > 0) {
			obs_enter_graphics();
			gs_texrender_reset(f->texrender);

			gs_stagesurface_destroy(f->stagesurface);

			uint32_t read_h = 256;
			uint32_t read_w = cx * read_h / cy;
			if (read_w > 256) {
				read_w = 256;
				read_h = cy * read_w / cx;
			}
			f->stagesurface = gs_stagesurface_create(read_w, read_h, GS_RGBA);

			obs_leave_graphics();
		}
	}
}

static void render_source_to_texrender(struct histogram_filter_data *f,
					obs_source_t *target,
					uint32_t cx, uint32_t cy)
{
	if (cx == 0 || cy == 0)
		return;

	gs_texrender_reset(f->texrender);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(f->texrender, cx, cy)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

		obs_source_video_render(target);

		gs_texrender_end(f->texrender);
	}

	gs_blend_state_pop();
}

static void compute_histogram_from_rgba(const uint8_t *data, uint32_t linesize,
					 uint32_t width, uint32_t height)
{
	uint32_t bins[HISTOGRAM_BINS];
	memset(bins, 0, sizeof(bins));
	uint32_t total = 0;

	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *row = data + y * linesize;
		for (uint32_t x = 0; x < width; x++) {
			const uint8_t *pixel = row + x * 4;

			/* Rec.601 亮度: Y = 0.299R + 0.587G + 0.114B
			   整数近似: (77R + 150G + 29B) >> 8 */
			uint8_t lum = (uint8_t)((pixel[0] * 77u +
						 pixel[1] * 150u +
						 pixel[2] * 29u) >> 8);
			bins[lum]++;
			total++;
		}
	}

	pthread_mutex_lock(&g_histogram_state.mutex);
	memcpy(g_histogram_state.data.bins, bins, sizeof(bins));
	g_histogram_state.data.total_pixels = total;
	g_histogram_state.data.last_update_ns = os_gettime_ns();
	g_histogram_state.data.valid = true;
	pthread_mutex_unlock(&g_histogram_state.mutex);
}

static enum gs_color_space
gf_get_color_space(void *data, size_t count,
		   const enum gs_color_space *preferred_spaces);

static void gf_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct histogram_filter_data *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);

	if (!target || !parent || f->cx == 0 || f->cy == 0) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	/* ① 频率限制：每 N 帧做一次 GPU 回读 */
	f->frame_count++;
	if (f->frame_count % f->sample_interval == 0 && f->stagesurface) {

		uint32_t read_cx = gs_stagesurface_get_width(f->stagesurface);
		uint32_t read_cy = gs_stagesurface_get_height(f->stagesurface);

		/* 渲染源到降采样纹理 */
		render_source_to_texrender(f, target, read_cx, read_cy);

		/* 先处理上一批 staged 数据 */
		if (f->readback_pending) {
			uint8_t *video_data = NULL;
			uint32_t video_linesize = 0;

			if (gs_stagesurface_map(f->stagesurface,
				&video_data, &video_linesize)) {
				compute_histogram_from_rgba(video_data, video_linesize,
							    read_cx, read_cy);
				gs_stagesurface_unmap(f->stagesurface);
			}
			f->readback_pending = false;
		}

		/* 提交新一批 staging */
		gs_stage_texture(f->stagesurface,
				 gs_texrender_get_texture(f->texrender));
		f->readback_pending = true;
	}

	/* ② 标准透传：用 OBS 滤镜管线，色彩正确 */
	enum gs_color_space space = gf_get_color_space(
		f, 0, NULL);

	if (obs_source_process_filter_begin_with_color_space(
		f->context, GS_RGBA, space,
		OBS_ALLOW_DIRECT_RENDERING)) {
		obs_source_process_filter_end(f->context,
			effect ? effect : obs_get_base_effect(OBS_EFFECT_DEFAULT),
			0, 0);
	} else {
		obs_source_skip_video_filter(f->context);
	}
}

static enum gs_color_space
gf_get_color_space(void *data, size_t count,
		   const enum gs_color_space *preferred_spaces)
{
	struct histogram_filter_data *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);

	const enum gs_color_space spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	    };

	return obs_source_get_color_space(target,
		sizeof(spaces) / sizeof(spaces[0]), spaces);
}

struct obs_source_info grayscale_histogram_filter = {
	.id           = "grayscale_histogram_filter",
	.type         = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name     = gf_get_name,
	.create       = gf_create,
	.destroy      = gf_destroy,
	.get_width    = NULL,
	.get_height   = NULL,
	.get_defaults = gf_defaults,
	.get_properties = gf_properties,
	.update       = gf_update,
	.video_tick   = gf_video_tick,
	.video_render = gf_video_render,
	.video_get_color_space = gf_get_color_space,
};

