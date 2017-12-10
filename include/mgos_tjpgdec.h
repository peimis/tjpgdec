/*
 *
 */
#ifndef __MGOS_LIBS_TJD_JPEG_H
#define __MGOS_LIBS_TJD_JPEG_H


typedef void (*mgos_jpeg_pixel_writer_t)(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint8_t *buf, uint32_t buflen);


#define JPG_IMAGE_LINE_BUF_SIZE		480

#define SCALE_FACTOR_1		0
#define SCALE_FACTOR_2		1	// Slightly slower than non-scaled
#define SCALE_FACTOR_4		2	// Slightly slower than non-scaled
#define SCALE_FACTOR_8		3	// 2-3 times faster than non-scaled

/**
 *
 * @param is_file		If TRUE, imageptr is handled as file, otherwise an source image is considered
 *						being located to memory buffer
 */
void mgos_jpg_image(void *imageptr, bool is_file, int x, int y, uint8_t scale, mgos_jpeg_pixel_writer_t fn);

#endif
