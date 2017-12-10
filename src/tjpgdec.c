#include "mgos.h"
#include "mgos_tjpgdec.h"
#include "rom/tjpgd.h"

mgos_jpeg_pixel_writer_t write_fn;

#define _RGB5(_v) 		(((_v)>>3) & 0x1F)
#define _RGB6(_v) 		(((_v)>>2) & 0x3F)
#define TO_RGB565(r, g, b)	((_RGB5(r) << 11) | (_RGB6(g) << 5) | (_RGB5(b)))

// ================ JPG SUPPORT ================================================
// User defined device identifier
typedef struct {
	FILE		*fhndl;			// File handler for input function
	int			x;				// image top left point X position
	int			y;				// image top left point Y position
	//
	uint8_t		*linebuf;		// memory buffer used for display output in RGB565 format
	uint16_t	linebuf_w;		// width of line buffer in pixels
	uint16_t	linebuf_h;		// current height of line buffer in pixels
	int			linebuf_size;	// buffer w*h
	uint16_t	linebuf_startline;	// starting line of linebuffer
	int			linebuf_pixels_left;
	int			image_pixels_left;
	//
	uint8_t		*membuff;		// memory buffer containing the image
	uint32_t	bufsize;		// size of the memory buffer
	uint32_t	bufptr;			// memory buffer current position
//	uint8_t		linbuf_idx;
} JPGIODEV;


// User defined call-back function to input JPEG data from file
//---------------------
static UINT tjd_input (
	JDEC* jd,		// Decompression object
	BYTE* buff,		// Pointer to the read buffer (NULL:skip)
	UINT nd			// Number of bytes to read/skip from input stream
)
{
	int rb = 0;
	// Device identifier for the session (5th argument of jd_prepare function)
	JPGIODEV *dev = (JPGIODEV*)jd->device;

	if (buff) {	// Read nd bytes from the input strem
		rb = fread(buff, 1, nd, dev->fhndl);
		return rb;	// Returns actual number of bytes read
	}
	else {	// Remove nd bytes from the input stream
		if (fseek(dev->fhndl, nd, SEEK_CUR) >= 0) return nd;
		else return 0;
	}
}

// User defined call-back function to input JPEG data from memory buffer
//-------------------------
static UINT tjd_buf_input (
	JDEC* jd,		// Decompression object
	BYTE* buff,		// Pointer to the read buffer (NULL:skip)
	UINT nd			// Number of bytes to read/skip from input stream
)
{
	// Device identifier for the session (5th argument of jd_prepare function)
	JPGIODEV *dev = (JPGIODEV*)jd->device;
	if (!dev->membuff) return 0;
	if (dev->bufptr >= (dev->bufsize + 2)) return 0; // end of stream

	if ((dev->bufptr + nd) > (dev->bufsize + 2)) nd = (dev->bufsize + 2) - dev->bufptr;

	if (buff) {	// Read nd bytes from the input strem
		memcpy(buff, dev->membuff + dev->bufptr, nd);
		dev->bufptr += nd;
		return nd;	// Returns number of bytes read
	}
	else {	// Remove nd bytes from the input stream
		dev->bufptr += nd;
		return nd;
	}
}



// User defined call-back function to output RGB bitmap to display device
//----------------------
static UINT tjd_output (
	JDEC* jd,		// Decompression object of current session
	void* bitmap,	// Bitmap data to be output
	JRECT* rect		// Rectangular region to output
)
{
	// Device identifier for the session (5th argument of jd_prepare function)
	JPGIODEV *dev = (JPGIODEV*)jd->device;
	BYTE *src = (BYTE*)bitmap;

	// ** Put the rectangular into the display device **
	int x, y;

	const int left = rect->left + dev->x;
	const int top = rect->top + dev->y;
	const int right = rect->right + dev->x;
	const int bottom = rect->bottom + dev->y;

	uint32_t len = ((right-left+1) * (bottom-top+1));	// calculate length of data

	if ((len > 0) && (len <= JPG_IMAGE_LINE_BUF_SIZE)) {
		uint16_t *dest = (uint16_t *)dev->linebuf;
		uint16_t rgb565;
		int index;

		// A new set of lines begins
		if ((0 == dev->linebuf_pixels_left) && (0 == rect->left)) {
			dev->linebuf_startline = rect->top;
			dev->linebuf_h = rect->bottom - rect->top + 1;
			dev->linebuf_pixels_left = dev->linebuf_w * dev->linebuf_h;
/*
			LOG(LL_INFO, ("Start filling linebuf with rect %d,%d:%d,%d : %d lines %d..%d",
				rect->left, rect->top, rect->right, rect->bottom,
				dev->linebuf_h, dev->linebuf_startline, dev->linebuf_startline + dev->linebuf_h ));
*/
		}

//		LOG(LL_INFO, ("Fill lines %d..%d X=%d..%d pixels left=%d", top, bottom, left, right, dev->linebuf_pixels_left));

		for (y = top; y <= bottom; y++) {
			index = (dev->linebuf_w * (y-dev->linebuf_startline)) + left;
			for (x = left; x <= right; x++) {
				rgb565 = TO_RGB565(src[0], src[1], src[2]);
				dest[ index++ ] = (rgb565 >> 8) | (rgb565 << 8);
				src += 3;
			}
		}

		dev->linebuf_pixels_left -= len;

		if (dev->linebuf_pixels_left <= 0) {
			const uint32_t pixels_written = dev->linebuf_w * dev->linebuf_h;
			dev->image_pixels_left -= pixels_written;
			LOG(LL_INFO, ("Write lines %d-%d W=%d pixels=%d image left=%d", dev->linebuf_startline, dev->linebuf_startline+dev->linebuf_h-1,
				dev->linebuf_w , pixels_written, dev->image_pixels_left));

			if (write_fn) {
				write_fn(0, dev->linebuf_startline, dev->linebuf_w-1, dev->linebuf_startline+dev->linebuf_h-1, 
					dev->linebuf, pixels_written * sizeof(uint16_t) );
				}
		}
	}
	else {
		LOG(LL_WARN, ("Data size error: %d jpg: (%d,%d:%d,%d)", len, left,top,right,bottom ));
		return 0;  // stop decompression
	}
	return 1;	// Continue to decompression
}


