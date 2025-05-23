#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <sys/stat.h>
#include <stdio.h>
#include <sys/stat.h>
#else
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#endif
#include <fcntl.h>

#include "ayemu.h"

#define AYEMU_VTX_STRING_MAX 254

typedef const char * data_ptr_t;


/* LHA5 decoder, defined in lh5dec.c */
extern void lh5_decode(unsigned const char *inp,
		       unsigned char *outp,
		       unsigned long original_size,
		       unsigned long packed_size);

/* Read 8-bit integer from file.
 */
static data_ptr_t read_byte(data_ptr_t pos, int *p)
{
  const unsigned char *data = (const unsigned char*)pos;
  *p = *data++;
  return (data_ptr_t)data;
}

/* Read 16-bit little-endian 1234 integer from file.
 */
static data_ptr_t read_word16(data_ptr_t pos, int *p)
{
  const unsigned char *data = (const unsigned char*)pos;
  *p = *data++;
  *p += *data++ << 8;
  return (data_ptr_t)data;
}

/* read 32-bit integer from file.
 */
static data_ptr_t read_word32(data_ptr_t pos, int *p)
{
  const unsigned char *data = (const unsigned char*)pos;
  *p = *data++;
  *p += *data++ << 8;
  *p += *data++ << 16;
  *p += *data++ << 24;
  return (data_ptr_t)data;
}

 
// --- Safe string copy for read_string ---
static data_ptr_t read_string(data_ptr_t pos, char **res) {
    int len;

    if (pos == NULL) return NULL;

    len = strlen(pos);

    if (len > AYEMU_VTX_STRING_MAX) {
        fprintf(stderr, "Error: string len more than %d (=%d)\n", AYEMU_VTX_STRING_MAX, len);
        return NULL;
    }

    *res = calloc(1, len + 1);

#if defined(_WIN32)
    strcpy_s(*res, len + 1, pos);
#else
    strcpy(*res, pos);
#endif

    return pos + len + 1;
}
static data_ptr_t read_header(data_ptr_t buf, ayemu_vtx_t **target, size_t size)
{
  char hdr[3];
  ayemu_vtx_t *vtx;

  hdr[0] = tolower(*buf++);
  hdr[1] = tolower(*buf++);
  hdr[2] = '\0';

  if (size < 20) {
    fprintf(stderr, "ayemu_vtx_open: file size < 20 bytes - it is impossible\n");
    return NULL;
  }

  vtx = (ayemu_vtx_t*) calloc(1, sizeof(ayemu_vtx_t));

  if (strncmp(hdr, "ay", 2) == 0)
    vtx->chiptype = AYEMU_AY;
  else if (strncmp (hdr, "ym", 2) == 0)
    vtx->chiptype = AYEMU_YM;
  else {
    fprintf (stderr, "File is _not_ VORTEX format!\n"
	     "It not begins with 'ay' or 'ym' string.\n");
    goto clean_and_ret_null;
  }

  buf = read_byte(buf,   &vtx->stereo);
  buf = read_word16(buf, &vtx->loop);
  buf = read_word32(buf, &vtx->chipFreq);
  buf = read_byte(buf,   &vtx->playerFreq);
  buf = read_word16(buf, &vtx->year);
  buf = read_word32(buf, &vtx->regdata_size);

  buf = read_string(buf, &vtx->title);
  buf = read_string(buf, &vtx->author);
  buf = read_string(buf, &vtx->from);
  buf = read_string(buf, &vtx->tracker);
  buf = read_string(buf, &vtx->comment);

  *target = vtx;
  return buf;

 clean_and_ret_null:

  ayemu_vtx_free(vtx);
  return NULL;
}


ayemu_vtx_t * ayemu_vtx_header(data_ptr_t buf, size_t size)
{
  ayemu_vtx_t *vtx;

  const char *data;

  data = read_header(buf, &vtx, size);

  return vtx;
}


