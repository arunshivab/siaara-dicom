/* SIAARA example - a safe preview of a DICOM file that arrived as UNTRUSTED bytes (for example a
 * mail attachment): a caption from a few header fields and a small picture of the first frame,
 * or a plain reason why there is no picture. docs/GETTING_STARTED.md, "Showing a DICOM attachment
 * safely (untrusted input)".
 *
 * What makes it safe is in preview_attachment():
 *   - the bytes are parsed from memory with every limit set explicitly (preview_limits): nesting
 *     depth, number of elements and items, what a deflated transfer syntax may inflate to, the
 *     largest decoded frame, the memory the frame functions may use (decode AND render), the
 *     JPEG scans per frame, no cropping of oversized codestreams;
 *   - the reader's recovery heuristic for damaged lengths is off;
 *   - an image with more pixels than max_pixels is refused before anything is decoded (a render
 *     works at 4 bytes per pixel whatever the bit depth);
 *   - only a fixed number of frames is ever decoded, each on its own, within a pixel budget for
 *     the whole preview (max_total_pixels);
 *   - the picture is rendered at a bounded size (max_box), whatever Rows and Columns claim;
 *   - a caption value longer than max_value_bytes is not decoded at all; control and format
 *     characters (bidirectional overrides, zero-width characters) are taken out of captions;
 *   - no patient identifier is read unless the caller asks (preview_patient_fields()).
 *
 * The limits bound MEMORY and the amount of work. They do not bound TIME: a decode has no
 * deadline and cannot be cancelled (a progressive JPEG with hundreds of scans is slow by design),
 * and a thread cannot be abandoned. Run previews of untrusted input in a SEPARATE PROCESS with a
 * kill timer and a memory limit (a job object, a cgroup, ulimit), and cap the attachment size
 * before you get here.
 *
 * Plain C99, usable from C++ too (the toolkit's own tests include this file). */
#ifndef SIAARA_EXAMPLE_ATTACHMENT_PREVIEW_H
#define SIAARA_EXAMPLE_ATTACHMENT_PREVIEW_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <siaara/siaara.h>

typedef struct preview_limits {
    size_t max_attachment_bytes;  /* larger attachments are not looked at */
    uint32_t max_box;             /* the picture fits max_box x max_box pixels */
    uint32_t max_strip_frames;    /* multi-frame: up to this many frames in a strip; 0 = no strip */
    uint32_t strip_box;           /* each strip tile fits strip_box x strip_box pixels */
    uint32_t max_depth;           /* sequence nesting */
    uint64_t max_elements;        /* data elements, and sequence items, of the whole file */
    uint64_t max_inflated_size;   /* what a deflated transfer syntax may inflate to */
    uint64_t max_frame_bytes;     /* the largest decoded frame that is previewed */
    uint64_t frame_memory_budget; /* memory for decoding and rendering one frame (pixels, codec working memory) */
    uint64_t max_pixels;          /* Rows x Columns of one frame: more is refused before decoding */
    uint64_t max_total_pixels;    /* pixels decoded for one preview, all frames together */
    uint32_t max_scans;           /* JPEG scans decoded per frame */
    uint32_t max_crop_factor;     /* 1 = a codestream must have the size the data set states */
    size_t max_value_bytes;       /* a longer caption value is not decoded at all */
    int strip_control_and_format; /* captions: 1 = controls become a space, format characters are removed */
} preview_limits;

/* The defaults of the limits added for the release-candidate review (R-191, R-192, R-193, R-199). */
#define PREVIEW_DEFAULT_MAX_PIXELS 64000000u
#define PREVIEW_DEFAULT_MAX_TOTAL_PIXELS 128000000u
#define PREVIEW_DEFAULT_MAX_SCANS 64u
#define PREVIEW_DEFAULT_MAX_CROP_FACTOR 1u
#define PREVIEW_DEFAULT_MAX_VALUE_BYTES 65536u

