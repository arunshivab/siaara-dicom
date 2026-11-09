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
    puts("capi_smoke: all checks passed");
    return 0;
}
