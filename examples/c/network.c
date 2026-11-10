/* SIAARA example (C): start an SCP over a local store, send C-ECHO and C-STORE to it, then start
 * the DICOMweb server over the same store and query it with QIDO-RS. Everything runs on the
 * loopback interface with ephemeral ports.
 *     network <empty work folder>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <siaara/siaara.h>

#include "make_sample.h"

static int fail(const char* what) {
    fprintf(stderr, "%s: %s\n", what, siaara_last_error_message());
    return 1;
}

int main(int argc, char** argv) {
    char store_root[1024], sample[1024], url[128], json[8192];
    siaara_store* store = NULL;
    siaara_scp* scp = NULL;
    siaara_web_server* web = NULL;
    siaara_web_client* client = NULL;
    uint32_t dimse_port = 0, web_port = 0;
    size_t needed = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: network <work folder>\n");
        return 2;
    }
    snprintf(store_root, sizeof store_root, "%s/store", argv[1]);
    snprintf(sample, sizeof sample, "%s/sample.dcm", argv[1]);
    if (example_make_sample(sample) != 0) return 1;

    /* [scp] A Storage SCP (Verification is always on) that files what it receives in a store. */
    if (siaara_store_open(store_root, strlen(store_root), NULL, &store) != SIAARA_OK) return fail("store");
    {
        const char* peers[] = {"EXAMPLE_SCU"};  /* the calling AE titles that may connect */
        siaara_scp_options options;
        memset(&options, 0, sizeof options);
        options.struct_size = sizeof options;
        options.port = 0;                       /* 0: an ephemeral port; a real archive names its port */
        options.bind_address = "127.0.0.1";
        options.ae_title = "EXAMPLE_SCP";
        options.allowed_calling_aes = peers;
        options.allowed_count = 1;
        if (siaara_scp_start(&options, store, NULL, NULL, &scp) != SIAARA_OK) return fail("scp start");
        if (siaara_scp_port(scp, &dimse_port) != SIAARA_OK) return fail("scp port");
    }
    printf("SCP EXAMPLE_SCP listening on 127.0.0.1:%u\n", (unsigned)dimse_port);
    /* [scp] */

    /* [echo-store] C-ECHO, then C-STORE of a file. */
    {
        siaara_dimse_peer peer;
        siaara_dimse_result result;
        const char* files[] = {sample};
        memset(&peer, 0, sizeof peer);
        peer.struct_size = sizeof peer;
        peer.host = "127.0.0.1";
        peer.port = dimse_port;
        peer.calling_ae = "EXAMPLE_SCU";
        peer.called_ae = "EXAMPLE_SCP";
        memset(&result, 0, sizeof result);
        result.struct_size = sizeof result;
        if (siaara_dimse_echo(&peer, NULL, &result) != SIAARA_OK) return fail("C-ECHO");
        printf("C-ECHO status 0x%04X\n", (unsigned)result.status);
        if (siaara_dimse_store_files(&peer, files, NULL, 1, NULL, NULL, NULL, &result) != SIAARA_OK) return fail("C-STORE");
        printf("C-STORE status 0x%04X, %u stored, %u failed\n", (unsigned)result.status, (unsigned)result.completed,
               (unsigned)result.failed);
    }
    /* [echo-store] */

    /* [dicomweb] DICOMweb (QIDO-RS, WADO-RS, STOW-RS) over the same store, then a QIDO-RS query. */
    {
        siaara_web_server_options options;
        memset(&options, 0, sizeof options);
        options.struct_size = sizeof options;
        options.port = 0;   /* ephemeral; plain HTTP is allowed on the loopback interface only */
        if (siaara_web_server_start(&options, store, &web) != SIAARA_OK) return fail("web server");
        if (siaara_web_server_port(web, &web_port) != SIAARA_OK) return fail("web port");
    }
    snprintf(url, sizeof url, "http://127.0.0.1:%u/dicomweb", (unsigned)web_port);
    if (siaara_web_client_create(url, strlen(url), 0, &client) != SIAARA_OK) return fail("web client");
    {
        const char* query = "PatientID=EX-0001";
        if (siaara_web_qido(client, SIAARA_QIDO_STUDIES, NULL, NULL, query, strlen(query), json, sizeof json, &needed) != SIAARA_OK)
            return fail("QIDO-RS");
    }
    printf("QIDO-RS %s/studies?PatientID=EX-0001 ->\n%s\n", url, json);
    /* [dicomweb] */

    if (strstr(json, "EX-0001") == NULL) {
        fprintf(stderr, "the stored study was not found by QIDO-RS\n");
        return 1;
    }
    siaara_web_client_free(client);
    siaara_web_server_free(web);
    siaara_scp_stop(scp);
    siaara_scp_free(scp);
    siaara_store_free(store);
    return 0;
}
