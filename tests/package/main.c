/* Links against an installed SIAARA package through find_package(siaara). */
#include <stdio.h>

#include <siaara/siaara.h>

int main(void) {
    if (siaara_abi_version() != SIAARA_ABI_VERSION) {
        fprintf(stderr, "ABI version mismatch\n");
        return 1;
    }
    printf("siaara %s\n", siaara_version_string());
    return 0;
}
