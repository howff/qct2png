/* > qct.cpp
 * 1.11 arb Fri Nov  9 18:27:01 GMT 2012 - read license information.
 * 1.10 arb Mon Jul 12 18:54:20 BST 2010 - added scalefactor, changed the API
 *      to allow the image to be reloaded at a different scale.
 * 1.07 arb Sat Jul 10 13:22:22 BST 2010 - added getDegreesPerPixel
 * 1.06 arb Tue Jun 22 04:38:55 BST 2010 - Fix huffman bug.
 * 1.05 arb Mon Jun 14 05:48:12 BST 2010 - Implemented latlon_to_xy.
 * 1.04 arb Fri May 28 10:59:34 BST 2010 - added unload methods called from
 *      destructor (and one is public to free image_data). headerOnly now bool.
 * 1.03 arb Thu May 27 22:36:49 BST 2010 - added coordInsideMap.
 * 1.02 arb Thu May 27 09:27:41 BST 2010 - Added to osmap lib, added getColour,
 *      memset palette in constructor.
 * 1.01 arb Mon May 24 21:56:04 BST 2010 - Fix bug reporting outline extent
 * 1.00 arb Sun May 23 23:46:28 BST 2010
 */

static const char SCCSid[] = "@(#)qct.c         1.11 (C) 2010 arb mapLib: Extract QCT map";

/*
 * The QCT class is a simple class to read a QCT (QuickChart) image
 * and unpack the image data into a memory buffer.  It can return a
 * pointer to the image data and the palette colours.
 */

/*
 * To do:
 * Use the colour interpolation matrix when scaling down.
 * Speed up everything.
 */

/*
 * Includes
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>  // for strerror
#include <time.h>    // for ctime
#include <math.h>    // for log2
#include <errno.h>   // for errno
#include "satlib/dundee.h" // for byte order
#include "inpoly.h"  // check if coord inside polygon (int coords)
#include "qct.h"

#ifdef USE_GIFLIB
#include "gif/gif.h"
#endif

#ifdef USE_PNG
#include <png.h>
#endif

#ifdef USE_TIFF
#endif

/*
 * 64-bit file I/O
 * Not specifically needed for the QCT decoder since it uses 32-bit
 * file offsets inside the file so can't be more than 2GB anyway.
 */
#ifdef unix
//#define HAS_FOPEN64
#endif

#ifdef HAS_FOPEN64
# ifndef _LARGEFILE64_SOURCE
#  define _LARGEFILE64_SOURCE // see lf64 manual
# endif
# define FOPEN  fopen64
# define FSEEKO fseeko64
# define FTELLO ftello64
# define OFF_T  off64_t
#elif defined HAS_FSEEKO
# define FOPEN  fopen
# define FSEEKO fseeko
# define FTELLO ftello
# define OFF_T  off_t
#else
# define FOPEN  fopen
# define FSEEKO fseek
# define FTELLO ftell
# define OFF_T  long
#endif


// QCT file format
// as documented in:
// The_Quick_Chart_File_Format_Specification_1.01.pdf
// All words are little-endian
// Header: 24 words
// Georef: 40 doubles
// Palette: 256 words of RGB where blue is LSByte
// Interp: 128x128 bytes
// Index: w*h words
// Data... tiles are 64x64 pixels

#define QCT_MAGIC 0x1423D5FF
#define QCT_TILE_SIZE 64
#define QCT_TILE_PIXELS (QCT_TILE_SIZE*QCT_TILE_SIZE)
// RGB stored packed in an int, Blue is LSB
#define PAL_RED(c)   ((c>>16)&255)
#define PAL_GREEN(c) ((c>>8)&255)
#define PAL_BLUE(c)  ((c)&255)


/* -------------------------------------------------------------------------
 * Generic little-endian file-reading
 */
static int
readInt(FILE *fp)
{
	int vv;
	vv = fgetc(fp);
	vv |= (fgetc(fp) << 8);
	vv |= (fgetc(fp) << 16);
	vv |= (fgetc(fp) << 24);
	if (feof(fp)) return(-1);
	return(vv);
}

static int
readIntSwapped(FILE *fp)
{
	int vv;
	vv  = (fgetc(fp) << 24);
	vv |= (fgetc(fp) << 16);
	vv |= (fgetc(fp) << 8);
	vv |= (fgetc(fp) << 0);
	if (feof(fp)) return(-1);
	return(vv);
}


static double
readDouble(FILE *fp)
{
	double dd;
	unsigned char *pp = (unsigned char*)&dd;
#ifdef _LITTLE_ENDIAN
	pp[0] = fgetc(fp);
	pp[1] = fgetc(fp);
	pp[2] = fgetc(fp);
	pp[3] = fgetc(fp);
	pp[4] = fgetc(fp);
	pp[5] = fgetc(fp);
	pp[6] = fgetc(fp);
	pp[7] = fgetc(fp);
#else
	pp[7] = fgetc(fp);
	pp[6] = fgetc(fp);
	pp[5] = fgetc(fp);
	pp[4] = fgetc(fp);
	pp[3] = fgetc(fp);
	pp[2] = fgetc(fp);
	pp[1] = fgetc(fp);
	pp[0] = fgetc(fp);
#endif
	return(dd);
}


/* -------------------------------------------------------------------------
 * Returns a string (nul-terminated) allocated from malloc
 * obtained by reading an index pointer then following it
 * by seeking into the file.  NB. file pointer upon return
 * will be after the index pointer not after the string.
 * If index pointer is nul then an empty string is returned
 * (rather than a null pointer).
 */
