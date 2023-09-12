#include <obs-module.h>
#include <plugin-support.h>

struct pixel_art_plugin_data {

	obs_source_t *context;
	gs_effect_t *effect;

	gs_texrender_t *render;

	gs_eparam_t *dither_spread_param;
	gs_eparam_t *bayer_level_param;
	gs_eparam_t *red_color_count_param;
	gs_eparam_t *green_color_count_param;
	gs_eparam_t *blue_color_count_param;
	gs_eparam_t *texture_width, *texture_height;

	int downscales, bayer_level, red_count, green_count, blue_count;
	float dither_spread, texwidth, texheight;
	bool processed_frame;
};

static const char *pixel_art_plugin_getname(void *unused)
{

	UNUSED_PARAMETER(unused);
	return obs_module_text("PixelArt");
}

static void pixel_art_plugin_update(void *data, obs_data_t *settings)
{

	struct pixel_art_plugin_data *filter = data;
	filter->downscales = (int)obs_data_get_int(settings, "downscales");
	filter->dither_spread =
		(float)obs_data_get_double(settings, "ditherspread");
	filter->bayer_level = (int)obs_data_get_int(settings, "bayerlevel");
	filter->red_count = (int)obs_data_get_int(settings, "redcount");
	filter->green_count = (int)obs_data_get_int(settings, "greencount");
	filter->blue_count = (int)obs_data_get_int(settings, "bluecount");
}

static void pixel_art_plugin_destroy(void *data)
{

	struct pixel_art_plugin_data *filter = data;

	if (filter->effect) {
		obs_enter_graphics();
		gs_effect_destroy(filter->effect);
		obs_leave_graphics();
	}

	bfree(data);
}

static void *pixel_art_plugin_create(obs_data_t *settings,
				     obs_source_t *context)
{

	struct pixel_art_plugin_data *filter =
		bzalloc(sizeof(struct pixel_art_plugin_data));
	char *effect_path = obs_module_file("pixel_art.effect");

	filter->context = context;

	obs_enter_graphics();

	filter->effect = gs_effect_create_from_file(effect_path, NULL);

	if (filter->effect) {
		filter->dither_spread_param =
			gs_effect_get_param_by_name(filter->effect, "spread");

		filter->bayer_level_param = gs_effect_get_param_by_name(
			filter->effect, "bayerLevel");

		filter->red_color_count_param = gs_effect_get_param_by_name(
			filter->effect, "redColorCount");

		filter->green_color_count_param = gs_effect_get_param_by_name(
			filter->effect, "greenColorCount");

		filter->blue_color_count_param = gs_effect_get_param_by_name(
			filter->effect, "blueColorCount");

		filter->texture_width = gs_effect_get_param_by_name(
			filter->effect, "texelWidth");

		filter->texture_height = gs_effect_get_param_by_name(
			filter->effect, "texelHeight");
	}
	obs_leave_graphics();

	bfree(effect_path);

	if (!filter->effect) {
		pixel_art_plugin_destroy(filter);
		return NULL;
	}

	pixel_art_plugin_update(filter, settings);
	return filter;
}

void pixel_art_plugin_draw_frame(struct pixel_art_plugin_data *context,
				 uint32_t w, uint32_t h)
{
	const enum gs_color_space current_space = gs_get_color_space();

	gs_effect_t *effect = context->effect;

	context->texwidth = (float)w;
	context->texheight = (float)h;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_texture_t *tex = gs_texrender_get_texture(context->render);
	if (!tex)
		return;

	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"),
				   tex);

	gs_effect_set_float(context->texture_width, context->texwidth);
	gs_effect_set_float(context->texture_height, context->texheight);
	gs_effect_set_float(context->dither_spread_param,
			    context->dither_spread);

	gs_effect_set_int(context->bayer_level_param, context->bayer_level);
	gs_effect_set_int(context->red_color_count_param, context->red_count);
	gs_effect_set_int(context->green_color_count_param,
			  context->green_count);
	gs_effect_set_int(context->blue_color_count_param, context->blue_count);

	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, w, h);

	gs_enable_framebuffer_srgb(previous);
	gs_blend_state_pop();
}

