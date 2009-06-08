/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenSlide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking OpenSlide statically or dynamically with other modules is
 *  making a combined work based on OpenSlide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

#include "config.h"

#include "openslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>

#include <openjpeg/openjpeg.h>

static const char APERIO_DESCRIPTION[] = "Aperio";

struct _openslide_tiff_tilereader {
  TIFF *tiff;
  int64_t tile_width;
  int64_t tile_height;
};

static void info_callback(const char *msg, void *data) {
  g_message("%s", msg);
}
static void warning_callback(const char *msg, void *data) {
  g_warning("%s", msg);
}
static void error_callback(const char *msg, void *data) {
  g_critical("%s", msg);
}

// XXX revisit assumptions that color is always downsampled in x by 2
static struct _openslide_tiff_tilereader *_openslide_aperio_tiff_tilereader_create(TIFF *tiff) {
  struct _openslide_tiff_tilereader *wtt = g_slice_new(struct _openslide_tiff_tilereader);
  uint32_t tmp;

  wtt->tiff = tiff;

  TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tmp);
  wtt->tile_width = tmp;
  TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tmp);
  wtt->tile_height = tmp;

  return wtt;
}

static void _openslide_aperio_tiff_tilereader_read(struct _openslide_tiff_tilereader *wtt,
						   uint32_t *dest,
						   int64_t x, int64_t y) {
  // get tile number
  ttile_t tile_no = TIFFComputeTile(wtt->tiff, x, y, 0, 0);

  //  g_debug("aperio reading tile_no: %d", tile_no);

  // get tile size
  tsize_t max_tile_size = TIFFTileSize(wtt->tiff);

  // get raw tile
  tdata_t buf = g_slice_alloc(max_tile_size);
  tsize_t size = TIFFReadRawTile(wtt->tiff, tile_no, buf, max_tile_size); // XXX?

  // init decompressor
  opj_dparameters_t parameters;
  opj_dinfo_t *dinfo = opj_create_decompress(CODEC_J2K);
  opj_set_default_decoder_parameters(&parameters);
  opj_setup_decoder(dinfo, &parameters);
  opj_cio_t *stream = opj_cio_open((opj_common_ptr) dinfo, buf, size);

  opj_event_mgr_t event_callbacks = {
    .error_handler = error_callback,
    .warning_handler = warning_callback,

    /* don't user info_handler, it outputs lots of junk */
    //.info_handler = info_callback,
  };
  opj_set_event_mgr((opj_common_ptr) dinfo, &event_callbacks, NULL);


  // decode
  opj_image_t *image = opj_decode(dinfo, stream);

  opj_image_comp_t *comps = image->comps;

  // copy
  for (int i = 0; i < wtt->tile_height * wtt->tile_width; i++) {
    uint8_t Y = comps[0].data[i];
    uint8_t Cb = comps[1].data[i/2];
    uint8_t Cr = comps[2].data[i/2];

    uint8_t A = 255;
    double R = Y + 1.402 * (Cr - 128);
    double G = Y - 0.34414 * (Cb - 128) - 0.71414 * (Cr - 128);
    double B = Y + 1.772 * (Cb - 128);

    if (R > 255) {
      R = 255;
    }
    if (R < 0) {
      R = 0;
    }
    if (G > 255) {
      G = 255;
    }
    if (G < 0) {
      G = 0;
    }
    if (B > 255) {
      B = 255;
    }
    if (B < 0) {
      B = 0;
    }

    dest[i] = A << 24 | ((uint8_t) R << 16) | ((uint8_t) G << 8) | ((uint8_t) B);
  }

  // erase
  g_slice_free1(max_tile_size, buf);
  opj_image_destroy(image);
  opj_cio_close(stream);
  opj_destroy_decompress(dinfo);
}

static void _openslide_aperio_tiff_tilereader_destroy(struct _openslide_tiff_tilereader *wtt) {
  g_slice_free(struct _openslide_tiff_tilereader, wtt);
}


static void add_properties(GHashTable *ht, char **props) {
  if (*props == NULL) {
    return;
  }

  // ignore first property in Aperio
  for(char **p = props + 1; *p != NULL; p++) {
    char **pair = g_strsplit(*p, "=", 2);

    if (pair) {
      char *name = g_strstrip(pair[0]);
      if (name) {
	char *value = g_strstrip(pair[1]);

	g_hash_table_insert(ht,
			    g_strdup_printf("aperio.%s", name),
			    g_strdup(value));
      }
    }
    g_strfreev(pair);
  }
}

