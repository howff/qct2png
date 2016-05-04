/* > qct.h
 */


#ifndef QCT_H
#define QCT_H


#define QCT_MAGIC 0x1423D5FF
#define QCT_TILE_SIZE 64
#define QCT_TILE_PIXELS (QCT_TILE_SIZE*QCT_TILE_SIZE)
// RGB stored packed in an int, Blue is LSB
#define PAL_RED(c)   ((c>>16)&255)
#define PAL_GREEN(c) ((c>>8)&255)
#define PAL_BLUE(c)  ((c)&255)


/* -------------------------------------------------------------------------
 * Class to read a QCT map image.
 *
 * Create a QCT object
 * Call openFilename to open the file, read the header and metadata, and
 * if requested read the image data too.
 * If image data not read at this stage then later call loadImage.
 * To reload the image at a new scale call unloadImage then loadImage.
 * Call closeFilename when you've completely finished.
 */
class QCT
{
public:
	QCT();
	~QCT();

protected:
	void throwError(const char *fmt, ...);
	void message(const char *fmt, ...);
	void debugmsg(const char *fmt, ...);

public:
	// Reading methods:
	bool openFilename(const char *filename, bool headeronly = false, int scale = 1);
	bool loadImage(int scale);
	void unloadImage();
	void closeFilename();

	// Information:
	void setDebug(int d)     { debug = d; }
	void setVerbose(int v)   { verbose = v; }
	void printMetadata(FILE *fp);

	// Writing methods:
	bool writePPMFile(FILE *);
	bool writePPMFilename(const char *filename);
	bool writeGIFFile(FILE *);
	bool writeGIFFilename(const char *filename);
	bool writePNGFile(FILE *);
	bool writePNGFilename(const char *filename);
	bool writeTIFFFile(FILE *);
	bool writeTIFFFilename(const char *filename);

	// Query methods:
	int getImageWidth()       { return width * QCT_TILE_SIZE / scalefactor; }
	int getImageHeight()      { return height * QCT_TILE_SIZE / scalefactor; }
	unsigned char *getImage() { return image_data; }
	bool getColour(int index, int *R, int *G, int *B)
	                          { if (index<0||index>127) return false;
	                          *R = PAL_RED(palette[index]);
	                          *G = PAL_GREEN(palette[index]);
	                          *B = PAL_BLUE(palette[index]);
	                          return true; }
							 
	// Query geolocation methods:
	int xy_to_latlon(int pixel_x, int pixel_y, double *lat, double *lon) const;
	int latlon_to_xy(double lat, double lon, int *pixel_x, int *pixel_y) const;
	double getDegreesPerPixel() const;

	// Query metadata methods:
	char *getTitle()          { return metadata.title; }
	char *getName()           { return metadata.name; }
	char *getIdentifier()     { return metadata.ident; }
	char *getProjection()     { return metadata.projection; }
	bool coordInsideMap(double lat, double lon);
	// Query map boundary:
	int  getOutlineSize() const { return metadata.num_outline; }
	void getOutlinePoint(int i, double *lat, double *lon) const
		{ if (i>=0 && i<metadata.num_outline) { *lat=metadata.outline_lat[i]; *lon=metadata.outline_lon[i]; } }
	void getOutlinePoints(double *lat, double *lon) const
		{ for (int i=0; i<metadata.num_outline; i++) { lat[i]=metadata.outline_lat[i]; lon[i]=metadata.outline_lon[i]; } }

private:
	bool readFile(FILE *, bool headeronly, int scale);
	void readTile(FILE *, int tile_x, int tile_y, int scale);
	bool loadMetadata(FILE *fp);
	void unload();
	void unloadMetadata();

private:
	FILE *qctfp;
	int width, height;         // size in tiles (of 64x64 each)
	int palette[256];          // combined RGB in each int
	unsigned char pal_interp[128][128];
	unsigned char *image_data; // one pixel per byte
	int scalefactor;           // reduction factor
	// Metadata
	struct
	{
		int  version;
		char *title, *name, *ident, *edition, *revision;
		char *keywords, *copyright, *scale, *datum;
		char *depths, *heights, *projection;
		int  flags;
		char *origfilename;
		int  origfilesize;
		time_t origfiletime;
		int  unknown1;
		char *maptype, *diskname, *associateddata;
		// License
		int   license_identifier;
		char *license_description;
		int   license_serial;
		int  unknown2, unknown3, unknown4, unknown5, unknown6;
		// Outline
		int num_outline;
		double *outline_lat, *outline_lon;
		// Offsets to tile data
		int *image_index;
	} metadata;
	// Georeferencing coefficients
	double eas, easY, easX, easYY, easXY, easXX, easYYY, easXYY, easXXY, easXXX;
	double nor, norY, norX, norYY, norXY, norXX, norYYY, norXYY, norXXY, norXXX;
	double lat, latX, latY, latXX, latXY, latYY, latXXX, latXXY, latXYY, latYYY;
	double lon, lonX, lonY, lonXX, lonXY, lonYY, lonXXX, lonXXY, lonXYY, lonYYY;
	double datum_shift_north, datum_shift_east;
	// Program options
	int verbose, debug, debug_kml_outline, debug_kml_boundary;
	FILE *dfp; // debug output goes here
};


#endif // !QCT_H