/* Recommended settings for a mail server. Each protects against something:
 *   max_attachment_bytes 64 MiB   everything below is proportional to the input at worst
 *   max_depth 16                  deeply nested sequences (stack and time)
 *   max_elements 200,000          millions of tiny elements or empty items (8 input bytes each
 *                                 become an object in memory)
 *   max_inflated_size 128 MiB     a deflate bomb (a few kB that inflate to gigabytes)
 *   max_frame_bytes 64 MiB        huge Rows x Columns (a 4096 x 4096 16-bit frame is 32 MiB)
 *   frame_memory_budget 256 MiB   a codestream that claims huge dimensions or many components;
 *                                 a render at image resolution (4 bytes per pixel) counts too
 *   max_pixels 64,000,000         a small file that states a huge 1-bit or 8-bit image
 *   max_total_pixels 128,000,000  many large frames in the strip
 *   max_scans 64                  a progressive JPEG with hundreds of scans (time)
 *   max_crop_factor 1             a codestream larger than the image, decoded only to be cropped
 *   max_value_bytes 65,536        a caption value of many megabytes
 *   max_box 512, strip 8 x 128    the size of what is produced, whatever the file claims */
static void preview_default_limits(preview_limits* l) {
    memset(l, 0, sizeof *l);
    l->max_attachment_bytes = (size_t)64 << 20;
    l->max_box = 512;
    l->max_strip_frames = 8;
    l->strip_box = 128;
    l->max_depth = 16;
    l->max_elements = 200000;
    l->max_inflated_size = (uint64_t)128 << 20;
    l->max_frame_bytes = (uint64_t)64 << 20;
    l->frame_memory_budget = (uint64_t)256 << 20;
    l->max_pixels = PREVIEW_DEFAULT_MAX_PIXELS;
    l->max_total_pixels = PREVIEW_DEFAULT_MAX_TOTAL_PIXELS;
    l->max_scans = PREVIEW_DEFAULT_MAX_SCANS;
    l->max_crop_factor = PREVIEW_DEFAULT_MAX_CROP_FACTOR;
    l->max_value_bytes = PREVIEW_DEFAULT_MAX_VALUE_BYTES;
    l->strip_control_and_format = 1;
}

enum {
    PREVIEW_OK = 0,            /* there is a picture */
    PREVIEW_NOT_DICOM = 1,     /* the bytes are not a DICOM Part 10 file */
    PREVIEW_NO_PIXEL_DATA = 2, /* a DICOM object without an image: SR, RT Structure Set, encapsulated PDF, DICOMDIR ... */
    PREVIEW_UNSUPPORTED = 3,   /* pixel data the toolkit cannot decode: video, JPEG XL ... (reason names it) */
    PREVIEW_TOO_LARGE = 4,     /* a limit was reached: no picture (reason names it) */
    PREVIEW_DAMAGED = 5        /* DICOM, but the file or its pixel data cannot be read */
};

typedef struct preview_image {
    uint32_t width, height; /* pixels */
    uint8_t* rgba;          /* width x height x 4 bytes, rows top to bottom; NULL when there is none */
} preview_image;

typedef struct preview_result {
    int outcome;              /* PREVIEW_* */
    char reason[256];         /* why there is no picture (PREVIEW_OK: "") */
    /* The read was not complete (truncated, bytes skipped, ...): what could be decoded is still
     * shown; say so next to the picture. read_loss has the SIAARA_READ_* bits. */
    int incomplete;
    uint32_t read_loss;
    /* Caption fields. No patient identifiers. Text from the file: escape it before it goes into
     * HTML. Empty when the file does not have the attribute. */
    char modality[20];
    char study_date[12];
    char series_description[260];
    char transfer_syntax[68];
    char transfer_syntax_name[100];
    uint32_t rows, columns, frames;
    preview_image image;      /* frame 0 */
    preview_image strip;      /* multi-frame: strip_frames frames side by side; rgba NULL when there is none */
    uint32_t strip_frames;
} preview_result;

static void preview_free(preview_result* r) {
    if (!r) return;
    free(r->image.rgba);
    free(r->strip.rgba);
    r->image.rgba = NULL;
    r->strip.rgba = NULL;
}

static int preview_stop(preview_result* r, int outcome, const char* reason) {
    r->outcome = outcome;
    snprintf(r->reason, sizeof r->reason, "%s", reason ? reason : "");
    return outcome;
}

/* Is code point c a Unicode format character (category Cf: soft hyphen, Arabic number signs,
 * zero-width characters, bidirectional marks, embeddings, overrides and isolates, the byte order
 * mark, interlinear annotation and tag characters ...)? The ranges of Unicode 16. */