static char *
readString(FILE *fp)
{
	OFF_T current_offset, string_offset;
	int ii;
	char *string;
	int string_length = 0, buf_length = 1024;

	string = (char*)malloc(buf_length);
	ii = readInt(fp);
	if (ii == 0)
		return(strdup(""));
	current_offset = FTELLO(fp);
	string_offset = ii; // yes, offsets are limited to 32-bits :-(
	FSEEKO(fp, string_offset, SEEK_SET);
	while (1)
	{
		ii = fgetc(fp);
		string[string_length] = ii;
		if (ii == 0)
			break;
		string_length++;
		if (string_length > buf_length)
		{
			buf_length += 1024;
			string = (char*)realloc(string, buf_length);
		}
	}
	string = (char*)realloc(string, string_length+1);
	FSEEKO(fp, current_offset, SEEK_SET);
	return(string);
}


/* -------------------------------------------------------------------------
 */
static int
bits_per_pixel(int num_colours)
{
#ifdef HAS_LOG2
	return (int)(log2(num_colours)+0.999);
#else
	if (num_colours <=   2) return 1;
	if (num_colours <=   4) return 2;
	if (num_colours <=   8) return 3;
	if (num_colours <=  16) return 4;
	if (num_colours <=  32) return 5;
	if (num_colours <=  64) return 6;
	if (num_colours <= 128) return 7;
#endif
}


/* -------------------------------------------------------------------------
 * Class to read a QCT map image.
 */
QCT::QCT()
{
	qctfp = NULL;
	width = height = 0;
	scalefactor = 1;
	image_data = NULL;
	memset(palette, 0, sizeof(palette));

	// Metadata
	metadata.title = metadata.name = metadata.ident = metadata.edition = metadata.revision = NULL;
	metadata.keywords = metadata.copyright = metadata.scale = metadata.datum = NULL;
	metadata.depths = metadata.heights = metadata.projection = NULL;
	metadata.origfilename = NULL;
	metadata.maptype = metadata.diskname = NULL;

	// Outline
	metadata.num_outline = 0;
	metadata.outline_lat = metadata.outline_lon = NULL;

	// Offsets to tiles
	metadata.image_index = NULL;

	// Program options
	verbose = debug = debug_kml_outline = debug_kml_boundary = 0;
	dfp = stdout;
}


QCT::~QCT()
{
	closeFilename();
}


#define FREE_POINTER(P) if (P) { free(P); P = NULL; }

void
QCT::unloadImage()
{
	// Image data
	FREE_POINTER(image_data);
}

void
QCT::unloadMetadata()
{
	// Metadata
	FREE_POINTER(metadata.title);
	FREE_POINTER(metadata.name);
	FREE_POINTER(metadata.ident);
	FREE_POINTER(metadata.edition);
	FREE_POINTER(metadata.revision);
	FREE_POINTER(metadata.keywords);
	FREE_POINTER(metadata.copyright);
	FREE_POINTER(metadata.scale);
	FREE_POINTER(metadata.datum);
	FREE_POINTER(metadata.depths);
	FREE_POINTER(metadata.heights);
	FREE_POINTER(metadata.projection);
	FREE_POINTER(metadata.origfilename);
	FREE_POINTER(metadata.maptype);
	FREE_POINTER(metadata.diskname);
	// Map outline
	FREE_POINTER(metadata.outline_lat);
	FREE_POINTER(metadata.outline_lon);
	// Offsets to tiles
	FREE_POINTER(metadata.image_index);
}


void
QCT::unload()
{
	unloadImage();
	unloadMetadata();
}


void
QCT::closeFilename()
{
	if (qctfp)
		fclose(qctfp);
	qctfp = NULL;
	unload();
}


/* -------------------------------------------------------------------------
 */
void
QCT::throwError(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}


void
QCT::message(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (dfp && (debug|verbose))
	{
		vfprintf(dfp, fmt, ap);
		fprintf(dfp, "\n");
	}
	va_end(ap);
}


void
QCT::debugmsg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (debug)
	{
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
	}
	va_end(ap);
}


/* -------------------------------------------------------------------------
 * fp is assumed to be open and pointing at the start of the tile data already
 * xx,yy are offsets (from 0,0 topleft) of the *tile* number
 * Pixels are put at the appropriate bytes in the class var image_data.
 */
