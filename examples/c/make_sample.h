/* SIAARA examples - writes a small sample file (2 x 2 signed 16-bit CT-like image) so that the
 * examples need no data: path receives the file. Returns 0 on success. */
#ifndef SIAARA_EXAMPLE_MAKE_SAMPLE_H
#define SIAARA_EXAMPLE_MAKE_SAMPLE_H

#include <stdio.h>
#include <string.h>

#include <siaara/siaara.h>

static int example_set(siaara_dataset* ds, uint32_t tag, const char* vr, const char* value) {
    return siaara_dataset_set_string(ds, tag, vr, value, strlen(value)) == SIAARA_OK ? 0 : 1;
}

static int example_make_sample(const char* path) {
    /* stored values 1024, 1074, 974, 2024 (little endian); Rescale Intercept -1024 -> 0, 50, -50, 1000 HU */
    static const uint8_t pixels[8] = {0x00, 0x04, 0x32, 0x04, 0xCE, 0x03, 0xE8, 0x07};
    siaara_file* file = NULL;
    siaara_dataset* ds = NULL;
    int bad = 0;
    if (siaara_file_create(&file) != SIAARA_OK || siaara_file_dataset(file, &ds) != SIAARA_OK) return 1;
    bad |= example_set(ds, 0x00080016u, "UI", "1.2.840.10008.5.1.4.1.1.7"); /* SOP Class: Secondary Capture */
    bad |= example_set(ds, 0x00080018u, "UI", "2.25.329800735698586629295641978511506172918");
    bad |= example_set(ds, 0x0020000Du, "UI", "2.25.329800735698586629295641978511506172916");
    bad |= example_set(ds, 0x0020000Eu, "UI", "2.25.329800735698586629295641978511506172917");
    bad |= example_set(ds, 0x00080060u, "CS", "OT");
    bad |= example_set(ds, 0x00100010u, "PN", "Example^Patient");
    bad |= example_set(ds, 0x00100020u, "LO", "EX-0001");
    bad |= example_set(ds, 0x00280004u, "CS", "MONOCHROME2");
    bad |= example_set(ds, 0x00281052u, "DS", "-1024"); /* Rescale Intercept */
    bad |= example_set(ds, 0x00281053u, "DS", "1");     /* Rescale Slope */
    bad |= siaara_dataset_set_uint16(ds, 0x00280002u, 1) != SIAARA_OK;  /* Samples per Pixel */
    bad |= siaara_dataset_set_uint16(ds, 0x00280010u, 2) != SIAARA_OK;  /* Rows */
    bad |= siaara_dataset_set_uint16(ds, 0x00280011u, 2) != SIAARA_OK;  /* Columns */
    bad |= siaara_dataset_set_uint16(ds, 0x00280100u, 16) != SIAARA_OK; /* Bits Allocated */
    bad |= siaara_dataset_set_uint16(ds, 0x00280101u, 16) != SIAARA_OK; /* Bits Stored */
    bad |= siaara_dataset_set_uint16(ds, 0x00280102u, 15) != SIAARA_OK; /* High Bit */
    bad |= siaara_dataset_set_uint16(ds, 0x00280103u, 1) != SIAARA_OK;  /* Pixel Representation: signed */
    bad |= siaara_dataset_set_bytes(ds, 0x7FE00010u, "OW", pixels, sizeof pixels) != SIAARA_OK;
    /* Explicit VR Little Endian */
    bad |= siaara_file_save(file, path, strlen(path), "1.2.840.10008.1.2.1") != SIAARA_OK;
    if (bad) fprintf(stderr, "sample file: %s\n", siaara_last_error_message());
    siaara_file_free(file);
    return bad;
}

#endif
