/* SIAARA example (C): open a file, read Patient's Name by keyword and by path, and read the pixel
 * value at a point.
 *     read_file [file.dcm]        without an argument a sample file is written and read
 */
#include <stdio.h>
#include <string.h>

#include <siaara/siaara.h>

#include "make_sample.h"

static int fail(const char* what) {
    fprintf(stderr, "%s: %s\n", what, siaara_last_error_message());
    return 1;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "siaara_example.dcm";
    siaara_file* file = NULL;
    siaara_dataset* ds = NULL;
    siaara_dict_entry entry;
    siaara_path_value value;
    siaara_pixel_value pixel;
    char text[256];
    size_t needed = 0;

    printf("SIAARA %s (ABI %u)\n", siaara_version_string(), (unsigned)siaara_abi_version());
    if (argc < 2 && example_make_sample(path) != 0) return 1;

    /* [open] */
    if (siaara_file_open(path, strlen(path), NULL, &file) != SIAARA_OK) return fail("open");
    if (siaara_file_dataset(file, &ds) != SIAARA_OK) return fail("dataset");
    /* [open] */

    /* [keyword] Patient's Name by keyword: the dictionary gives the tag, the value comes as UTF-8. */
    memset(&entry, 0, sizeof entry);
    entry.struct_size = sizeof entry;
    if (siaara_dict_lookup_keyword("PatientName", strlen("PatientName"), &entry) != SIAARA_OK) return fail("keyword");
    if (siaara_dataset_get_utf8(ds, entry.tag, 0, text, sizeof text, &needed) != SIAARA_OK) return fail("PatientName");
    printf("PatientName (%08X) = %s\n", (unsigned)entry.tag, text);
    /* [keyword] */

    /* [path] The same by path; paths also reach into sequences: "BeamSequence[0].BeamName". */
    memset(&value, 0, sizeof value);
    value.struct_size = sizeof value;
    if (siaara_get_by_path(ds, "PatientName", strlen("PatientName"), &value, text, sizeof text, &needed) != SIAARA_OK)
        return fail("path");
    printf("by path: VR %s, VM %u, text %s\n", value.vr, (unsigned)value.vm, text);
    /* [path] */

    /* [pixel] Stored value and modality value (rescale applied) at column 1, row 0 of frame 0. */
    memset(&pixel, 0, sizeof pixel);
    pixel.struct_size = sizeof pixel;
    if (siaara_file_pixel_value(file, 0, 1, 0, 0, &pixel) != SIAARA_OK) return fail("pixel value");
    printf("pixel (1,0): stored %lld", (long long)pixel.stored);
    if (pixel.has_modality) printf(", modality value %g", pixel.modality_value);
    printf("\n");
    /* [pixel] */

    siaara_file_free(file);
    return 0;
}