static int preview_is_format(uint32_t c) {
    static const uint32_t ranges[][2] = {
        {0x00AD, 0x00AD},   {0x0600, 0x0605},   {0x061C, 0x061C},   {0x06DD, 0x06DD},   {0x070F, 0x070F},
        {0x0890, 0x0891},   {0x08E2, 0x08E2},   {0x180E, 0x180E},   {0x200B, 0x200F},   {0x202A, 0x202E},
        {0x2060, 0x2064},   {0x2066, 0x206F},   {0xFEFF, 0xFEFF},   {0xFFF9, 0xFFFB},   {0x110BD, 0x110BD},
        {0x110CD, 0x110CD}, {0x13430, 0x1343F}, {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A}, {0xE0001, 0xE0001},
        {0xE0020, 0xE007F}};
    size_t i;
    for (i = 0; i < sizeof ranges / sizeof ranges[0]; ++i)
        if (c >= ranges[i][0] && c <= ranges[i][1]) return 1;
    return 0;
}

/* Caption text made safe to show as plain text, in place: control characters (C0, DEL, C1) and
 * the line / paragraph separators become a space, format characters are removed, bytes that are
 * not UTF-8 become '?'. The text never grows. It is still text from the file: escape it before
 * it goes into HTML. */
static void preview_clean_text(char* text) {
    const unsigned char* in = (const unsigned char*)text;
    char* out = text;
    while (*in != 0) {
        uint32_t c = *in;
        size_t n = 1, k;
        if (c >= 0xF0) {
            n = c <= 0xF4 ? 4 : 0;
            c &= 0x07;
        } else if (c >= 0xE0) {
            n = 3;
            c &= 0x0F;
        } else if (c >= 0xC2) {
            n = 2;
            c &= 0x1F;
        } else if (c >= 0x80) {
            n = 0;
        }
        for (k = 1; k < n; ++k) {
            if ((in[k] & 0xC0) != 0x80) {
                n = 0; /* also stops at the terminating NUL */
                break;
            }
            c = (c << 6) | (in[k] & 0x3Fu);
        }
        if (n == 0 || (n == 3 && c < 0x800) || (n == 4 && (c < 0x10000 || c > 0x10FFFF)) || (c >= 0xD800 && c <= 0xDFFF)) {
            *out++ = '?';
            ++in;
            continue;
        }
        if (c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029) {
            *out++ = ' ';
        } else if (!preview_is_format(c)) {
            for (k = 0; k < n; ++k) *out++ = (char)in[k];
        }
        in += n;
    }
    *out = '\0';
}

/* A text attribute as UTF-8 for a caption; "" when absent, when it does not fit `cap`, or when its
 * stored value is longer than max_value_bytes (the stored length is looked at first: such a value
 * is not decoded at all). */
static void preview_text(const siaara_dataset* ds, uint32_t tag, const preview_limits* lim, char* out, size_t cap) {
    size_t needed = 0, stored = 0, i;
    const uint8_t* raw = NULL;
    out[0] = '\0';
    if (siaara_dataset_get_raw(ds, tag, &raw, &stored) != SIAARA_OK || stored > lim->max_value_bytes) return;
    if (siaara_dataset_get_utf8(ds, tag, 0, out, cap, &needed) != SIAARA_OK) out[0] = '\0';
    if (lim->strip_control_and_format) {
        preview_clean_text(out);
    } else {
        for (i = 0; out[i] != '\0'; ++i)
            if ((unsigned char)out[i] < 0x20 || out[i] == 0x7F) out[i] = ' ';
    }
}

/* `text` escaped for HTML element content and quoted attribute values (&, <, >, the double quote
 * and the apostrophe become character references), cut where `cap` ends (never inside a
 * reference). Every caption field is text from the file and goes through this before it is put
 * into a page. Returns the length written. */
static size_t preview_html_escape(const char* text, char* out, size_t cap) {
    size_t n = 0;
    if (cap == 0) return 0;
    for (; *text != '\0'; ++text) {
        const char* rep = NULL;
        size_t len = 1;
        switch (*text) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }
        if (rep) len = strlen(rep);
        if (n + len + 1 > cap) break;
        if (rep) memcpy(out + n, rep, len);
        else out[n] = *text;
        n += len;
    }
    out[n] = '\0';
    return n;
}

