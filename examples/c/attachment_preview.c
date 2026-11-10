/* SIAARA example (C): show a DICOM file that arrived as untrusted bytes - a caption and a small
 * PNG / JPEG of the first frame, or a plain reason why there is no picture.
 *     attachment_preview [file.dcm]
 * Without an argument a sample is written and previewed, followed by three attachments that have
 * no picture: bytes that are not DICOM, a DICOM object without an image, and a cut-off file.
 * The work is in attachment_preview.h (preview_attachment); read that first. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <siaara/siaara.h>

#include "attachment_preview.h"
#include "make_sample.h"

static const char* outcome_name(int outcome) {
    switch (outcome) {
        case PREVIEW_OK: return "preview";
        case PREVIEW_NOT_DICOM: return "not a DICOM file";
        case PREVIEW_NO_PIXEL_DATA: return "no image";
        case PREVIEW_UNSUPPORTED: return "cannot preview";
        case PREVIEW_TOO_LARGE: return "too large to preview";
        default: return "damaged";
    }
}

/* The whole file in memory (the attachment as the mail server holds it). */
static uint8_t* read_all(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    uint8_t* data = NULL;
    long n = 0;
    *size = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = (uint8_t*)malloc(n > 0 ? (size_t)n : 1);
        if (data && fread(data, 1, (size_t)n, f) == (size_t)n) *size = (size_t)n;
    }
    fclose(f);
    return data;
}

static int write_buffer(const char* path, const siaara_buffer* b) {
    const uint8_t* data = NULL;
    uint64_t size = 0;
    FILE* f;
    if (siaara_buffer_data(b, &data, &size) != SIAARA_OK) return 1;
    f = fopen(path, "wb");
    if (!f) return 1;
    fwrite(data, 1, (size_t)size, f);
    fclose(f);
    printf("  %s: %llu bytes\n", path, (unsigned long long)size);
    return 0;
}

/* Previews one attachment, prints what a mail client would show and writes the pictures.
 * Returns the outcome (PREVIEW_*), or -1 when a picture could not be encoded or written. */
static int show(const char* label, const uint8_t* data, size_t size, const char* out_prefix) {
    preview_limits limits;
    preview_result r;
    char path[512];
    siaara_buffer* png = NULL;
    siaara_buffer* jpeg = NULL;
    int outcome, bad = 0;

    preview_default_limits(&limits);
    outcome = preview_attachment(data, size, &limits, &r);
    printf("%s (%lu bytes): %s", label, (unsigned long)size, outcome_name(outcome));
    if (outcome != PREVIEW_OK) printf(" - %s", r.reason);
    printf("\n");
    if (r.transfer_syntax[0]) {
        printf("  caption: %s%s%s%s%s", r.modality[0] ? r.modality : "?", r.study_date[0] ? ", " : "", r.study_date,
               r.series_description[0] ? ", " : "", r.series_description);
        if (r.rows) printf(", %u x %u, %u frame(s)", (unsigned)r.columns, (unsigned)r.rows, (unsigned)r.frames);
        printf(", %s\n", r.transfer_syntax_name[0] ? r.transfer_syntax_name : r.transfer_syntax);
    }
    if (r.incomplete)
        printf("  note: the file is incomplete (read loss 0x%X)%s\n", (unsigned)r.read_loss,
               outcome == PREVIEW_OK ? "; the picture shows what could be read" : "");
    if (outcome == PREVIEW_OK) {
        printf("  picture: %u x %u pixels\n", (unsigned)r.image.width, (unsigned)r.image.height);
        /* [encode] PNG (lossless) and JPEG of the RGBA8 picture. */
        if (preview_encode(&r.image, 0, &png) != SIAARA_OK || preview_encode(&r.image, 1, &jpeg) != SIAARA_OK) bad = 1;
        /* [encode] */
        if (!bad) {
            snprintf(path, sizeof path, "%s.png", out_prefix);
            bad |= write_buffer(path, png);
            snprintf(path, sizeof path, "%s.jpg", out_prefix);
            bad |= write_buffer(path, jpeg);
        }
        siaara_buffer_free(png);
        siaara_buffer_free(jpeg);
        if (!bad && r.strip.rgba) {
            siaara_buffer* strip = NULL;
            printf("  strip: %u frame(s), %u x %u pixels\n", (unsigned)r.strip_frames, (unsigned)r.strip.width, (unsigned)r.strip.height);
            if (preview_encode(&r.strip, 0, &strip) == SIAARA_OK) {
                snprintf(path, sizeof path, "%s-strip.png", out_prefix);
                bad |= write_buffer(path, strip);
            } else {
                bad = 1;
            }
            siaara_buffer_free(strip);
        }
        if (bad) fprintf(stderr, "encode: %s\n", siaara_last_error_message());
    }
    preview_free(&r);
    return bad ? -1 : outcome;
}