void
QCT::readTile(FILE *fp, int tile_xx, int tile_yy, int scalefactor)
{
	unsigned char tile_data[QCT_TILE_PIXELS];
	unsigned char *row_ptr[QCT_TILE_SIZE];
	int packing;
	int row, bytes_per_row;
	int pixelnum = 0;
	int ii;
	// Rows are interleaved in this order (reverse binary)
	static int row_seq[] =
	{
		0,  32, 16, 48,  8, 40, 24, 56,  4, 36, 20, 52, 12, 44, 28, 60, 2,
		34, 18, 50, 10, 42, 26, 58,  6, 38, 22, 54, 14, 46, 30, 62, 1,
		33, 17, 49,  9, 41, 25, 57,  5, 37, 21, 53, 13, 45, 29, 61, 3, 35,
		19, 51, 11, 43, 27, 59,  7, 39, 23, 55, 15, 47, 31, 63
	};


	debugmsg("Tile %d, %d starts at file offset 0x%x", tile_xx, tile_yy, FTELLO(fp));

	// Determine which method was used to pack this tile
	packing = fgetc(fp);

	debugmsg("Reading tile %d, %d; packed using %s", tile_xx, tile_yy, ((packing==0||packing==255)?"huffman":(packing>127?"pixel":"RLE")));

	memset(tile_data, 0, QCT_TILE_PIXELS);

	// Size for one whole row in image_data
	bytes_per_row = width * QCT_TILE_SIZE / scalefactor;

	// Calculate pointer into image_data for each row in this tile
	if (scalefactor == 1)
	{
		for (row=0; row<QCT_TILE_SIZE; row++)
		{
			// Top left corner of tile within image
			row_ptr[row] = image_data + (tile_yy * QCT_TILE_SIZE * bytes_per_row) + (tile_xx * QCT_TILE_SIZE);
			// Interleaved rows in the above sequence
			row_ptr[row] += row_seq[row] * bytes_per_row;
		}
	}
	else
	{
		for (row=0; row<QCT_TILE_SIZE; row++)
		{
			// Top left corner of tile within image
			row_ptr[row] = image_data + (tile_yy * QCT_TILE_SIZE / scalefactor * bytes_per_row) + (tile_xx * QCT_TILE_SIZE / scalefactor);
			// Interleaved rows in the above sequence
			row_ptr[row] += row_seq[row] * bytes_per_row;
		}
	}

	// Uncompress each row
	if (packing == 0 || packing == 255)
	{
		// Huffman
		//debugmsg("Huffman");
		int huff_size = 256;
		unsigned char *huff = (unsigned char *)malloc(huff_size);
		unsigned char *huff_ptr = huff;
		int huff_idx = 0;
		int num_colours = 0;
		int num_branches = 0;
		while (num_colours <= num_branches)
		{
			huff[huff_idx] = getc(fp);
			// Relative jump further than 128 needs two more bytes
			if (huff[huff_idx] == 128)
			{
				huff[++huff_idx] = getc(fp);
				huff[++huff_idx] = getc(fp);
				num_branches++;
			}
			// Relative jump nearer is encoded directly
			else if (huff[huff_idx] > 128)
			{
				// Count number of branches so we know when tree is built
				num_branches++;
			}
			// Otherwise it's a colour index into the palette
			else
			{
				// Count number of colours so we know when tree is built
				num_colours++;
			}
			huff_idx++;
			// Make space when Huffman table runs out of space
			if (huff_idx > huff_size)
			{
				huff_size += 256;
				huff = (unsigned char*)realloc(huff, huff_size);
			}
		}
		// If only 1 colour then tile is solid colour so no data follows
		if (num_colours == 1)
		{
			memset(tile_data, huff[0], QCT_TILE_PIXELS);
		}
		else
		{
			// Validate Huffman table by ensuring all branches are within table
			// (if not just return so tile will be not be unpacked, ie. blank)
			{
				int ii, delta;
				for (ii=0; ii<huff_idx; ii++)
				{
					if (huff[ii] < 128)
						continue;
					else if (huff[ii] == 128)
					{
						if (ii+2 >= huff_idx)
							return;
						delta = 65537 - (256 * huff[ii+2] + huff[ii+1]) + 2;
						if (ii+delta >= huff_idx)
							return;
						ii += 2;
					}
					else
					{
						delta = 257 - huff[ii];
						if (ii+delta >= huff_idx)
							return;
					}
				}
			}
			// Read tile data one bit at a time following branches in Huffman tree
			int bits_left = 8;
			int bit_value;
			ii = fgetc(fp);
			while (pixelnum < QCT_TILE_PIXELS)
			{
				// If entry is a colour then output it
				if (*huff_ptr < 128)
				{
					tile_data[pixelnum++] = *huff_ptr;
					// Go back to top of tree for next pixel
					huff_ptr = huff;
					continue;
				}
				// Get the next "bit"
				bit_value = (ii & 1);
				// Prepare for the following one
				ii >>= 1;
				bits_left--;
				if (bits_left == 0)
				{
					ii = fgetc(fp);
					bits_left = 8;
				}
				// Now check value to see whether to follow branch or not
				if (bit_value == 0)
				{
					// Don't jump just proceed to next entry in Huffman table
					if (*huff_ptr == 128) huff_ptr+=2;
					huff_ptr++;
				}
				else
				{
					// Follow branch to different part of Huffman table
					if (*huff_ptr > 128)
					{
						// Near jump
						huff_ptr += 257 - (*huff_ptr);
					}
					else if (*huff_ptr == 128)
					{
						// Far jump needs two more bytes
						int delta = 65537 - (256 * huff_ptr[2] + huff_ptr[1]) + 2;
						huff_ptr += delta;
					}
					// NB < 128 already handled above
				}
			}
		}
	}

	else if (packing > 128)
	{
		// Pixel packing
		int num_sub_colours = 256 - packing;
		int shift = bits_per_pixel(num_sub_colours);
		int mask = (1 << shift) - 1;
		int num_pixels_per_word = 32 / shift;
		int palette_index[256];
		debugmsg("PACKED: sub-palette size is %d (%d bits) shift=%d mask=%d numpixperword=%d", num_sub_colours, shift, shift, mask, num_pixels_per_word);
		// Read the sub-palette
		for (ii=0; ii<num_sub_colours; ii++)
		{
			palette_index[ii] = fgetc(fp);
			debugmsg("PACKED: palette %d = %d", ii, palette_index[ii]);
		}
		// Read the pixels in 4-byte words and unpack the bits from each
		while (pixelnum < QCT_TILE_PIXELS)
		{
			int colour, runs;
			ii = readInt(fp);
			for (runs = 0; runs < num_pixels_per_word; runs++)
		 	{
				colour = ii & mask;
				ii = ii >> shift;
				tile_data[pixelnum++] = palette_index[colour];
			}
		}
	}

	else if (packing == 128)
	{
		// An encrypted type of packing??
		debugmsg("unknown packing %02x %02x %02x %02x %02x %02x %02x %02x",
			fgetc(fp), fgetc(fp), fgetc(fp), fgetc(fp),
			fgetc(fp), fgetc(fp), fgetc(fp), fgetc(fp));
	}

	else
	{
		// Run-length Encoding
		int num_sub_colours = packing;
		int num_low_bits = bits_per_pixel(num_sub_colours);
		int pal_mask = (1 << num_low_bits)-1;
		int palette_index[256];
		//debugmsg("RLE: sub-palette size is %d (uses %d bits) mask 0x%x", num_sub_colours, num_low_bits, pal_mask);
		for (ii=0; ii<num_sub_colours; ii++)
		{
			palette_index[ii] = fgetc(fp);
			//debugmsg("RLE palette %d = %d", ii, palette_index[ii]);
		}
		while (pixelnum < QCT_TILE_PIXELS)
		{
			int colour, runs;
			ii = fgetc(fp);
			colour = ii & pal_mask;
			runs = ii >> num_low_bits;
			//debugmsg("RLE value 0x%x is colour %d for %d runs [%d..%d]", ii, colour, runs, pixelnum, pixelnum+runs);
			while (runs-- > 0)
			{
				tile_data[pixelnum++] = palette_index[colour];
			}
		}
	}

	// Decommutate the interleaved rows and copy into the image
	if (scalefactor == 1)
	{
		int yy;
		for (yy=0; yy<QCT_TILE_SIZE; yy++)
		{
			memcpy(row_ptr[yy], tile_data+yy*QCT_TILE_SIZE, QCT_TILE_SIZE);
		}
	}
	else
	{
		int xx, yy, nn;
		unsigned char *row;
		unsigned char *src = tile_data;
		for (yy=0; yy<QCT_TILE_SIZE/scalefactor; yy++)
		{
			unsigned char *dest = row_ptr[yy*scalefactor];
			unsigned char pix;
			src = tile_data + (yy*QCT_TILE_SIZE);
			// Interpolate the colours of all pixels to be combined
			// only does it horizontally in this row
			// XXX should interpolate all in corresponding rows below too.
			for (xx=0; xx<QCT_TILE_SIZE/scalefactor; xx++)
			{
				pix = *src++;
				for (nn=1; nn<scalefactor; nn++)
				{
					pix = pal_interp[pix][*src++];
				}
				*dest++ = pix;
			}
		}
	}
}


