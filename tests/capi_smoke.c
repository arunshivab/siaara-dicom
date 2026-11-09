/* Compiled as C11: proves the public header is valid C and the shared library links. */
#include <stdio.h>
#include <string.h>

#include "siaara/siaara.h"

#define EXPECT(c) do { if (!(c)) { fprintf(stderr, "FAILED: %s (line %d): %s\n", #c, __LINE__, siaara_last_error_message()); return 1; } } while (0)

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "capi_smoke.dcm";
    siaara_file* f = NULL;
    siaara_dataset* ds = NULL;
    char buf[64];
    size_t needed = 0;
    int64_t rows = 0;
    siaara_dict_entry e;

    EXPECT(siaara_abi_version() == SIAARA_ABI_VERSION);
    EXPECT(siaara_file_create(&f) == SIAARA_OK);
    EXPECT(siaara_file_dataset(f, &ds) == SIAARA_OK);
    EXPECT(siaara_dataset_set_string(ds, 0x00080016u, "UI", "1.2.840.10008.5.1.4.1.1.7", 25) == SIAARA_OK);
    EXPECT(siaara_dataset_set_string(ds, 0x00080018u, "UI", "2.25.42", 7) == SIAARA_OK);
    EXPECT(siaara_dataset_set_string(ds, 0x00100010u, "PN", "Doe^Jane", 8) == SIAARA_OK);
    EXPECT(siaara_dataset_set_uint16(ds, 0x00280010u, 512) == SIAARA_OK);
    EXPECT(siaara_file_save(f, path, strlen(path), "1.2.840.10008.1.2") == SIAARA_OK);
    siaara_file_free(f);
    f = NULL;

    EXPECT(siaara_file_open(path, strlen(path), NULL, &f) == SIAARA_OK);
    EXPECT(siaara_file_dataset(f, &ds) == SIAARA_OK);
    EXPECT(siaara_dataset_get_string(ds, 0x00100010u, 0, buf, sizeof buf, &needed) == SIAARA_OK);
    EXPECT(strcmp(buf, "Doe^Jane") == 0 && needed == 8);
    EXPECT(siaara_dataset_get_string(ds, 0x00100010u, 0, buf, 4, &needed) == SIAARA_ERR_BUFFER_TOO_SMALL);
    EXPECT(siaara_dataset_get_int64(ds, 0x00280010u, 0, &rows) == SIAARA_OK && rows == 512);
    EXPECT(siaara_dataset_get_int64(ds, 0x00280011u, 0, &rows) == SIAARA_ERR_NOT_FOUND);
    EXPECT(siaara_file_transfer_syntax(f, buf, sizeof buf, &needed) == SIAARA_OK);
    EXPECT(strcmp(buf, "1.2.840.10008.1.2") == 0);
    siaara_file_free(f);
    siaara_file_free(NULL);

    memset(&e, 0, sizeof e);
    e.struct_size = sizeof e;
    EXPECT(siaara_dict_lookup(0x00100010u, &e) == SIAARA_OK);
    EXPECT(strcmp(e.keyword, "PatientName") == 0 && strcmp(e.vr, "PN") == 0);
    EXPECT(siaara_dict_lookup_keyword("Rows", 4, &e) == SIAARA_OK && e.tag == 0x00280010u);
    EXPECT(siaara_file_open(NULL, 0, NULL, &f) == SIAARA_ERR_INVALID_ARGUMENT);

    /* pixel data: 2x2 signed 16-bit CT-like image, rescale intercept -1024, window 0/100 */
    {
        const uint8_t px[8] = {0x00, 0x04, 0x32, 0x04, 0xCE, 0x03, 0xFF, 0x7F}; /* 1024, 1074, 974, 32767 */
        siaara_pixel_info pi;
        siaara_render_options ro;
        uint8_t native[8];
        float fl[4];
        uint8_t rgba[16];
        uint32_t frames = 0;
        EXPECT(siaara_file_create(&f) == SIAARA_OK);
        EXPECT(siaara_file_dataset(f, &ds) == SIAARA_OK);
        EXPECT(siaara_dataset_set_string(ds, 0x00080016u, "UI", "1.2.840.10008.5.1.4.1.1.7", 25) == SIAARA_OK);
        EXPECT(siaara_dataset_set_string(ds, 0x00080018u, "UI", "2.25.43", 7) == SIAARA_OK);
        EXPECT(siaara_dataset_set_uint16(ds, 0x00280002u, 1) == SIAARA_OK);
        EXPECT(siaara_dataset_set_string(ds, 0x00280004u, "CS", "MONOCHROME2", 11) == SIAARA_OK);
        EXPECT(siaara_dataset_set_uint16(ds, 0x00280010u, 2) == SIAARA_OK);
        EXPECT(siaara_dataset_set_uint16(ds, 0x00280011u, 2) == SIAARA_OK);
        EXPECT(siaara_dataset_set_uint16(ds, 0x00280100u, 16) == SIAARA_OK);
        EXPECT(siaara_dataset_set_uint16(ds, 0x00280101u, 16) == SIAARA_OK);
        EXPECT(siaara_dataset_set_uint16(ds, 0x00280102u, 15) == SIAARA_OK);
        EXPECT(siaara_dataset_set_uint16(ds, 0x00280103u, 1) == SIAARA_OK);
        EXPECT(siaara_dataset_set_string(ds, 0x00281050u, "DS", "0", 1) == SIAARA_OK);
        EXPECT(siaara_dataset_set_string(ds, 0x00281051u, "DS", "100", 3) == SIAARA_OK);
        EXPECT(siaara_dataset_set_string(ds, 0x00281052u, "DS", "-1024", 5) == SIAARA_OK);
        EXPECT(siaara_dataset_set_string(ds, 0x00281053u, "DS", "1", 1) == SIAARA_OK);
        EXPECT(siaara_dataset_set_bytes(ds, 0x7FE00010u, "OW", px, sizeof px) == SIAARA_OK);

        memset(&pi, 0, sizeof pi);
        EXPECT(siaara_file_pixel_info(f, &pi) == SIAARA_ERR_INVALID_ARGUMENT); /* struct_size not set */
        pi.struct_size = sizeof pi;
        EXPECT(siaara_file_pixel_info(f, &pi) == SIAARA_OK);
        EXPECT(pi.rows == 2 && pi.columns == 2 && pi.bits_stored == 16 && pi.pixel_representation == 1);
        EXPECT(pi.frame_bytes == 8 && strcmp(pi.photometric, "MONOCHROME2") == 0 && pi.encapsulated == 0);
        EXPECT(siaara_frame_count(f, &frames) == SIAARA_OK && frames == 1);

        EXPECT(siaara_frame_decode(f, 0, SIAARA_FRAME_NATIVE, NULL, 0, &needed) == SIAARA_OK && needed == 8);
        EXPECT(siaara_frame_decode(f, 0, SIAARA_FRAME_NATIVE, native, 4, &needed) == SIAARA_ERR_BUFFER_TOO_SMALL);
        EXPECT(siaara_frame_decode(f, 0, SIAARA_FRAME_NATIVE, native, sizeof native, &needed) == SIAARA_OK);
        EXPECT(memcmp(native, px, 8) == 0);
        EXPECT(siaara_frame_decode(f, 0, SIAARA_FRAME_FLOAT32_MODALITY, (uint8_t*)fl, sizeof fl, &needed) == SIAARA_OK);
        EXPECT(needed == 16 && fl[0] == 0.0f && fl[1] == 50.0f && fl[2] == -50.0f);
        EXPECT(siaara_frame_decode(f, 1, SIAARA_FRAME_NATIVE, native, sizeof native, &needed) == SIAARA_ERR_INVALID_ARGUMENT);
        EXPECT(siaara_frame_decode(f, 0, 7, native, sizeof native, &needed) == SIAARA_ERR_INVALID_ARGUMENT);

        EXPECT(siaara_render_rgba8(f, NULL, rgba, sizeof rgba, &needed) == SIAARA_OK && needed == 16);
        EXPECT(rgba[4] == 255 && rgba[8] == 0 && rgba[3] == 255); /* +50 HU white, -50 HU black */
        memset(&ro, 0, sizeof ro);
        ro.struct_size = sizeof ro;
        ro.voi_mode = SIAARA_VOI_EXPLICIT;
        ro.window_center = 0;
        ro.window_width = 100;
        ro.invert = 1;
        EXPECT(siaara_render_rgba8(f, &ro, rgba, sizeof rgba, &needed) == SIAARA_OK);
        EXPECT(rgba[4] == 0 && rgba[8] == 255);
        ro.window_width = 0;
        EXPECT(siaara_render_rgba8(f, &ro, rgba, sizeof rgba, &needed) == SIAARA_ERR_INVALID_ARGUMENT);
        EXPECT(siaara_render_rgba8(f, NULL, rgba, 8, &needed) == SIAARA_ERR_BUFFER_TOO_SMALL);
        siaara_file_free(f);
        f = NULL;
    }
    puts("capi_smoke: all checks passed");
    return 0;
}