/* The size that fits box x box and keeps the aspect ratio (never larger than the image). */
static void preview_fit(uint32_t columns, uint32_t rows, uint32_t box, uint32_t* w, uint32_t* h) {
    double scale = 1.0;
    if (columns > box) scale = (double)box / (double)columns;
    if (rows > box && (double)box / (double)rows < scale) scale = (double)box / (double)rows;
    *w = (uint32_t)((double)columns * scale + 0.5);
    *h = (uint32_t)((double)rows * scale + 0.5);
    if (*w == 0) *w = 1;
    if (*h == 0) *h = 1;
}

/* Decodes one frame and renders it with the file's own VOI / presentation defaults (window or VOI
 * LUT, MONOCHROME1, palette, colour) into w x h RGBA8 at dst (stride = stride_pixels). */
static siaara_status preview_render_frame(const siaara_file* file, uint32_t index, uint32_t w, uint32_t h, uint8_t* dst,
                                          uint32_t stride_pixels) {
    siaara_frame* frame = NULL;
    siaara_view2d_options vo;
    size_t needed = 0;
    uint8_t* tile;
    uint32_t y;
    siaara_status st = siaara_frame_open(file, index, &frame); /* decodes this one frame, within the file's limits */
    if (st != SIAARA_OK) return st;
    tile = (uint8_t*)malloc((size_t)w * h * 4u);
    if (!tile) {
        siaara_frame_free(frame);
        return SIAARA_ERR_OUT_OF_MEMORY;
    }
    memset(&vo, 0, sizeof vo);
    vo.struct_size = sizeof vo;
    vo.viewport_width = w;
    vo.viewport_height = h;
    vo.output_format = SIAARA_PIXELS_RGBA8;
    vo.zoom = 0; /* fit the viewport */
    vo.interpolation = SIAARA_INTERP_LINEAR;
    vo.voi_mode = SIAARA_VOI_FILE;
    vo.background_rgba[3] = 255; /* opaque black around the image */
    st = siaara_frame_render(frame, &vo, tile, (size_t)w * h * 4u, &needed);
    if (st == SIAARA_OK)
        for (y = 0; y < h; ++y) memcpy(dst + (size_t)y * stride_pixels * 4u, tile + (size_t)y * w * 4u, (size_t)w * 4u);
    free(tile);
    siaara_frame_free(frame);
    return st;
}

/* The preview of `size` untrusted bytes. Always fills *out (free with preview_free) and returns
 * out->outcome. Never needs more memory than the limits allow, whatever the bytes claim: the read,
 * every decode and every render are held to them. Time is not bounded: see the top of this file. */