bool
QCT::loadMetadata(FILE *fp)
{
	int ii;

	// Read Metadata
	ii = readInt(fp);
	if (ii != QCT_MAGIC)
	{
		throwError("Not a QCT file (%x != %x)\n",ii,QCT_MAGIC);
		return false;
	}

	metadata.version = readInt(fp);
	width  = readInt(fp);
	height = readInt(fp);
	metadata.title      = readString(fp);
	metadata.name       = readString(fp);
	metadata.ident      = readString(fp);
	metadata.edition    = readString(fp);
	metadata.revision   = readString(fp);
	metadata.keywords   = readString(fp);
	metadata.copyright  = readString(fp);
	metadata.scale      = readString(fp);
	metadata.datum      = readString(fp);
	metadata.depths     = readString(fp);
	metadata.heights    = readString(fp);
	metadata.projection = readString(fp);
	metadata.flags      = readInt(fp);
	metadata.origfilename = readString(fp);
	metadata.origfilesize = readInt(fp);
	metadata.origfiletime = readInt(fp);
	metadata.unknown1     = readInt(fp);

	// Pointer to extended metadata
	{
		OFF_T current_position, extended_position;
		extended_position = readInt(fp);
		current_position = FTELLO(fp);
		FSEEKO(fp, extended_position, SEEK_SET);
		metadata.maptype = readString(fp);
		// Pointer to datum shift
		{
			OFF_T current_position, datum_shift_position;
			datum_shift_position = readInt(fp);
			current_position = FTELLO(fp);
			FSEEKO(fp, datum_shift_position, SEEK_SET);
			datum_shift_north = readDouble(fp);
			datum_shift_east  = readDouble(fp);
			FSEEKO(fp, current_position, SEEK_SET);
		}
		metadata.diskname = readString(fp);
		metadata.unknown2 = readInt(fp);
		metadata.unknown3 = readInt(fp);
		// Pointer to license structure
		{
			OFF_T current_position, license_position;
			license_position = readInt(fp);
			if (license_position)
			{
				current_position = FTELLO(fp);
				FSEEKO(fp, license_position, SEEK_SET);
				metadata.license_identifier = readInt(fp);
				readInt(fp);
				readInt(fp);
				metadata.license_description = readString(fp);
				// Pointer to license serial structure
				{
					OFF_T current_position, serial_position;
					serial_position = readInt(fp);
					if (serial_position)
					{
						current_position = FTELLO(fp);
						FSEEKO(fp, serial_position, SEEK_SET);
						metadata.license_serial = readInt(fp);
						FSEEKO(fp, current_position, SEEK_SET);
					}
				}
				readInt(fp);
				// 16 bytes
				// 64 bytes
				FSEEKO(fp, current_position, SEEK_SET);
			}
		}
		metadata.associateddata = readString(fp);
		metadata.unknown6 = readInt(fp);
		FSEEKO(fp, current_position, SEEK_SET);
	}

	// Map outline
	metadata.num_outline = readInt(fp);  // number of map outline points
	metadata.outline_lat = (double*)calloc(metadata.num_outline, sizeof(double));
	metadata.outline_lon = (double*)calloc(metadata.num_outline, sizeof(double));
	if (metadata.outline_lat == NULL || metadata.outline_lon == NULL)
		return false;

	// Pointer to map outline
	{
		int outline;
		OFF_T current_position, outline_position;
		outline_position = readInt(fp);
		current_position = FTELLO(fp);
		FSEEKO(fp, outline_position, SEEK_SET);
		for (outline=0; outline<metadata.num_outline; outline++)
		{
			metadata.outline_lat[outline] = readDouble(fp);
			metadata.outline_lon[outline] = readDouble(fp);
		}
		FSEEKO(fp, current_position, SEEK_SET);
	}

	// Georeferencing ceofficients

	eas = readDouble(fp);
	easY = readDouble(fp);
	easX = readDouble(fp);
	easYY = readDouble(fp);
	easXY = readDouble(fp);
	easXX = readDouble(fp);
	easYYY = readDouble(fp);
	easXYY = readDouble(fp);
	easXXY = readDouble(fp);
	easXXX = readDouble(fp);

	nor = readDouble(fp);
	norY = readDouble(fp);
	norX = readDouble(fp);
	norYY = readDouble(fp);
	norXY = readDouble(fp);
	norXX = readDouble(fp);
	norYYY = readDouble(fp);
	norXYY = readDouble(fp);
	norXXY = readDouble(fp);
	norXXX = readDouble(fp);

	lat = readDouble(fp);
	latX = readDouble(fp);
	latY = readDouble(fp);
	latXX = readDouble(fp);
	latXY = readDouble(fp);
	latYY = readDouble(fp);
	latXXX = readDouble(fp);
	latXXY = readDouble(fp);
	latXYY = readDouble(fp);
	latYYY = readDouble(fp);

	lon = readDouble(fp);
	lonX = readDouble(fp);
	lonY = readDouble(fp);
	lonXX = readDouble(fp);
	lonXY = readDouble(fp);
	lonYY = readDouble(fp);
	lonXXX = readDouble(fp);
	lonXXY = readDouble(fp);
	lonXYY = readDouble(fp);
	lonYYY = readDouble(fp);

	// Palette
	for (ii=0; ii<256; ii++)
	{
		palette[ii] = readInt(fp);
	}

	// Interpolation matrix (128 x 128)
	for (ii=0; ii<128; ii++) for (int jj=0; jj<128; jj++)
	{
		pal_interp[ii][jj] = (unsigned char)fgetc(fp);
	}

	// Image index (width * height offsets)
	metadata.image_index = (int*)calloc(width * height, sizeof(int*));
	if (metadata.image_index == NULL)
		return false;
	for (ii=0; ii<width * height; ii++)
		metadata.image_index[ii] = readInt(fp);

	return true;
}


