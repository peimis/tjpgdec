#include "mgos.h"
#include "mgos_tjpgdec.h"
#include "rom/tjpgd.h"

mgos_jpeg_pixel_writer_t write_fn;


// ================ JPG SUPPORT ================================================
// User defined device identifier
typedef struct {
	FILE		*fhndl;			// File handler for input function
	int			x;				// image top left point X position
	int			y;				// image top left point Y position
	uint8_t		*membuff;		// memory buffer containing the image
	uint32_t	bufsize;		// size of the memory buffer
	uint32_t	bufptr;			// memory buffer current position
	uint8_t	*linbuf[2];		// memory buffer used for display output in RGB565 format
	uint8_t		linbuf_idx;
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


#define _RGB5(_v) 		(((_v)>>3) & 0x1F)
#define _RGB6(_v) 		(((_v)>>2) & 0x3F)
#define TO_RGB565(r, g, b)	((_RGB5(r) << 11) | (_RGB6(g) << 5) | (_RGB5(b)))

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
	int dleft, dtop, dright, dbottom;
	int x, y;

	const int left = rect->left + dev->x;
	const int top = rect->top + dev->y;
	const int right = rect->right + dev->x;
	const int bottom = rect->bottom + dev->y;
	dleft = left;
	dtop = top;
	dright = right;
	dbottom = bottom;

	uint32_t len = 2* ((right-left+1) * (bottom-top+1));	// calculate length of data

	if ((len > 0) && (len <= JPG_IMAGE_LINE_BUF_SIZE)) {
		uint8_t *dest = (uint8_t *)(dev->linbuf[dev->linbuf_idx]);
		uint16_t rgb565;

		for (y = top; y <= bottom; y++) {
			for (x = left; x <= right; x++) {
				rgb565 = TO_RGB565(src[0], src[1], src[2]);
				// Clip to display area
				if ((x >= dleft) && (y >= dtop) && (x <= dright) && (y <= dbottom)) {
					*dest++ = (rgb565 >> 8) & 0xFF;
					*dest++ = rgb565 & 0xFF;
//					*dest++ = (*src++) & 0xFC;
//					*dest++ = (*src++) & 0xFC;
//					*dest++ = (*src++) & 0xFC;
				}
				else {
					*dest++ = 0xa5;
					*dest++ = 0x5a;
				}
				src += 3;
			}
		}

		if (write_fn) {
//			LOG(LL_INFO, ("Write Rect %d,%d:%d,%d -> dev %d,%d:%d,%d len=%d", rect->left, rect->top, rect->right, rect->bottom, dleft, dtop, dright, dbottom, len));
			write_fn(dleft, dtop, dright, dbottom, dev->linbuf[dev->linbuf_idx], len);
		} else {
			LOG(LL_WARN, ("No place to put data.."));
		}
		dev->linbuf_idx = ((dev->linbuf_idx + 1) & 1);
	}
	else {
		LOG(LL_WARN, ("Data size error: %d jpg: (%d,%d,%d,%d) disp: (%d,%d,%d,%d)", len, left,top,right,bottom, dleft,dtop,dright,dbottom));
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

	dev.linbuf[0] = NULL;
	dev.linbuf[1] = NULL;
	dev.linbuf_idx = 0;

	dev.fhndl = NULL;

	if (imageptr == NULL) {
		return;
	}

	if (!is_file) {
		int size = 123;
		// image from buffer
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

			dev.linbuf[0] = calloc(1, JPG_IMAGE_LINE_BUF_SIZE*3);
			if (dev.linbuf[0] == NULL) {
				LOG(LL_WARN, ("Line buffer #0 alloc failed"));
				goto exit;
			}
			dev.linbuf[1] = calloc(1, JPG_IMAGE_LINE_BUF_SIZE*3);
			if (dev.linbuf[1] == NULL) {
				LOG(LL_WARN, ("Line buffer #1 alloc failed"));
				goto exit;
			}

			write_fn = pixelwriter;

			// Start to decode the JPEG file
			rc = jd_decomp(&jd, tjd_output, scale);

			if (rc != JDR_OK) {
				LOG(LL_WARN, ("JPEG decompression error %d", rc));
			}
			LOG(LL_INFO, ("JPEG size: %dx%d, position; %d,%d, scale: %d, bytes used: %d\r\n", jd.width, jd.height, x, y, scale, jd.sz_pool));
		}
		else {
			LOG(LL_WARN, ("JPEG prepare error: %d", rc));
		}
	}
	else {
		LOG(LL_INFO, ("Work buffer alloc failed"));
	}

exit:
	if (work) free(work);  // free work buffer
	if (dev.linbuf[0]) free(dev.linbuf[0]);
	if (dev.linbuf[1]) free(dev.linbuf[1]);
	if (dev.fhndl) fclose(dev.fhndl);  // close input file
	write_fn = NULL;
}



bool mgos_tjpgdec_init(void)
{
	write_fn = NULL;

	return true;
}