static int preview_attachment(const uint8_t* data, size_t size, const preview_limits* lim, preview_result* out) {
    siaara_read_options ro;
    siaara_read_report report;
    siaara_pixel_info pi;
    siaara_transfer_syntax_info tsi;
    siaara_file* file = NULL;
    siaara_dataset* ds = NULL;
    siaara_status st;
    size_t needed = 0;
    uint32_t w = 0, h = 0;
    uint64_t frame_pixels = 0, decoded_pixels = 0;

    memset(out, 0, sizeof *out);
    if (size > lim->max_attachment_bytes) return preview_stop(out, PREVIEW_TOO_LARGE, "the attachment is larger than the preview limit");
    /* A DICOM file starts with 128 bytes of preamble and "DICM". Data sets without that header
     * exist, but an attachment that claims to be a DICOM file has it; nothing else is parsed. */
    if (!data || size < 132 || memcmp(data + 128, "DICM", 4) != 0) return preview_stop(out, PREVIEW_NOT_DICOM, "not a DICOM file");

    /* [limits] Every limit explicitly: nothing is left to defaults meant for trusted local files. */
    memset(&ro, 0, sizeof ro);
    ro.struct_size = sizeof ro;
    ro.allow_raw_dataset = 0;                         /* Part 10 only */
    ro.strict = 0;                                    /* lenient: real-world files are rarely perfect ... */
    ro.recover_overlong_lengths = SIAARA_RECOVER_OFF; /* ... but no guessing at damaged lengths */
    ro.max_depth = lim->max_depth;
    ro.max_elements = lim->max_elements;
    ro.max_inflated_size = lim->max_inflated_size;
    ro.max_frame_bytes = lim->max_frame_bytes;         /* belong to the file handle: every decode below */
    ro.frame_memory_budget = lim->frame_memory_budget; /* and render is held to them */
    ro.max_scans = lim->max_scans;                     /* progressive JPEG: scans per frame */
    ro.max_crop_factor = lim->max_crop_factor;         /* no decoding of a larger codestream to crop it */
    /* [limits] */
    st = siaara_file_open_memory(data, size, &ro, &file);
    if (st != SIAARA_OK) {
        const int outcome = st == SIAARA_ERR_NOT_DICOM ? PREVIEW_NOT_DICOM : st == SIAARA_ERR_LIMIT_EXCEEDED ? PREVIEW_TOO_LARGE : PREVIEW_DAMAGED;
        return preview_stop(out, outcome, siaara_last_error_message());
    }
    if (siaara_file_dataset(file, &ds) != SIAARA_OK) {
        siaara_file_free(file);
        return preview_stop(out, PREVIEW_DAMAGED, siaara_last_error_message());
    }

    /* An incomplete read (truncated attachment, bytes skipped) is not an error here: what was read
     * is previewed and flagged. */
    memset(&report, 0, sizeof report);
    report.struct_size = sizeof report;
    if (siaara_file_read_report(file, &report) == SIAARA_OK) {
        out->read_loss = report.loss;
        out->incomplete = report.loss != 0;
    }

    /* [caption] Safe header fields only. */
    preview_text(ds, 0x00080060u, lim, out->modality, sizeof out->modality);                     /* Modality */
    preview_text(ds, 0x00080020u, lim, out->study_date, sizeof out->study_date);                 /* Study Date */
    preview_text(ds, 0x0008103Eu, lim, out->series_description, sizeof out->series_description); /* Series Description */
    if (siaara_file_transfer_syntax(file, out->transfer_syntax, sizeof out->transfer_syntax, &needed) != SIAARA_OK)
        out->transfer_syntax[0] = '\0';
    /* The file meta is text from the file like any other: a UID is digits and dots, at most 64, or nothing. */
    {
        size_t i = 0;
        for (; out->transfer_syntax[i] != '\0'; ++i) {
            const char c = out->transfer_syntax[i];
            if (i >= 64 || (c != '.' && (c < '0' || c > '9'))) {
                out->transfer_syntax[0] = '\0';
                break;
            }
        }
    }
    memset(&tsi, 0, sizeof tsi);
    tsi.struct_size = sizeof tsi;
    if (siaara_transfer_syntax_info_get(out->transfer_syntax, strlen(out->transfer_syntax), &tsi) == SIAARA_OK && tsi.name)
        snprintf(out->transfer_syntax_name, sizeof out->transfer_syntax_name, "%s", tsi.name);
    /* [caption] */

    /* [pixels] Is there an image, and can it be decoded? Nothing is decoded yet. */
    memset(&pi, 0, sizeof pi);
    pi.struct_size = sizeof pi;
    st = siaara_file_pixel_info(file, &pi);
    if (st != SIAARA_OK) {
        const int outcome = st == SIAARA_ERR_NOT_FOUND     ? PREVIEW_NO_PIXEL_DATA
                            : st == SIAARA_ERR_UNSUPPORTED ? PREVIEW_UNSUPPORTED
                            : st == SIAARA_ERR_LIMIT_EXCEEDED ? PREVIEW_TOO_LARGE
                                                              : PREVIEW_DAMAGED;
        preview_stop(out, outcome, outcome == PREVIEW_NO_PIXEL_DATA ? "no image in this DICOM object" : siaara_last_error_message());
        siaara_file_free(file);
        return outcome;
    }
    out->rows = pi.rows;
    out->columns = pi.columns;
    out->frames = pi.number_of_frames;
    if (pi.rows == 0 || pi.columns == 0 || pi.frame_bytes == 0 || pi.frame_bytes > lim->max_frame_bytes) {
        siaara_file_free(file);
        return preview_stop(out, PREVIEW_TOO_LARGE, "the image is larger than the preview limit");
    }
    /* Pixels, not bytes: a render works at 4 bytes per pixel whatever the bit depth, so a 1-bit
     * frame within max_frame_bytes can still be hundreds of millions of pixels. */
    frame_pixels = (uint64_t)pi.rows * pi.columns;
    if (frame_pixels > lim->max_pixels || frame_pixels > lim->max_total_pixels) {
        siaara_file_free(file);
        return preview_stop(out, PREVIEW_TOO_LARGE, "the image has more pixels than the preview limit");
    }
    decoded_pixels = frame_pixels;
    /* [pixels] */

    /* [render] Frame 0 at a bounded size. */
    preview_fit(pi.columns, pi.rows, lim->max_box, &w, &h);
    out->image.rgba = (uint8_t*)malloc((size_t)w * h * 4u);
    if (!out->image.rgba) {
        siaara_file_free(file);
        return preview_stop(out, PREVIEW_DAMAGED, "out of memory");
    }
    out->image.width = w;
    out->image.height = h;
    st = preview_render_frame(file, 0, w, h, out->image.rgba, w);
    if (st != SIAARA_OK) {
        const int outcome = st == SIAARA_ERR_UNSUPPORTED ? PREVIEW_UNSUPPORTED : st == SIAARA_ERR_LIMIT_EXCEEDED ? PREVIEW_TOO_LARGE : PREVIEW_DAMAGED;
        preview_stop(out, outcome, siaara_last_error_message());
        preview_free(out);
        siaara_file_free(file);
        return outcome;
    }
    /* [render] */

    /* [strip] Multi-frame / cine: a few frames spread over the whole run, never all of them. A
     * frame that cannot be decoded, or that would pass the preview's pixel budget, ends the strip;
     * the main picture stays. */
    if (pi.number_of_frames > 1 && lim->max_strip_frames > 1) {
        const uint32_t tiles = pi.number_of_frames < lim->max_strip_frames ? pi.number_of_frames : lim->max_strip_frames;
        uint32_t tw = 0, th = 0, t;
        preview_fit(pi.columns, pi.rows, lim->strip_box, &tw, &th);
        out->strip.rgba = (uint8_t*)calloc((size_t)tw * tiles * th, 4u);
        if (out->strip.rgba) {
            for (t = 0; t < tiles; ++t) {
                const uint32_t index = (uint32_t)((uint64_t)t * pi.number_of_frames / tiles);
                if (decoded_pixels + frame_pixels > lim->max_total_pixels) break;
                decoded_pixels += frame_pixels;
                if (preview_render_frame(file, index, tw, th, out->strip.rgba + (size_t)t * tw * 4u, tw * tiles) != SIAARA_OK) break;
            }
            if (t == 0) {
                free(out->strip.rgba);
                out->strip.rgba = NULL;
            } else {
                out->strip.width = tw * tiles; /* tiles not reached stay transparent */
                out->strip.height = th;
                out->strip_frames = t;
            }
        }
    }
    /* [strip] */

    siaara_file_free(file);
    out->outcome = PREVIEW_OK;
    return PREVIEW_OK;
}