bool
QCT::loadImage(int scale)
{
	int xx, yy;

	if (qctfp == NULL)
		return false;

	scalefactor = scale;

	image_data = (unsigned char*)calloc(height*QCT_TILE_SIZE, width*QCT_TILE_SIZE);
	if (image_data == NULL)
		return false;

	for (yy=0; yy<height; yy++)
	{
		for (xx=0; xx<width; xx++)
		{
			OFF_T tile_offset;
			tile_offset = metadata.image_index[yy*width+xx];
			FSEEKO(qctfp, tile_offset, SEEK_SET);
			//debugmsg("Would read tile %d, %d packing %d at offset %x", xx, yy, fgetc(fp), tile_offset);
			readTile(qctfp, xx, yy, scalefactor);
		}
	}

	return true;
}


bool
QCT::readFile(FILE *fp, bool headeronly, int scale)
{
	scalefactor = scale;

	if (!loadMetadata(fp))
		return false;

	// Don't read and unpack image data if not required
	if (headeronly)
		return true;

	return loadImage(scalefactor);
}


bool
QCT::openFilename(const char *filename, bool headeronly, int scalefactor)
{
	bool truth;

	qctfp = fopen(filename, "rb");
	if (qctfp == NULL)
	{
		throwError("cannot open %s (%s)", filename, strerror(errno));
		return false;
	}
	truth = readFile(qctfp, headeronly, scalefactor);
	return(truth);
}


/* -------------------------------------------------------------------------
 */