/* A DICOM object without an image (a bare Structured Report header). Returns 0 on success. */
static int make_report(const char* path) {
    siaara_file* file = NULL;
    siaara_dataset* ds = NULL;
    int bad = 0;
    if (siaara_file_create(&file) != SIAARA_OK || siaara_file_dataset(file, &ds) != SIAARA_OK) return 1;
    bad |= example_set(ds, 0x00080016u, "UI", "1.2.840.10008.5.1.4.1.1.88.11"); /* Basic Text SR */
    bad |= example_set(ds, 0x00080018u, "UI", "2.25.329800735698586629295641978511506172920");
    bad |= example_set(ds, 0x0020000Du, "UI", "2.25.329800735698586629295641978511506172916");
    bad |= example_set(ds, 0x0020000Eu, "UI", "2.25.329800735698586629295641978511506172921");
    bad |= example_set(ds, 0x00080060u, "CS", "SR");
    bad |= example_set(ds, 0x00080020u, "DA", "20260115");
    bad |= siaara_file_save(file, path, strlen(path), "1.2.840.10008.1.2.1") != SIAARA_OK;
    siaara_file_free(file);
    return bad;
}

int main(int argc, char** argv) {
    static const char text[] = "Dear colleague, please find the images attached. Regards";
    uint8_t* data = NULL;
    uint8_t padded[256];
    size_t size = 0;
    int outcome, bad = 0;

    printf("SIAARA %s\n", siaara_version_string());
    if (argc > 1) {
        data = read_all(argv[1], &size);
        if (!data) {
            fprintf(stderr, "cannot read %s\n", argv[1]);
            return 1;
        }
        outcome = show(argv[1], data, size, "preview");
        free(data);
        return outcome < 0 ? 1 : 0;
    }

    /* 1. An image: caption and picture. */
    if (example_make_sample("siaara_attachment.dcm") != 0) return 1;
    data = read_all("siaara_attachment.dcm", &size);
    if (!data) return 1;
    bad |= show("image", data, size, "siaara_attachment_preview") != PREVIEW_OK;
    {
        /* Patient identifiers are a separate, deliberate call. */
        preview_limits limits;
        char name[260], id[260];
        preview_default_limits(&limits);
        if (preview_patient_fields(data, size, &limits, name, sizeof name, id, sizeof id) == 0)
            printf("  (on request) patient: %s, %s\n", name, id);
        else
            bad = 1;
    }
    /* 2. The same file cut off in the middle of its pixel data: flagged, not shown as whole. */
    outcome = show("cut-off image", data, size - 3, "siaara_attachment_cut");
    bad |= outcome < 0 || outcome == PREVIEW_NOT_DICOM;
    free(data);
    /* 3. Bytes that are not DICOM, whatever the attachment's name says. */
    memset(padded, ' ', sizeof padded);
    memcpy(padded, text, sizeof text - 1);
    bad |= show("text named .dcm", padded, sizeof padded, "unused") != PREVIEW_NOT_DICOM;
    /* 4. DICOM without an image. */
    if (make_report("siaara_attachment_sr.dcm") != 0) return 1;
    data = read_all("siaara_attachment_sr.dcm", &size);
    if (!data) return 1;
    bad |= show("structured report", data, size, "unused") != PREVIEW_NO_PIXEL_DATA;
    free(data);
    if (bad) fprintf(stderr, "attachment_preview: an outcome was not the expected one\n");
    return bad;
}
