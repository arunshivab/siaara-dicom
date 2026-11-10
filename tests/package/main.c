/* Links against an installed SIAARA package (find_package(siaara) or pkg-config): checks the ABI
 * and, when SIAARA_EXPECT_VERSION is defined, the version; then writes a small file in memory
 * terms (create, set, save), opens it again and reads a value back. */
#include <stdio.h>
#include <string.h>

#include <siaara/siaara.h>

int main(void) {
    const char* path = "siaara_consumer_test.dcm";
    siaara_file* file = NULL;
    siaara_dataset* ds = NULL;
    char name[64];
    size_t needed = 0;

    if (siaara_abi_version() != SIAARA_ABI_VERSION) {
        fprintf(stderr, "ABI version mismatch\n");
        return 1;
    }
#ifdef SIAARA_EXPECT_VERSION
    if (strcmp(siaara_version_string(), SIAARA_EXPECT_VERSION) != 0) {
        fprintf(stderr, "library version %s, expected %s\n", siaara_version_string(), SIAARA_EXPECT_VERSION);
        return 1;
    }
#endif
    if (siaara_file_create(&file) != SIAARA_OK || siaara_file_dataset(file, &ds) != SIAARA_OK ||
        siaara_dataset_set_string(ds, 0x00080016u, "UI", "1.2.840.10008.5.1.4.1.1.7", 25) != SIAARA_OK ||
        siaara_dataset_set_string(ds, 0x00080018u, "UI", "2.25.42", 7) != SIAARA_OK ||
        siaara_dataset_set_string(ds, 0x00100010u, "PN", "Consumer^Test", 13) != SIAARA_OK ||
        siaara_file_save(file, path, strlen(path), "1.2.840.10008.1.2.1") != SIAARA_OK) {
        fprintf(stderr, "write: %s\n", siaara_last_error_message());
        return 1;
    }
    siaara_file_free(file);
    file = NULL;
    if (siaara_file_open(path, strlen(path), NULL, &file) != SIAARA_OK || siaara_file_dataset(file, &ds) != SIAARA_OK ||
        siaara_dataset_get_string(ds, 0x00100010u, 0, name, sizeof name, &needed) != SIAARA_OK ||
        strcmp(name, "Consumer^Test") != 0) {
        fprintf(stderr, "read back: %s\n", siaara_last_error_message());
        return 1;
    }
    siaara_file_free(file);
    remove(path);
    printf("siaara %s\n", siaara_version_string());
    return 0;
}