void
QCT::printMetadata(FILE *fp)
{
	int ii;

	dfp = fp;
	verbose++;

	message("Version     %d", metadata.version);
	message("Width:      %d tiles (%d pixels)", width, width*QCT_TILE_SIZE);
	message("Height:     %d tiles (%d pixels)", height, height*QCT_TILE_SIZE);
	message("Title:      %s", metadata.title);
	message("Name:       %s", metadata.name);
	message("Identifier: %s", metadata.ident);
	message("Edition:    %s", metadata.edition);
	message("Revision:   %s", metadata.revision);
	message("Keywords:   %s", metadata.keywords);
	message("Copyright:  %s", metadata.copyright);
	message("Scale:      %s", metadata.scale);
	message("Datum:      %s", metadata.datum);
	message("Depths:     %s", metadata.depths);
	message("Heights:    %s", metadata.heights);
	message("Projection: %s", metadata.projection);
	message("Flags:      0x%x", metadata.flags);
	message("OriginalFileName:    %s", metadata.origfilename);
	message("OriginalFileSize     %d bytes", metadata.origfilesize);
	message("OriginalFileCreation %s", ctime(&metadata.origfiletime));
	message("MapType:    %s", metadata.maptype);
	message("DiskName:   %s", metadata.diskname);
	message("AssocData:  %s", metadata.associateddata);
	message("LicIdent:   %d", metadata.license_identifier);
	message("LicDesc:    %s", metadata.license_description);
	message("LicSerial:  %d", metadata.license_serial);
	message("Unknown:    %d", metadata.unknown1);
	message("Unknown:    %d", metadata.unknown2);
	message("Unknown:    %d", metadata.unknown3);
	message("Unknown:    %d", metadata.unknown4);
	message("Unknown:    %d", metadata.unknown5);
	message("Unknown:    %d", metadata.unknown6);

	// Palette
	for (ii=0; ii<256; ii++)
	{
		if (palette[ii]) message("Colour %d = %6x", ii, palette[ii]);
	}

	// Outline
	message("OutlinePts: %d", metadata.num_outline);
	{
		double outline_lat_min = 99, outline_lat_max = -99;
		double outline_lon_min = 399, outline_lon_max = -399;
		FILE *kml = NULL;
		if (debug_kml_outline) kml = fopen("outline.kml", "w");
		if (kml) fprintf(kml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			"<kml xmlns=\"http://earth.google.com/kml/2.0\">\n"
			"<Document>\n"
			"<name>Outline</name>\n"
			"<description>Outline</description>\n"
			"<Style><LineStyle><color>ffffff00</color><width>6</width></LineStyle></Style>\n"
			"<Placemark>\n"
			"<name>Outline</name>\n"
			"<description>Outline</description>\n"
			"<LineString>\n"
			"<coordinates>");
		for (ii=0; ii<metadata.num_outline; ii++)
		{
			message(" %3.9f %3.9f", metadata.outline_lat[ii], metadata.outline_lon[ii]);
			if (kml) fprintf(kml, "%f,%f,%f ", metadata.outline_lon[ii], metadata.outline_lat[ii], 0.0);
			if (metadata.outline_lat[ii] > outline_lat_max) outline_lat_max = metadata.outline_lat[ii];
			if (metadata.outline_lat[ii] < outline_lat_min) outline_lat_min = metadata.outline_lat[ii];
			if (metadata.outline_lon[ii] > outline_lon_max) outline_lon_max = metadata.outline_lon[ii];
			if (metadata.outline_lon[ii] < outline_lon_min) outline_lon_min = metadata.outline_lon[ii];
		}
		if (kml)
		{
			fprintf(kml, "</coordinates>\n</LineString>\n</Placemark>\n</Document>\n</kml>\n");
			fclose(kml);
		}
		message("OutlineLat %f to %f", outline_lat_min, outline_lat_max);
		message("OutlineLon %f to %f", outline_lon_min, outline_lon_max);
	}

	// Georeferencing
	message("GeoTopLeftLonLat:    %f %f", lon, lat);
	message("GeoTopLeftEastNorth: %f %f", eas, nor);
	message("DatumShiftEastNorth: %f %f\n", datum_shift_east, datum_shift_north);

	// Calculate corner coordinates
	{
		double lat, lon;
		xy_to_latlon(0, 0, &lat, &lon);
		message("TL  %f, %f", lat, lon);
		xy_to_latlon(width*QCT_TILE_SIZE-1, 0, &lat, &lon);
		message("TR  %f, %f", lat, lon);
		xy_to_latlon(0, height*QCT_TILE_SIZE-1, &lat, &lon);
		message("BL  %f, %f", lat, lon);
		xy_to_latlon(width*QCT_TILE_SIZE-1, height*QCT_TILE_SIZE-1, &lat, &lon);
		message("BR  %f, %f", lat, lon);
	}
}


/* -------------------------------------------------------------------------
 */
bool
QCT::coordInsideMap(double lat, double lon)
{
	unsigned int intpoly[metadata.num_outline][2];
	unsigned int intlat, intlon;
	int ii, rc;

	// Need at least 3 points to make a polygon!
	if (metadata.num_outline < 3)
		return false;

	// Convert all real numbers to positive integers
#define LAT_TO_INT(L) (unsigned int)((L+90.0) * 1e3)
#define LON_TO_INT(L) (unsigned int)((L+180.0) * 1e3)
	intlat = LAT_TO_INT(lat);
	intlon = LON_TO_INT(lon);

	for (ii=0; ii<metadata.num_outline; ii++)
	{
		intpoly[ii][0] = LON_TO_INT(metadata.outline_lon[ii]);
		intpoly[ii][1] = LAT_TO_INT(metadata.outline_lat[ii]);
	}

	// Fast check if point within polygon
	rc = inpoly(intpoly, metadata.num_outline, intlon, intlat);

	return (rc ? true : false);
}


/* -------------------------------------------------------------------------
 */
bool
QCT::writePPMFile(FILE *fp)
{
	int xx, yy, colour;
	unsigned char *image_ptr = image_data;

	// PPM file header (for raw data not ASCII)
	fprintf(fp, "P6 %d %d 255\n",
		width*QCT_TILE_SIZE,
		height*QCT_TILE_SIZE);

	// Expand palette to R,G,B for each pixel
	for (yy=0; yy<height*QCT_TILE_SIZE; yy++)
	{
		for (xx=0; xx<width*QCT_TILE_SIZE; xx++)
		{
			colour = palette[*image_ptr++];
			fputc(PAL_RED(colour), fp);
			fputc(PAL_GREEN(colour), fp);
			fputc(PAL_BLUE(colour), fp);
		}
	}

	if (ferror(fp))
		return false;

	return(true);
}


bool
QCT::writePPMFilename(const char *filename)
{
	FILE *fp;
	bool truth;

	fp = fopen(filename, "wb");
	if (fp == NULL)
	{
		throwError("cannot open %s (%s)", filename, strerror(errno));
		return false;
	}
	truth = writePPMFile(fp);
	if (fclose(fp))
	{
		throwError("cannot write %s (%s)", filename, strerror(errno));
		truth = false;
	}
	return(truth);
}


/* -------------------------------------------------------------------------
 */