// add the image from the current TIFF directory
static void add_associated_image(GHashTable *ht, const char *name_if_available,
				 TIFF *tiff) {
  char *name = NULL;
  if (name_if_available) {
    name = g_strdup(name_if_available);
  } else {
    char *val;

    // get name
    if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &val)) {
      return;
    }

    // parse ImageDescription, after newline up to first whitespace -> gives name
    char **lines = g_strsplit_set(val, "\r\n", -1);
    if (!lines) {
      return;
    }

    if (lines[0] && lines[1]) {
      char *line = lines[1];

      char **names = g_strsplit(line, " ", -1);
      if (names && names[0]) {
	name = g_strdup(names[0]);
      }
      g_strfreev(names);
    }

    g_strfreev(lines);
  }


  // if we have a name, this is probably valid
  if (name) {
    uint32_t tmp;

    // get the dimensions
    if (!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &tmp)) {
      g_free(name);
      return;
    }
    int64_t w = tmp;

    if (!TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &tmp)) {
      g_free(name);
      return;
    }
    int64_t h = tmp;

    // get the image
    uint32_t *img_data = g_malloc(w * h * 4);
    if (!TIFFReadRGBAImageOriented(tiff, w, h, img_data, ORIENTATION_TOPLEFT, 0)) {
      g_free(name);
      g_free(img_data);
      return;
    }

    // permute
    uint32_t *p = img_data;
    uint32_t *end = img_data + (w * h);
    while (p < end) {
      uint32_t val = p;
      *p++ = (val & 0xFF00FF00)
	| ((val << 16) & 0xFF0000)
	| ((val >> 16) & 0xFF);
    }

    // load into struct
    struct _openslide_associated_image *aimg =
      g_slice_new(struct _openslide_associated_image);
    aimg->w = w;
    aimg->h = h;
    aimg->argb_data = img_data;

    // save
    g_hash_table_insert(ht, name, aimg);
  }
}


bool _openslide_try_aperio(openslide_t *osr, const char *filename) {
  char *tagval;

  // first, see if it's a TIFF
  TIFF *tiff = TIFFOpen(filename, "r");
  if (tiff == NULL) {
    return false; // not TIFF, not aperio
  }

  int tiff_result;
  tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);
  if (!tiff_result ||
      (strncmp(APERIO_DESCRIPTION, tagval, strlen(APERIO_DESCRIPTION)) != 0)) {
    // not aperio
    TIFFClose(tiff);
    return false;
  }

  /*
   * http://www.aperio.com/documents/api/Aperio_Digital_Slides_and_Third-party_data_interchange.pdf
   * page 14:
   *
   * The first image in an SVS file is always the baseline image (full
   * resolution). This image is always tiled, usually with a tile size
   * of 240 x 240 pixels. The second image is always a thumbnail,
   * typically with dimensions of about 1024 x 768 pixels. Unlike the
   * other slide images, the thumbnail image is always
   * stripped. Following the thumbnail there may be one or more
   * intermediate "pyramid" images. These are always compressed with
   * the same type of compression as the baseline image, and have a
   * tiled organization with the same tile size.
   *
   * Optionally at the end of an SVS file there may be a slide label
   * image, which is a low resolution picture taken of the slide’s
   * label, and/or a macro camera image, which is a low resolution
   * picture taken of the entire slide. The label and macro images are
   * always stripped.
   */

  // for aperio, the tiled layers are the ones we want
  int32_t layer_count = 0;
  int32_t *layers = NULL;
  do {
    if (TIFFIsTiled(tiff)) {
      layer_count++;
    }
  } while (TIFFReadDirectory(tiff));
  layers = g_new(int32_t, layer_count);

  TIFFSetDirectory(tiff, 0);
  int32_t i = 0;
  do {
    if (TIFFIsTiled(tiff)) {
      layers[i++] = TIFFCurrentDirectory(tiff);
      //g_debug("tiled layer: %d", TIFFCurrentDirectory(tiff));
    } else {
      // associated image
      if (osr) {
	char *name = (i == 1) ? "thumbnail" : NULL;
	add_associated_image(osr->associated_images, name, tiff);
      }
      //g_debug("associated image: %d", TIFFCurrentDirectory(tiff));
    }
  } while (TIFFReadDirectory(tiff));

  // read properties
  if (osr) {
    TIFFSetDirectory(tiff, 0);
    TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);
    char **props = g_strsplit(tagval, "|", -1);
    add_properties(osr->properties, props);
    g_strfreev(props);
  }

  // all set, load up the TIFF-specific ops
  TIFFSetDirectory(tiff, 0);
  uint16_t compression_mode;
  TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression_mode);

  //  g_debug("compression mode: %d", compression_mode);

  if (compression_mode == 33003) {
    // special jpeg 2000 aperio thing
    _openslide_add_tiff_ops(osr, tiff, 0, NULL, layer_count, layers,
			    _openslide_aperio_tiff_tilereader_create,
			    _openslide_aperio_tiff_tilereader_read,
			    _openslide_aperio_tiff_tilereader_destroy);
  } else {
    // let libtiff handle it
    _openslide_add_tiff_ops(osr, tiff, 0, NULL, layer_count, layers,
			    _openslide_generic_tiff_tilereader_create,
			    _openslide_generic_tiff_tilereader_read,
			    _openslide_generic_tiff_tilereader_destroy);
  }

  return true;
}
