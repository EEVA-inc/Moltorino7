#ifndef QUIRC_H_
#define QUIRC_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct quirc;

const char *quirc_version(void);

struct quirc *quirc_new(void);

void quirc_destroy(struct quirc *q);

int quirc_resize(struct quirc *q, int w, int h);

uint8_t *quirc_begin(struct quirc *q, int *w, int *h);
void quirc_end(struct quirc *q);

struct quirc_point {
	int	x;
	int	y;
};

typedef enum {
	QUIRC_SUCCESS = 0,
	QUIRC_ERROR_INVALID_GRID_SIZE,
	QUIRC_ERROR_INVALID_VERSION,
	QUIRC_ERROR_FORMAT_ECC,
	QUIRC_ERROR_DATA_ECC,
	QUIRC_ERROR_UNKNOWN_DATA_TYPE,
	QUIRC_ERROR_DATA_OVERFLOW,
	QUIRC_ERROR_DATA_UNDERFLOW
} quirc_decode_error_t;

const char *quirc_strerror(quirc_decode_error_t err);

#define QUIRC_MAX_VERSION	40
#define QUIRC_MAX_GRID_SIZE	(QUIRC_MAX_VERSION * 4 + 17)
#define QUIRC_MAX_BITMAP	(((QUIRC_MAX_GRID_SIZE * QUIRC_MAX_GRID_SIZE) + 7) / 8)
#define QUIRC_MAX_PAYLOAD	8896

#define QUIRC_ECC_LEVEL_M     0
#define QUIRC_ECC_LEVEL_L     1
#define QUIRC_ECC_LEVEL_H     2
#define QUIRC_ECC_LEVEL_Q     3

#define QUIRC_DATA_TYPE_NUMERIC       1
#define QUIRC_DATA_TYPE_ALPHA         2
#define QUIRC_DATA_TYPE_BYTE          4
#define QUIRC_DATA_TYPE_KANJI         8

#define QUIRC_ECI_ISO_8859_1		1
#define QUIRC_ECI_IBM437		2
#define QUIRC_ECI_ISO_8859_2		4
#define QUIRC_ECI_ISO_8859_3		5
#define QUIRC_ECI_ISO_8859_4		6
#define QUIRC_ECI_ISO_8859_5		7
#define QUIRC_ECI_ISO_8859_6		8
#define QUIRC_ECI_ISO_8859_7		9
#define QUIRC_ECI_ISO_8859_8		10
#define QUIRC_ECI_ISO_8859_9		11
#define QUIRC_ECI_WINDOWS_874		13
#define QUIRC_ECI_ISO_8859_13		15
#define QUIRC_ECI_ISO_8859_15		17
#define QUIRC_ECI_SHIFT_JIS		20
#define QUIRC_ECI_UTF_8			26

struct quirc_code {
	struct quirc_point	corners[4];

	int			size;
	uint8_t			cell_bitmap[QUIRC_MAX_BITMAP];
};

struct quirc_data {
	int			version;
	int			ecc_level;
	int			mask;

	int			data_type;

	uint8_t			payload[QUIRC_MAX_PAYLOAD];
	int			payload_len;

	uint32_t		eci;
};

int quirc_count(const struct quirc *q);

void quirc_extract(const struct quirc *q, int index,
		   struct quirc_code *code);

quirc_decode_error_t quirc_decode(const struct quirc_code *code,
				  struct quirc_data *data);

void quirc_flip(struct quirc_code *code);

#ifdef __cplusplus
}
#endif

#endif