ayemu_vtx_t * ayemu_vtx_load(data_ptr_t buf, size_t size)
{
  ayemu_vtx_t *vtx;

  const char *data = read_header(buf, &vtx, size);

  if (! data) {
    fprintf(stderr, "ayemu_vtx_load: Cannot parse file header\n");
    return NULL;
  }

  // unpack data
  size -= (buf - data);

  if ((vtx->regdata = (unsigned char *) malloc (vtx->regdata_size)) == NULL) {
    fprintf (stderr, "ayemu_vtx_load_data: Can allocate %d bytes"
	     " for unpack register data\n", vtx->regdata_size);
    return NULL;
  }

  lh5_decode ((unsigned char*)data, vtx->regdata, vtx->regdata_size, size);

  vtx->frames = vtx->regdata_size / 14;

  return vtx;
}


/** Get specified data frame by number.
 *
 */
void ayemu_vtx_getframe(const ayemu_vtx_t *vtx, size_t frame_n, ayemu_ay_reg_frame_t regs)
{
  int n;

  if (frame_n >= vtx->frames)
    return;

  // calculate begin of data
  unsigned char *p = vtx->regdata + frame_n;

  // step is data size / 14
  for(n = 0 ; n < 14 ; n++) {
      regs[n] = *p;
      p += vtx->frames;
  }
}

/** Free all allocaded resources.
 *
 */
void ayemu_vtx_free(ayemu_vtx_t *vtx)
{
#define FREE_PTR(x)  if (x) { free(x); x = NULL; }

  FREE_PTR(vtx->title);
  FREE_PTR(vtx->author);
  FREE_PTR(vtx->from);
  FREE_PTR(vtx->tracker);
  FREE_PTR(vtx->comment);
  FREE_PTR(vtx->regdata);
}



ayemu_vtx_t *ayemu_vtx_header_from_file(const char *filename) {
    ayemu_vtx_t *ret = NULL;
    size_t size;
    FILE *f = NULL;
#if defined(_WIN32)
    char errbuf[128];
    if (fopen_s(&f, filename, "rb") != 0 || !f) {
        strerror_s(errbuf, sizeof(errbuf), errno);
        fprintf(stderr, "Can't open file %s: %s\n", filename, errbuf);
        return NULL;
    }
#else
    f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Can't open file %s: %s\n", filename, strerror(errno));
        return NULL;
    }
#endif
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *data = (char *)malloc(size);
    if (!data) {
        fprintf(stderr, "Can't allocate memory for file %s\n", filename);
        fclose(f);
        return NULL;
    }
#if defined(_WIN32)
    if (fread_s(data, size, 1, size, f) != size) {
#else
    if (fread(data, 1, size, f) != size) {
#endif
        fprintf(stderr, "Can't read file %s\n", filename);
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    ret = ayemu_vtx_header(data, size);

    free(data);
    return ret;
}

ayemu_vtx_t *ayemu_vtx_load_from_file(const char *filename) {
    ayemu_vtx_t *ret = NULL;
    size_t size;
    FILE *f = NULL;
#if defined(_WIN32)
    char errbuf[128];
    if (fopen_s(&f, filename, "rb") != 0 || !f) {
        strerror_s(errbuf, sizeof(errbuf), errno);
        fprintf(stderr, "Can't open file %s: %s\n", filename, errbuf);
        return NULL;
    }
#else
    f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Can't open file %s: %s\n", filename, strerror(errno));
        return NULL;
    }
#endif
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *data = (char *)malloc(size);
    if (!data) {
        fprintf(stderr, "Can't allocate memory for file %s\n", filename);
        fclose(f);
        return NULL;
    }
#if defined(_WIN32)
    if (fread_s(data, size, 1, size, f) != size) {
#else
    if (fread(data, 1, size, f) != size) {
#endif
        fprintf(stderr, "Can't read file %s\n", filename);
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    ret = ayemu_vtx_load(data, size);

    free(data);
    return ret;
}