static void pixel_art_plugin_render(void *data, gs_effect_t *effect)
{

	UNUSED_PARAMETER(effect);

	struct pixel_art_plugin_data *filter = data;

	obs_source_t *target = obs_filter_get_target(filter->context);
	obs_source_t *parent = obs_filter_get_parent(filter->context);

	uint32_t base_width = obs_source_get_width(target);
	uint32_t base_height = obs_source_get_height(target);
	uint32_t current_width = base_width;
	uint32_t current_height = base_height;

	const float w = (float)base_width;
	const float h = (float)base_height;

	uint32_t parent_flags = obs_source_get_output_flags(target);
	bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
	bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;

	struct vec4 clear_color;
	vec4_zero(&clear_color);

	if (!base_width || !base_height || !target || !parent) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	if (filter->processed_frame) {
		pixel_art_plugin_draw_frame(filter, base_width, base_height);
		return;
	}

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	const enum gs_color_space space = obs_source_get_color_space(
		target, OBS_COUNTOF(preferred_spaces), preferred_spaces);
	const enum gs_color_format format = gs_get_format_from_space(space);

	if (!filter->render ||
	    gs_texrender_get_format(filter->render) != format) {
		gs_texrender_destroy(filter->render);
		filter->render = gs_texrender_create(format, GS_ZS_NONE);
	} else {
		gs_texrender_reset(filter->render);
	}

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	for (int i = 0; i < filter->downscales; i++) {

		gs_texrender_reset(filter->render);

		current_width /= 2;
		current_height /= 2;

		if (gs_texrender_begin_with_color_space(
			    filter->render, current_width, current_height,
			    space)) {

			gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
			gs_ortho(0.0f, w, 0.0f, h, -100.0f, 100.0f);

			if (target == parent && !custom_draw && !async) {
				obs_source_default_render(target);
			} else {
				obs_source_video_render(target);
			}
			gs_texrender_end(filter->render);
		}
	}

	if (filter->downscales == 0) {

		gs_texrender_reset(filter->render);

		if (gs_texrender_begin_with_color_space(
			    filter->render, current_width, current_height,
			    space)) {

			gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
			gs_ortho(0.0f, w, 0.0f, h, -100.0f, 100.0f);

			if (target == parent && !custom_draw && !async) {
				obs_source_default_render(target);
			} else {
				obs_source_video_render(target);
			}
			gs_texrender_end(filter->render);
		}
	}

	gs_blend_state_pop();

	filter->processed_frame = true;

	pixel_art_plugin_draw_frame(filter, base_width, base_height);
}

static obs_properties_t *pixel_art_plugin_properties(void *data)
{

	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *prop = obs_properties_add_int_slider(
		props, "downscales", obs_module_text("TimesToDownscale"), 0, 8,
		1);

	obs_properties_t *dither = obs_properties_create();

	prop = obs_properties_add_float_slider(dither, "ditherspread",
					       obs_module_text("DitherSpread"),
					       0.0, 1.0, 0.01);

	prop = obs_properties_add_int_slider(
		dither, "bayerlevel", obs_module_text("BayerLevel"), 0, 2, 1);

	prop = obs_properties_add_int_slider(
		dither, "redcount", obs_module_text("RedColorCount"), 1, 16, 1);

	prop = obs_properties_add_int_slider(dither, "greencount",
					     obs_module_text("GreenColorCount"),
					     1, 16, 1);

	prop = obs_properties_add_int_slider(dither, "bluecount",
					     obs_module_text("BlueColorCount"),
					     1, 16, 1);

	obs_properties_add_group(props, "dither", obs_module_text("Dithering"),
				 OBS_GROUP_NORMAL, dither);

	return props;
}

static void pixel_art_plugin_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "downscales", 0);
	obs_data_set_default_int(settings, "bayerlevel", 0);
	obs_data_set_default_int(settings, "redcount", 8);
	obs_data_set_default_int(settings, "greencount", 8);
	obs_data_set_default_int(settings, "bluecount", 8);
	obs_data_set_default_double(settings, "ditherspread", 0.0);
}

void pixel_art_plugin_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct pixel_art_plugin_data *context = data;
	context->processed_frame = false;
}

static enum gs_color_space
pixel_art_plugin_get_color_space(void *data, size_t count,
				 const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	const enum gs_color_space potential_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	struct pixel_art_plugin_data *const filter = data;
	const enum gs_color_space source_space = obs_source_get_color_space(
		obs_filter_get_target(filter->context),
		OBS_COUNTOF(potential_spaces), potential_spaces);

	return source_space;
}

struct obs_source_info pixel_art_plugin = {
	.id = "pixel_art_plugin",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = pixel_art_plugin_getname,
	.create = pixel_art_plugin_create,
	.video_render = pixel_art_plugin_render,
	.video_tick = pixel_art_plugin_video_tick,
	.update = pixel_art_plugin_update,
	.destroy = pixel_art_plugin_destroy,
	.get_properties = pixel_art_plugin_properties,
	.get_defaults = pixel_art_plugin_defaults,
	.video_get_color_space = pixel_art_plugin_get_color_space,
};