bool
QCT::writeGIFFile(FILE *fp)
{
#ifdef USE_GIFLIB
	unsigned char cmap[3][256];
	unsigned char *image_data_ptr = image_data;
	int background = 0;
	int transparent = -1;
	int xx, yy;

	for (xx=0; xx<256; xx++)
	{
		cmap[0][xx] = PAL_RED(palette[xx]);
		cmap[1][xx] = PAL_GREEN(palette[xx]);
		cmap[2][xx] = PAL_BLUE(palette[xx]);
	}

	gifout_open_file(fp, width*QCT_TILE_SIZE, height*QCT_TILE_SIZE, 256, cmap, background, transparent);
	gifout_open_image(0, 0, width*QCT_TILE_SIZE, height*QCT_TILE_SIZE);

	for (yy=0; yy<height*QCT_TILE_SIZE; yy++)
	{
		for (xx=0; xx<width*QCT_TILE_SIZE; xx++)
			gifout_put_pixel(*image_data_ptr++);
	}

	gifout_close_image();
	gifout_close_file();
	return true;
#else
	throwError("cannot write file (GIF not supported)");
	return false;
#endif
}


bool
QCT::writeGIFFilename(const char *filename)
{
	FILE *fp;
	bool truth;

	fp = fopen(filename, "wb");
	if (fp == NULL)
	{
		throwError("cannot open %s (%s)", filename, strerror(errno));
		return false;
	}
	truth = writeGIFFile(fp);
	if (fclose(fp))
	{
		throwError("cannot write %s (%s)", filename, strerror(errno));
		truth = false;
	}
	return(truth);
}


/* -------------------------------------------------------------------------
 */
bool
QCT::writePNGFile(FILE *fp)
{
#ifdef USE_PNG
	int ii;

	png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
	if (!png_ptr)
		return(false);

	png_infop info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
	{
		png_destroy_write_struct(&png_ptr, (png_infopp)NULL);
		return (false);
	}

	// Any errors inside png_ functions will return here
	if (setjmp(png_jmpbuf(png_ptr)))
	{
		throwError("PNG file write error\n");
		return false;
	}

	png_init_io(png_ptr, fp);

	int bit_depth = 8;
	png_set_IHDR(png_ptr, info_ptr, width*QCT_TILE_SIZE, height*QCT_TILE_SIZE,
		bit_depth, PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

	int num_palette = 256;
	png_color pal[num_palette];
	for (ii=0; ii<256; ii++)
	{
		pal[ii].red   = PAL_RED(palette[ii]);
		pal[ii].green = PAL_GREEN(palette[ii]);
		pal[ii].blue  = PAL_BLUE(palette[ii]);
	}
	png_set_PLTE(png_ptr, info_ptr, pal, num_palette);

	png_byte *row_pointers[height*QCT_TILE_SIZE];
	for (ii=0; ii<height*QCT_TILE_SIZE; ii++)
		row_pointers[ii]=image_data + width*QCT_TILE_SIZE*ii;
	png_set_rows(png_ptr, info_ptr, row_pointers);

	png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);

	png_write_end(png_ptr, info_ptr);
	png_destroy_write_struct(&png_ptr, &info_ptr);

	return true;
#else
	throwError("cannot write file (PNG not supported)");
	return false;
#endif
}


bool
QCT::writePNGFilename(const char *filename)
{
	FILE *fp;
	bool truth;

	fp = fopen(filename, "wb");
	if (fp == NULL)
	{
		throwError("cannot open %s (%s)", filename, strerror(errno));
		return false;
	}
	truth = writePNGFile(fp);
	if (fclose(fp))
	{
		throwError("cannot write %s (%s)", filename, strerror(errno));
		truth = false;
	}
	return(truth);
}


/* -------------------------------------------------------------------------
 */
bool
QCT::writeTIFFFile(FILE *fp)
{
#ifdef USE_TIFF
	throwError("cannot write file (TIFF not implemented)");
	return false;
#else
	throwError("cannot write file (TIFF not supported)");
	return false;
#endif
}


bool
QCT::writeTIFFFilename(const char *filename)
{
	FILE *fp;
	bool truth;

	fp = fopen(filename, "wb");
	if (fp == NULL)
	{
		throwError("cannot open %s (%s)", filename, strerror(errno));
		return false;
	}
	truth = writeTIFFFile(fp);
	if (fclose(fp))
	{
		throwError("cannot write %s (%s)", filename, strerror(errno));
		truth = false;
	}
	return(truth);
}


/* -------------------------------------------------------------------------
 * Convert pixel (x,y) in image into (longitude,latitude)
 * x,y from top left
 * latitude,longitude in degrees WGS84.
 * clips out of range x,y rather than blindly converting.
 */
