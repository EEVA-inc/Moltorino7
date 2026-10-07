#include <stdlib.h>
#include <string.h>
#include "quirc_internal.h"

const char *quirc_version(void)
{
	return "1.0";
}

struct quirc *quirc_new(void)
{
	struct quirc *q = malloc(sizeof(*q));

	if (!q)
		return NULL;

	memset(q, 0, sizeof(*q));
	return q;
}

void quirc_destroy(struct quirc *q)
{
	free(q->image);
	if (!QUIRC_PIXEL_ALIAS_IMAGE)
		free(q->pixels);
	free(q->flood_fill_vars);
	free(q);
}

int quirc_resize(struct quirc *q, int w, int h)
{
	uint8_t		*image  = NULL;
	quirc_pixel_t	*pixels = NULL;
	size_t num_vars;
	size_t vars_byte_size;
	struct quirc_flood_fill_vars *vars = NULL;

	if (w < 0 || h < 0)
		goto fail;

	image = calloc(w, h);
	if (!image)
		goto fail;

	size_t olddim = q->w * q->h;
	size_t newdim = w * h;
	size_t min = (olddim < newdim ? olddim : newdim);

	(void)memcpy(image, q->image, min);

	if (!QUIRC_PIXEL_ALIAS_IMAGE) {
		pixels = calloc(newdim, sizeof(quirc_pixel_t));
		if (!pixels)
			goto fail;
	}

	if ((size_t)h * 2 / 2 != h) {
		goto fail;
	}
	num_vars = (size_t)h * 2 / 3;
	if (num_vars == 0) {
		num_vars = 1;
	}

	vars_byte_size = sizeof(*vars) * num_vars;
	if (vars_byte_size / sizeof(*vars) != num_vars) {
		goto fail;
	}
	vars = malloc(vars_byte_size);
	if (!vars)
		goto fail;

	q->w = w;
	q->h = h;
	free(q->image);
	q->image = image;
	if (!QUIRC_PIXEL_ALIAS_IMAGE) {
		free(q->pixels);
		q->pixels = pixels;
	}
	free(q->flood_fill_vars);
	q->flood_fill_vars = vars;
	q->num_flood_fill_vars = num_vars;

	return 0;
fail:
	free(image);
	free(pixels);
	free(vars);

	return -1;
}

int quirc_count(const struct quirc *q)
{
	return q->num_grids;
}

static const char *const error_table[] = {
	[QUIRC_SUCCESS] = "Success",
	[QUIRC_ERROR_INVALID_GRID_SIZE] = "Invalid grid size",
	[QUIRC_ERROR_INVALID_VERSION] = "Invalid version",
	[QUIRC_ERROR_FORMAT_ECC] = "Format data ECC failure",
	[QUIRC_ERROR_DATA_ECC] = "ECC failure",
	[QUIRC_ERROR_UNKNOWN_DATA_TYPE] = "Unknown data type",
	[QUIRC_ERROR_DATA_OVERFLOW] = "Data overflow",
	[QUIRC_ERROR_DATA_UNDERFLOW] = "Data underflow"
};

const char *quirc_strerror(quirc_decode_error_t err)
{
	if (err >= 0 && err < sizeof(error_table) / sizeof(error_table[0]))
		return error_table[err];

	return "Unknown error";
}