/* PNG and JPEG of a preview image. Caller owns *out: siaara_buffer_data / siaara_buffer_free. */
static siaara_status preview_encode(const preview_image* image, int jpeg, siaara_buffer** out) {
    const size_t n = (size_t)image->width * image->height * 4u;
    return jpeg ? siaara_encode_jpeg(image->rgba, n, image->width, image->height, SIAARA_PIXELS_RGBA8, 0, out)
                : siaara_encode_png(image->rgba, n, image->width, image->height, SIAARA_PIXELS_RGBA8, -1, out);
}

/* Patient's Name and Patient ID, only when the caller asks for them (they are personal data:
 * decide who may see them before you call this). Opens the bytes again, header only, with the
 * same limits. Returns 0 when read. */
static int preview_patient_fields(const uint8_t* data, size_t size, const preview_limits* lim, char* name, size_t name_cap,
                                  char* id, size_t id_cap) {
    siaara_read_options ro;
    siaara_file* file = NULL;
    siaara_dataset* ds = NULL;
    name[0] = '\0';
    id[0] = '\0';
    if (!data || size > lim->max_attachment_bytes || size < 132 || memcmp(data + 128, "DICM", 4) != 0) return 1;
    memset(&ro, 0, sizeof ro);
    ro.struct_size = sizeof ro;
    ro.stop_before_pixel_data = 1;
    ro.recover_overlong_lengths = SIAARA_RECOVER_OFF;
    ro.max_depth = lim->max_depth;
    ro.max_elements = lim->max_elements;
    ro.max_inflated_size = lim->max_inflated_size;
    if (siaara_file_open_memory(data, size, &ro, &file) != SIAARA_OK) return 1;
    if (siaara_file_dataset(file, &ds) == SIAARA_OK) {
        preview_text(ds, 0x00100010u, lim, name, name_cap); /* Patient's Name */
        preview_text(ds, 0x00100020u, lim, id, id_cap);     /* Patient ID */
    }
    siaara_file_free(file);
    return 0;
}

#endif