int
QCT::xy_to_latlon(int x, int y, double *latitude, double *longitude) const
{
	int x2, y2, x3, y3, xy;

	x *= scalefactor;
	y *= scalefactor;

	// Range check - clip to image limits, or allow extrapolation?
	if (x<0) x=0;
	if (y<0) y=0;
	if (x>=width*QCT_TILE_SIZE) x=width*QCT_TILE_SIZE-1;
	if (y>=height*QCT_TILE_SIZE) y=height*QCT_TILE_SIZE;

	x2 = x * x;
	x3 = x2 * x;
	y2 = y * y;
	y3 = y2 * y;
	xy = x * y;

	// Python:
	// coeffs in order are: c, cy, cx, cy2, cxy, cx2, cy3, cy2x, cyx2, cx3
	// There are coefficients for x and for y
	// each coordinate is calculated as
	// c + cy*y + cx*x + cy2*y^2 + cxy*x*y + cx2*x^2 + cy3*y^3 + cy2x*(y^2)*x + cyx2*y*x^2 + cx3*x^3
	// c0 + c1*y + c2*x + c3*y^2 + c4*x*y + c5*x^2 + c6*y^3 + c7*(y^2)*x + c8*y*x^2 + c9*x^3

	// Touratech:
	// Substitute into a and b as follows:
	// 	x and y to latitude or longitude: x and y
	// 	lat and lon to x or y: lat and lon

	// x = c0 + c1*x + c2*y + c3*x2 + c4*x*y + c5*y2 + c6*x3 + c7*x2*y + c8*x*y2 + c9*y3
	// y = c0 + c1*x + c2*y + c3*x2 + c4*x*y + c5*y2 + c6*x3 + c7*x2*y + c8*x*y2 + c9*y3
	// lat = c0 + c1*x + c2*y + c3*x2 + c4*x*y + c5*y2 + c6*x3 + c7*x2*y + c8*x*y2 + c9*y3
	// lon = c0 + c1*x + c2*y + c3*x2 + c4*x*y + c5*y2 + c6*x3 + c7*x2*y + c8*x*y2 + c9*y3

	// New equations (1.01) :
	// eas, easY, easX, easYY, easXY, easXX, easYYY, easYYX, easYXX, easXXX;
	// nor, norY, norX, norYY, norXY, norXX, norYYY, norYYX, norYXX, norXXX;
	// lat, latX, latY, latXX, latXY, latYY, latXXX, latXXY, latXYY, latYYY;
	// lon, lonX, lonY, lonXX, lonXY, lonYY, lonXXX, lonXXY, lonXYY, lonYYY;
	*longitude = lon + lonX * x + lonY * y + lonXX * x2 + lonXY * x * y +
		lonYY * y2 + lonXXX * x3 + lonXXY * x2 * y + lonXYY * x * y2 + lonYYY * y3;
	*latitude = lat + latX * x + latY * y + latXX * x2 + latXY * x * y +
		latYY * y2 + latXXX * x3 + latXXY * x2 * y + latXYY * x * y2 + latYYY * y3;
	// Mistyped in doc:
#if 0
	*longitude = lonXXX * x3 + lonXX * x2 + lonX * x +
		lonYYY * y3 + lonYY * y2 + lonY * y +
		lonXXY * x2 * y + lonYYX * y2 * x + lonXY * xy +
		lon;
	*latitude = latXXX * x3 + latXX * x2 + latX * x +
		latYYY * y3 + latYY * y2 + latY * y +
		latXXY * x2 * y + latYYX * y2 * x + latXY * xy +
		lat;
#endif

	// Old equations (1.00) :
#if 0
	x2 = x * x;
	y2 = y * y;
	R = lonX2 * x2 + lonY2 * y2 + lonX * x + lonY * y + lonXY * x * y + lon;
	S = latX2 * x2 + latY2 * y2 + latX * x + latY * y + latXY * x * y + lat;
	R2 = R * R;
	S2 = S * S;
	T = easX2 * R2 + easY2 * S2 + easX * R + easY * S + easXY * R * S + eas + 2 * x;
	U = norX2 * R2 + norY2 * S2 + norX * R + norY * S + norXY * R * S + nor + 2 * y;
	T2 = T * T;
	U2 = U * U;
	*longitude = lonX2 * T2 + lonY2 * U2 + lonX * T + lonY * U + lonXY * T * U + lon;
	*latitude  = latX2 * T2 + latY2 * U2 + latX * T + latY * U + latXY * T * U + lat;
#endif

	// Add the datum shift
	*longitude += datum_shift_east;
	*latitude  += datum_shift_north;

	return(0);
}


int
QCT::latlon_to_xy(double latitude, double longitude, int *pixel_x, int *pixel_y) const
{
	double longitude2, latitude2, longitude3, latitude3, longitudelatitude;

	// Range check - clip to image limits, or allow extrapolation?
	if (latitude<-90)   latitude=-90;
	if (latitude>90)    latitude=90;
	if (longitude<-360) longitude=0;
	if (longitude>360)  longitude=0;

	longitude2 = longitude * longitude;
	longitude3 = longitude2 * longitude;
	latitude2 = latitude * latitude;
	latitude3 = latitude2 * latitude;
	longitudelatitude = longitude * latitude;

	// Remove the datum shift
	longitude -= datum_shift_east;
	latitude  -= datum_shift_north;

	*pixel_x = NINT(eas + easX * longitude + easY * latitude + easXX * longitude2 + easXY * longitude * latitude +
		easYY * latitude2 + easXXX * longitude3 + easXXY * longitude2 * latitude + easXYY * longitude * latitude2 + easYYY * latitude3);
	*pixel_y = NINT(nor + norX * longitude + norY * latitude + norXX * longitude2 + norXY * longitude * latitude +
		norYY * latitude2 + norXXX * longitude3 + norXXY * longitude2 * latitude + norXYY * longitude * latitude2 + norYYY * latitude3);

	*pixel_x /= scalefactor;
	*pixel_y /= scalefactor;

	return(-1);
}


double
QCT::getDegreesPerPixel() const
{
	if (width<1 || height<1)
		return 0;
	int y = height/2;
	int w = (width * QCT_TILE_SIZE / scalefactor);
	double lat0,lon0, lat1,lon1;
	// Find the longitude of the left edge and right edge
	// mid-way up the image and divide long range by width in pixels
	xy_to_latlon(0, y, &lat0, &lon0);
	xy_to_latlon(w-1, y, &lat1, &lon1);
	double dpp = (double)fabs(lon1 - lon0) / (double)(w);
	return (fabs(lon1 - lon0) / (w));
}


/* -------------------------------------------------------------------------
 */