// tft.jpgimage(X, Y, scale, file_name, buf, size]
// X & Y can be < 0 !
//==================================================================================
void mgos_jpg_image(void *imageptr, bool is_file, int x, int y, uint8_t scale, mgos_jpeg_pixel_writer_t pixelwriter)
{
	JPGIODEV dev;
	struct stat sb;
	char *work = NULL;		// Pointer to the working buffer (must be 4-byte aligned)
	UINT sz_work = 3800;	// Size of the working buffer (must be power of 2)
	JDEC jd;				// Decompression object (70 bytes)
	JRESULT rc;

	dev.linebuf = NULL;
//	dev.linbuf_idx = 0;

	dev.fhndl = NULL;

	if (imageptr == NULL) {
		return;
	}

	if (!is_file) {
		int size = 123;
		// image from buffer
		LOG(LL_ERROR, ("iMAGE from buffer not supported yet"));

		dev.membuff = imageptr;
		dev.bufsize = size;
		dev.bufptr = 0;
	}
	else {
		char *fname = (char *)imageptr;
		// image from file
		dev.membuff = NULL;
		dev.bufsize = 0;
		dev.bufptr = 0;

		if (stat(fname, &sb) != 0) {
			LOG(LL_WARN, ("File error: '%s'", strerror(errno)));
			goto exit;
		}

		dev.fhndl = fopen(fname, "r");
		if (!dev.fhndl) {
			LOG(LL_WARN, ("Error opening file: '%s'", strerror(errno)));
			goto exit;
		}
	}

	if (scale > 3) scale = 3;

	work = malloc(sz_work);

	if (work) {
		if (dev.membuff) rc = jd_prepare(&jd, tjd_buf_input, (void *)work, sz_work, &dev);
		else rc = jd_prepare(&jd, tjd_input, (void *)work, sz_work, &dev);
		if (rc == JDR_OK) {

			dev.x = x;
			dev.y = y;

			// Make room for a maximum of 16 lines of 16-bit RGB 565 data
			// linebuf_h is adjusted in tjd_output accordind to given rectangle size
			dev.linebuf_h = 16;
			dev.linebuf_w = jd.width / (1 << scale);
			dev.linebuf_size = sizeof(uint16_t) * dev.linebuf_w * dev.linebuf_h;
			dev.linebuf_pixels_left = 0;
			dev.image_pixels_left = dev.linebuf_w * (jd.height / (1 << scale));
			LOG(LL_INFO, ("JPEG size: %dx%d, position; %d,%d, scale: %d, bytes used: %d", jd.width, jd.height, x, y, scale, jd.sz_pool));
			LOG(LL_INFO, ("Linebuf size: %dx%d, bytes: %d image %d pixels", dev.linebuf_w, dev.linebuf_h, dev.linebuf_size, dev.image_pixels_left));

			dev.linebuf = calloc(1, dev.linebuf_size);
			if (dev.linebuf == NULL) {
				LOG(LL_ERROR, ("Line buffer alloc failed"));
				goto exit;
			}
			write_fn = pixelwriter;

			// Start to decode the JPEG file
			rc = jd_decomp(&jd, tjd_output, scale);

			if (rc != JDR_OK) {
				LOG(LL_ERROR, ("JPEG decompression error %d", rc));
			}
		}
		else {
			LOG(LL_ERROR, ("JPEG prepare error: %d", rc));
		}
	}
	else {
		LOG(LL_ERROR, ("Work buffer alloc failed"));
	}

exit:
	if (work) free(work);  // free work buffer
	if (dev.linebuf) free(dev.linebuf);
	if (dev.fhndl) fclose(dev.fhndl);  // close input file
	write_fn = NULL;
}



bool mgos_tjpgdec_init(void)
{
	write_fn = NULL;

	return true;
}