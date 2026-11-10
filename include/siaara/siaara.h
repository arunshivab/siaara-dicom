/*
 * SIAARA DICOM Toolkit - public C API.
 *
 * Conventions (stable ABI):
 *  - Every function returns siaara_status (0 = SIAARA_OK). Details for the last failure on the
 *    calling thread: siaara_last_error_message(). No C++ exception ever crosses this boundary.
 *  - Handles are opaque. Every *_open / *_create has exactly one matching *_free; free(NULL) is a no-op.
 *  - Strings in and out are UTF-8. Output strings use a caller buffer: pass buf/cap; the function
 *    always writes a NUL terminator when cap > 0 and reports the full length (without NUL) in
 *    *needed. If cap is too small it returns SIAARA_ERR_BUFFER_TOO_SMALL.
 *  - Ownership is stated per function: "borrowed" pointers stay valid until the owning handle is
 *    freed, unless the function says otherwise (e.g. raw values and items, below).
 *  - Thread safety: a siaara_file and the data sets borrowed from it may be read from many threads
 *    at once; writes need exclusive access.
 */
#ifndef SIAARA_SIAARA_H
#define SIAARA_SIAARA_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define SIAARA_CALL __cdecl
#  if defined(SIAARA_STATIC)
#    define SIAARA_API
#  elif defined(SIAARA_BUILDING)
#    define SIAARA_API __declspec(dllexport)
#  else
#    define SIAARA_API __declspec(dllimport)
#  endif
#else
#  define SIAARA_CALL
#  define SIAARA_API __attribute__((visibility("default")))
#endif

/* Functions that cannot fail and never throw are declared noexcept for C++ callers. */
#ifdef __cplusplus
#  define SIAARA_NOEXCEPT noexcept
#else
#  define SIAARA_NOEXCEPT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- status codes */
typedef int32_t siaara_status;
enum {
    SIAARA_OK = 0,
    SIAARA_ERR_INVALID_ARGUMENT = 1,
    SIAARA_ERR_IO = 2,
    SIAARA_ERR_NOT_DICOM = 3,
    SIAARA_ERR_TRUNCATED = 4,
    SIAARA_ERR_MALFORMED = 5,
    SIAARA_ERR_UNSUPPORTED = 6,
    SIAARA_ERR_NOT_FOUND = 7,
    SIAARA_ERR_TYPE_MISMATCH = 8,
    SIAARA_ERR_LIMIT_EXCEEDED = 9,
    SIAARA_ERR_OUT_OF_MEMORY = 10,
    SIAARA_ERR_NETWORK = 11,
    SIAARA_ERR_TIMEOUT = 12,
    SIAARA_ERR_REJECTED = 13,
    SIAARA_ERR_CANCELLED = 14,
    SIAARA_ERR_BUFFER_TOO_SMALL = 15,
    SIAARA_ERR_ADDRESS_IN_USE = 16, /* siaara_scp_start(_ex) / siaara_web_server_start: the port is taken;
                                       the message names the port */
    SIAARA_ERR_INTERNAL = 99
};

/* ---------------------------------------------------------------- version */
#define SIAARA_ABI_VERSION 1u
SIAARA_API uint32_t SIAARA_CALL siaara_abi_version(void) SIAARA_NOEXCEPT;
/* Static string, never freed. */
SIAARA_API const char* SIAARA_CALL siaara_version_string(void) SIAARA_NOEXCEPT;
/* Message for the last failed call on this thread; valid until the next call on this thread. */
SIAARA_API const char* SIAARA_CALL siaara_last_error_message(void) SIAARA_NOEXCEPT;

/* ---------------------------------------------------------------- handles */
typedef struct siaara_file siaara_file;        /* a parsed Part 10 file (owns its data sets) */
typedef struct siaara_dataset siaara_dataset;  /* a data set; borrowed from a file or an item */

/* ---------------------------------------------------------------- reading */
typedef struct siaara_read_options {
    uint32_t struct_size;            /* set to sizeof(siaara_read_options) */
    int32_t stop_before_pixel_data;  /* 0/1 */
    int32_t allow_raw_dataset;       /* 0/1, default 1 */
    int32_t strict;                  /* 0 = lenient (default), 1 = strict */
    uint32_t max_depth;              /* 0 = default (64) */
} siaara_read_options;

/* Opens and parses a file. options may be NULL. Caller owns *out: free with siaara_file_free. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_open(const char* utf8_path, size_t path_len,
                                                      const siaara_read_options* options,
                                                      siaara_file** out);
/* Parses a Part 10 stream from memory. The bytes are copied. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_open_memory(const uint8_t* data, size_t size,
                                                             const siaara_read_options* options,
                                                             siaara_file** out);
SIAARA_API void SIAARA_CALL siaara_file_free(siaara_file* file) SIAARA_NOEXCEPT;

/* Creates an empty file (explicit VR little endian). Caller owns *out. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_create(siaara_file** out);

/* Borrowed: the main data set and the file meta group. Valid until the file is freed. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_dataset(siaara_file* file, siaara_dataset** out);
SIAARA_API siaara_status SIAARA_CALL siaara_file_meta(siaara_file* file, siaara_dataset** out);
SIAARA_API siaara_status SIAARA_CALL siaara_file_transfer_syntax(const siaara_file* file, char* buf,
                                                                 size_t cap, size_t* needed);
SIAARA_API siaara_status SIAARA_CALL siaara_file_warning_count(const siaara_file* file, uint32_t* out);
/* Warning `index` (< warning count) as text (string buffer rules: buf / cap / needed). When the
 * read was not complete the first warning starts with "PARSE_INCOMPLETE" (input bytes skipped),
 * "INPUT_TRUNCATED" (the input ended inside an element) or "READ_RECOVERED" (a recovery heuristic
 * was applied). Thread safety: as the file. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_warning(const siaara_file* file, uint32_t index, char* buf,
                                                         size_t cap, size_t* needed);

/* Code of file warning `index`: SIAARA_FILE_WARNING_* (the text says more). */
enum {
    SIAARA_FILE_WARNING_GENERAL = 0,          /* a lenient-mode finding (repair, stray item, ...) */
    SIAARA_FILE_WARNING_PARSE_INCOMPLETE = 1, /* input bytes were skipped (SIAARA_READ_BYTES_SKIPPED) */
    SIAARA_FILE_WARNING_INPUT_TRUNCATED = 2,  /* the input ended early (SIAARA_READ_INPUT_TRUNCATED) */
    SIAARA_FILE_WARNING_READ_RECOVERED = 3    /* damaged input recovered by a heuristic (SIAARA_READ_RECOVERED) */
};
SIAARA_API siaara_status SIAARA_CALL siaara_file_warning_code(const siaara_file* file, uint32_t index, int32_t* out);

/* Whether the data set holds everything the input held (bit mask). */
enum {
    SIAARA_READ_BYTES_SKIPPED = 1u << 0,   /* lenient parse skipped input bytes that are in no element */
    SIAARA_READ_INPUT_TRUNCATED = 1u << 1, /* the input ended inside an element, item or sequence */
    SIAARA_READ_HEADER_ONLY = 1u << 2,     /* stop_before_pixel_data: Pixel Data and the rest not read */
    SIAARA_READ_RECOVERED = 1u << 3        /* lenient reading recovered damaged input by a heuristic (an element
                                              length corrected, ACR-NEMA implicit VR big endian detected): the data
                                              set is an interpretation; the warnings say what was done */
};
typedef struct siaara_read_report {
    uint32_t struct_size;            /* set to sizeof(siaara_read_report) */
    uint32_t loss;                   /* SIAARA_READ_* bits; 0 = complete */
    uint64_t first_loss_offset;      /* offset of the first loss (inflated stream for deflate) */
    uint64_t bytes_not_read;         /* input bytes in no element */
    uint64_t stopped_at_offset;      /* header-only read: offset of the Pixel Data element, else 0 */
    uint32_t warning_count;
    uint32_t reserved0;
} siaara_read_report;
/* Fills *out (struct_size must be set; a smaller struct_size gets the fields that fit). A save of a
 * file with loss != 0 is refused unless siaara_save_options.allow_incomplete is 1. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_read_report(const siaara_file* file, siaara_read_report* out);

/* Writes the file. transfer_syntax_uid may be NULL to keep the current one. Uses the default
 * siaara_save_options: an unedited file saved in its own transfer syntax is reproduced byte for
 * byte (see siaara_file_save_ex). */
SIAARA_API siaara_status SIAARA_CALL siaara_file_save(const siaara_file* file, const char* utf8_path,
                                                      size_t path_len, const char* transfer_syntax_uid);

/* ---------------------------------------------------------------- data set access */
/* Tags are (group << 16) | element. vm_index is 0-based. */
SIAARA_API int32_t SIAARA_CALL siaara_dataset_contains(const siaara_dataset* ds, uint32_t tag) SIAARA_NOEXCEPT;
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_vr(const siaara_dataset* ds, uint32_t tag, char out_vr[3]);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_vm(const siaara_dataset* ds, uint32_t tag, uint32_t* out);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_string(const siaara_dataset* ds, uint32_t tag,
                                                               uint32_t vm_index, char* buf, size_t cap,
                                                               size_t* needed);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_int64(const siaara_dataset* ds, uint32_t tag,
                                                              uint32_t vm_index, int64_t* out);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_double(const siaara_dataset* ds, uint32_t tag,
                                                               uint32_t vm_index, double* out);
/* Raw value bytes, borrowed (valid until the owning file is freed or this element is changed or
 * removed; changes to other elements do not invalidate it).
 * Byte order: always little endian, whatever the transfer syntax. Data sets hold values little
 * endian in memory, so for Explicit VR Big Endian files multi-byte binary values (US, UL, FL, OW,
 * AT ...) come back byte-swapped relative to the file. Strings and OB/UN are never swapped. For
 * the bytes exactly as stored in the file use siaara_file_get_raw_stored. */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_raw(const siaara_dataset* ds, uint32_t tag,
                                                            const uint8_t** out_data, size_t* out_size);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_item_count(const siaara_dataset* ds, uint32_t tag,
                                                               uint32_t* out);
/* Borrowed item data set of a sequence. Valid until the file is freed, this item is removed, or
 * the sequence element is changed or removed. Adding, changing or removing other elements of the
 * parent, or other items of the sequence, does not invalidate it (items have stable storage). */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_item(siaara_dataset* ds, uint32_t tag,
                                                         uint32_t index, siaara_dataset** out);
/* Number of top-level elements and the tag at a position (for iteration). */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_element_count(const siaara_dataset* ds, uint32_t* out);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_element_tag(const siaara_dataset* ds, uint32_t index,
                                                                uint32_t* out_tag);

/* Setters. vr is two ASCII characters, e.g. "PN". */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_set_string(siaara_dataset* ds, uint32_t tag,
                                                               const char* vr, const char* utf8,
                                                               size_t len);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_set_uint16(siaara_dataset* ds, uint32_t tag, uint16_t v);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_set_uint32(siaara_dataset* ds, uint32_t tag, uint32_t v);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_set_bytes(siaara_dataset* ds, uint32_t tag,
                                                              const char* vr, const uint8_t* data,
                                                              size_t size);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_remove(siaara_dataset* ds, uint32_t tag);

/* ---------------------------------------------------------------- dictionary */
typedef struct siaara_dict_entry {
    uint32_t struct_size;   /* set to sizeof(siaara_dict_entry) */
    uint32_t tag;
    char vr[3];
    char vr_alt[3];
    int32_t retired;
    const char* keyword;    /* static, NUL-terminated */
    const char* name;       /* static, NUL-terminated */
    const char* vm;         /* static, NUL-terminated */
} siaara_dict_entry;

SIAARA_API siaara_status SIAARA_CALL siaara_dict_lookup(uint32_t tag, siaara_dict_entry* out);
SIAARA_API siaara_status SIAARA_CALL siaara_dict_lookup_keyword(const char* keyword, size_t len,
                                                                siaara_dict_entry* out);

/* Private dictionary, built only from vendor DICOM Conformance Statements (docs/PRIVATE_DICTIONARY.md).
 * A key is the Private Creator string + group + the low byte of the element in the creator's
 * block ((gggg,xxEE): EE). */
enum {
    SIAARA_PRIVATE_STATED = 0,   /* a document states the creator for this element */
    SIAARA_PRIVATE_ASSUMED = 1,  /* creator assumed from a nearby declaration (lower confidence) */
    SIAARA_PRIVATE_EXACT = 0,    /* creator matched as written (trailing spaces / NULs ignored) */
    SIAARA_PRIVATE_FUZZY = 1     /* matched a spelling variant ignoring ASCII case and whitespace */
};
typedef struct siaara_private_dict_entry {
    uint32_t struct_size;      /* set to sizeof(siaara_private_dict_entry) */
    uint16_t group;
    uint8_t element;           /* low byte of the element */
    char vr[3];                /* "" when undecided (conflict, or no document states a VR) */
    int32_t retired;           /* 1 when a document marks it retired */
    int32_t conflict;          /* 1 when documents disagree on the VR (see siaara_dict_private_candidate) */
    int32_t vm_conflict;       /* 1 when documents disagree on the VM (vm is "") */
    int32_t confidence;        /* SIAARA_PRIVATE_STATED / SIAARA_PRIVATE_ASSUMED */
    int32_t match;             /* SIAARA_PRIVATE_EXACT / SIAARA_PRIVATE_FUZZY */
    uint32_t source_page;      /* primary source: physical PDF page */
    uint32_t source_count;     /* documents that define this key */
    uint32_t candidate_count;  /* VR candidates (siaara_dict_private_candidate) */
    const char* creator;       /* the dictionary's spelling; static, NUL-terminated (as are all below) */
    const char* keyword;       /* may be "" */
    const char* name;          /* may be "" */
    const char* vm;            /* "" when undecided or not stated */
    const char* vendor;
    const char* source_document;
} siaara_private_dict_entry;

typedef struct siaara_private_dict_candidate {
    uint32_t struct_size;  /* set to sizeof(siaara_private_dict_candidate) */
    char vr[3];
    uint32_t page;         /* physical PDF page */
    const char* document;  /* static, NUL-terminated */
} siaara_private_dict_candidate;

/* Looks up creator (UTF-8, len bytes, as stored in (gggg,00xx)) + group + element low byte. The
 * exact creator is tried first, then its case/whitespace spelling variants. NotFound if unknown. */
SIAARA_API siaara_status SIAARA_CALL siaara_dict_lookup_private(const char* creator, size_t len, uint16_t group,
                                                                uint8_t element_low_byte,
                                                                siaara_private_dict_entry* out);
/* Looks up the private data element `tag` (gggg,xxEE) of ds through its Private Creator (gggg,00xx). */
SIAARA_API siaara_status SIAARA_CALL siaara_dict_lookup_private_element(const siaara_dataset* ds, uint32_t tag,
                                                                        siaara_private_dict_entry* out);
/* Candidate `index` (0 .. candidate_count-1) of the entry found for creator + group + element: a
 * VR conflict lists every (VR, document) pair, a decided VR has one candidate (the primary
 * source), an entry whose documents state no VR has none. NotFound past the end. */
SIAARA_API siaara_status SIAARA_CALL siaara_dict_private_candidate(const char* creator, size_t len, uint16_t group,
                                                                   uint8_t element_low_byte, uint32_t index,
                                                                   siaara_private_dict_candidate* out);

/* ---------------------------------------------------------------- pixel data */
/* Image Pixel module of the file's main data set (PS3.3 C.7.6.3), validated. */
typedef struct siaara_pixel_info {
    uint32_t struct_size;            /* set to sizeof(siaara_pixel_info) */
    uint32_t rows;
    uint32_t columns;
    uint32_t samples_per_pixel;
    uint32_t bits_allocated;
    uint32_t bits_stored;
    uint32_t high_bit;
    int32_t pixel_representation;    /* 0 unsigned, 1 two's complement */
    int32_t planar_configuration;    /* 0 colour-by-pixel, 1 colour-by-plane */
    uint32_t number_of_frames;
    int32_t lossy;                   /* 1 if Lossy Image Compression is "01" or the transfer syntax is lossy */
    int32_t sample_kind;             /* SIAARA_SAMPLES_INTEGER, _FLOAT32 or _FLOAT64 */
    int32_t encapsulated;            /* 1 if the pixel data is encapsulated (compressed) */
    uint64_t frame_bytes;            /* bytes of one frame from siaara_frame_decode with SIAARA_FRAME_NATIVE */
    char photometric[20];            /* Photometric Interpretation, NUL-terminated (truncated if longer) */
} siaara_pixel_info;

enum {
    SIAARA_SAMPLES_INTEGER = 0,
    SIAARA_SAMPLES_FLOAT32 = 1,
    SIAARA_SAMPLES_FLOAT64 = 2
};

SIAARA_API siaara_status SIAARA_CALL siaara_file_pixel_info(const siaara_file* file, siaara_pixel_info* out);
SIAARA_API siaara_status SIAARA_CALL siaara_frame_count(const siaara_file* file, uint32_t* out);

/* Frame output formats for siaara_frame_decode. */
enum {
    /* DICOM native layout of the file's own attributes: little-endian samples of Bits Allocated
     * bits, Planar Configuration as stored, 1-bit frames bit-packed from bit 0, native YBR 4:2:2
     * as stored (Y Y Cb Cr). Size: siaara_pixel_info.frame_bytes. */
    SIAARA_FRAME_NATIVE = 0,
    /* float32 per sample after the Modality LUT / rescale (grayscale; colour samples unchanged),
     * colour-by-pixel. Size: rows * columns * samples_per_pixel * 4 bytes. */
    SIAARA_FRAME_FLOAT32_MODALITY = 1
};

/* Decodes one frame (0-based) into buf. *needed receives the required size. With buf == NULL and
 * cap == 0 the call only reports the size. cap < needed: SIAARA_ERR_BUFFER_TOO_SMALL. */
SIAARA_API siaara_status SIAARA_CALL siaara_frame_decode(const siaara_file* file, uint32_t frame, int32_t format,
                                                         uint8_t* buf, size_t cap, size_t* needed);

enum {
    SIAARA_VOI_FILE = 0,      /* the file's first window / VOI LUT; full pixel range if it has none */
    SIAARA_VOI_EXPLICIT = 1,  /* window_center / window_width below (LINEAR, width >= 1) */
    SIAARA_VOI_FULL_RANGE = 2 /* minimum .. maximum of the frame */
};

typedef struct siaara_render_options {
    uint32_t struct_size;     /* set to sizeof(siaara_render_options) */
    uint32_t frame;           /* 0-based */
    int32_t voi_mode;         /* SIAARA_VOI_* */
    int32_t invert;           /* 0/1: invert on top of MONOCHROME1 / Presentation LUT Shape */
    double window_center;
    double window_width;
    int32_t no_overlays;      /* 0/1 */
    int32_t no_shutters;      /* 0/1 */
} siaara_render_options;

/* Renders a frame to RGBA8 (rows * columns * 4 bytes). options may be NULL (frame 0, file VOI).
 * Buffer rules as for siaara_frame_decode. */
SIAARA_API siaara_status SIAARA_CALL siaara_render_rgba8(const siaara_file* file, const siaara_render_options* options,
                                                         uint8_t* buf, size_t cap, size_t* needed);
/* ---------------------------------------------------------------- character sets (PS3.5 6.1) */
/* Value vm_index of a text element decoded to UTF-8 using the data set's Specific Character Set
 * (0008,0005), padding removed. Undecodable bytes become U+FFFD. Output buffer convention as
 * above. A sequence item that has no (0008,0005) of its own uses the value in force in its
 * enclosing data set, which the item carries: it is recorded when the file (or JSON / XML) is
 * read, when a sequence or (0008,0005) is set or removed, and by siaara_dataset_append_item, so
 * an item handle decodes correctly on its own. siaara_dataset_get_utf8_cs overrides it. */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_utf8(const siaara_dataset* ds, uint32_t tag,
                                                             uint32_t vm_index, char* buf, size_t cap,
                                                             size_t* needed);
/* Encodes utf8 (all values, backslash separated) into the data set's Specific Character Set (for
 * an item without its own, the inherited one as above) and stores it. Fails with SIAARA_ERR_UNSUPPORTED, leaving the data set unchanged, when a character
 * cannot be represented; the message gives the count and the first byte offset. */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_set_utf8(siaara_dataset* ds, uint32_t tag,
                                                             const char* vr, const char* utf8,
                                                             size_t len);
/* As above, with the Specific Character Set to use when ds has no (0008,0005) of its own
 * (the enclosing data set's raw value, for sequence items). inherited may be NULL: then the
 * value the item carries is used, as siaara_dataset_get_utf8 does. */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_utf8_cs(const siaara_dataset* ds, uint32_t tag,
                                                                uint32_t vm_index, const char* inherited,
                                                                size_t inherited_len, char* buf,
                                                                size_t cap, size_t* needed);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_set_utf8_cs(siaara_dataset* ds, uint32_t tag,
                                                                const char* vr, const char* utf8,
                                                                size_t len, const char* inherited,
                                                                size_t inherited_len);

/* ---------------------------------------------------------------- DICOM JSON (PS3.18 Annex F) */
enum {
    SIAARA_JSON_DEFAULT = 0,
    SIAARA_JSON_NUMBERS_AS_STRINGS = 1, /* DS and IS values as JSON strings */
    SIAARA_JSON_STRICT_CHARSET = 2,     /* fail instead of writing U+FFFD for unconvertible text */
    SIAARA_JSON_PRIVATE_NAMES = 4       /* add "keyword" / "name" to private attributes the private
                                           dictionary knows (not PS3.18 members; readers ignore them) */
};

/* Encodes a data set as one DICOM JSON object (UTF-8, compact). Buffer rules as for strings:
 * *needed receives the length without the NUL terminator. Binary values are written inline
 * (InlineBinary); encapsulated pixel data as its encapsulated byte stream. */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_to_json(const siaara_dataset* ds, uint32_t flags, char* buf,
                                                            size_t cap, size_t* needed);
/* Parses one DICOM JSON object into a new file (explicit VR little endian unless the JSON carries
 * a Transfer Syntax UID (0002,0010); group 0002 elements go to the file meta). BulkDataURI
 * values are left empty. Caller owns *out: free with siaara_file_free. */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_from_json(const char* utf8, size_t len, siaara_file** out);

/* ---------------------------------------------------------------- DICOMweb client (PS3.18) */
/* A synchronous client with one persistent HTTP connection. Not thread safe: use one per thread. */
typedef struct siaara_web_client siaara_web_client;

/* base_url: "http://host:port/path" or "https://host:port/path" (https: the OS trust store, no
 * client certificate; siaara_web_client_create_tls takes TLS options). timeout_ms applies to the
 * connect and to every network wait; 0 = default (30 s). Caller owns *out. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_client_create(const char* base_url, size_t len, int32_t timeout_ms,
                                                              siaara_web_client** out);
SIAARA_API void SIAARA_CALL siaara_web_client_free(siaara_web_client* client) SIAARA_NOEXCEPT;
/* Authorization header value sent with every request, e.g. "Bearer abc" or "Basic dXNlcjpwdw==".
 * value NULL (or len 0) removes it. The value is copied. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_client_set_authorization(siaara_web_client* client, const char* value,
                                                                         size_t len);

enum {
    SIAARA_QIDO_STUDIES = 0,
    SIAARA_QIDO_SERIES = 1,
    SIAARA_QIDO_INSTANCES = 2
};
/* QIDO-RS search. study_uid / series_uid (NUL-terminated, may be NULL) narrow the search path.
 * query: the URL query string without '?', already percent-encoded, e.g. "PatientID=X&limit=10"
 * (may be NULL). Writes the application/dicom+json result array to buf. The response stays in
 * the client, so after SIAARA_ERR_BUFFER_TOO_SMALL siaara_web_last_response() returns it
 * without a new request. HTTP failures return SIAARA_ERR_REJECTED (404: SIAARA_ERR_NOT_FOUND). */
SIAARA_API siaara_status SIAARA_CALL siaara_web_qido(siaara_web_client* client, int32_t level, const char* study_uid,
                                                     const char* series_uid, const char* query, size_t query_len,
                                                     char* buf, size_t cap, size_t* needed);
/* Body of the last successful QIDO response (string buffer rules). */
SIAARA_API siaara_status SIAARA_CALL siaara_web_last_response(const siaara_web_client* client, char* buf, size_t cap,
                                                              size_t* needed);
/* WADO-RS retrieve of one instance (as stored by the server). Caller owns *out. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_instance(siaara_web_client* client, const char* study_uid,
                                                              const char* series_uid, const char* instance_uid,
                                                              siaara_file** out);
/* STOW-RS of one file (written in its current transfer syntax). study_uid may be NULL.
 * Returns SIAARA_OK when stored, SIAARA_ERR_REJECTED when the server refused the instance.
 * A file whose read was not complete (siaara_file_read_report: truncated input, skipped bytes,
 * heuristic recovery, header-only read) is not sent: SIAARA_ERR_REJECTED with a message starting
 * "INCOMPLETE_INPUT", before any request, as for C-STORE (siaara_web_stow_ex can allow it). */
SIAARA_API siaara_status SIAARA_CALL siaara_web_stow(siaara_web_client* client, const siaara_file* file,
                                                     const char* study_uid);

/* ---------------------------------------------------------------- index records (appended) */
/* One flat record per instance (or per patient/study/series in store query results) for viewer
 * databases. Numbers are fields; text is UTF-8 in a caller-supplied string table: string k is
 * strings + str_offset[k], NUL-terminated, str_length[k] bytes (offset 0 / length 0 = empty).
 * Calls that fill a record take buf/cap/needed for the string table: buf == NULL with cap == 0
 * only reports *needed; a too-small buffer gives SIAARA_ERR_BUFFER_TOO_SMALL (the numeric fields
 * and offsets are still filled). The layout has no implicit padding on any platform. */
enum {
    SIAARA_IDX_PATIENT_ID = 0,
    SIAARA_IDX_ISSUER_OF_PATIENT_ID = 1,
    SIAARA_IDX_PATIENT_NAME = 2,
    SIAARA_IDX_PATIENT_BIRTH_DATE = 3,
    SIAARA_IDX_PATIENT_SEX = 4,
    SIAARA_IDX_STUDY_INSTANCE_UID = 5,
    SIAARA_IDX_STUDY_DATE = 6,
    SIAARA_IDX_STUDY_TIME = 7,
    SIAARA_IDX_ACCESSION_NUMBER = 8,
    SIAARA_IDX_STUDY_ID = 9,
    SIAARA_IDX_STUDY_DESCRIPTION = 10,
    SIAARA_IDX_REFERRING_PHYSICIAN = 11,
    SIAARA_IDX_SERIES_INSTANCE_UID = 12,
    SIAARA_IDX_MODALITY = 13,
    SIAARA_IDX_SERIES_DESCRIPTION = 14,
    SIAARA_IDX_BODY_PART_EXAMINED = 15,
    SIAARA_IDX_SERIES_DATE = 16,
    SIAARA_IDX_SERIES_TIME = 17,
    SIAARA_IDX_FRAME_OF_REFERENCE_UID = 18,
    SIAARA_IDX_SOP_CLASS_UID = 19,
    SIAARA_IDX_SOP_INSTANCE_UID = 20,
    SIAARA_IDX_TRANSFER_SYNTAX_UID = 21,
    SIAARA_IDX_PHOTOMETRIC = 22,
    SIAARA_IDX_CONTENT_DATE = 23,
    SIAARA_IDX_CONTENT_TIME = 24,
    SIAARA_IDX_IMAGE_TYPE = 25,
    SIAARA_IDX_SPECIFIC_CHARACTER_SET = 26,
    SIAARA_IDX_FILE_PATH = 27,            /* store results: absolute path; DICOMDIR: resolved file */
    SIAARA_IDX_MODALITIES_IN_STUDY = 28,  /* store study/series/instance results */
    SIAARA_IDX_RECORD_TYPE = 29,          /* DICOMDIR: Directory Record Type */
    SIAARA_IDX_COUNT = 30
};
#define SIAARA_INDEX_MAX_STRINGS 32

enum {
    SIAARA_IDXF_SERIES_NUMBER = 1 << 0,
    SIAARA_IDXF_INSTANCE_NUMBER = 1 << 1,
    SIAARA_IDXF_POSITION = 1 << 2,        /* image_position valid */
    SIAARA_IDXF_ORIENTATION = 1 << 3,     /* image_orientation valid */
    SIAARA_IDXF_PIXEL_DATA = 1 << 4,
    SIAARA_IDXF_ENCAPSULATED = 1 << 5,
    SIAARA_IDXF_PREAMBLE = 1 << 6,
    SIAARA_IDXF_PIXEL_SPACING = 1 << 7,
    SIAARA_IDXF_SLICE_THICKNESS = 1 << 8,
    SIAARA_IDXF_REFERENCED = 1 << 9,      /* store: indexed in place, not copied */
    SIAARA_IDXF_PIXEL_OFFSET = 1 << 10    /* pixel_data_offset / _length are valid file offsets */
};

enum {
    SIAARA_LEVEL_PATIENT = 0,
    SIAARA_LEVEL_STUDY = 1,
    SIAARA_LEVEL_SERIES = 2,
    SIAARA_LEVEL_INSTANCE = 3
};

typedef struct siaara_index_record {
    uint32_t struct_size;          /* set to sizeof(siaara_index_record) */
    int32_t level;                 /* SIAARA_LEVEL_* */
    uint32_t flags;                /* SIAARA_IDXF_* */
    int32_t series_number;
    int32_t instance_number;
    uint32_t number_of_frames;     /* 0 when there is no pixel data */
    uint32_t rows;
    uint32_t columns;
    uint16_t bits_allocated;
    uint16_t bits_stored;
    uint16_t high_bit;
    uint16_t samples_per_pixel;
    uint16_t pixel_representation;
    uint16_t reserved0;
    uint32_t num_studies;          /* store results: children counts */
    uint32_t num_series;
    uint32_t num_instances;
    uint64_t file_size;
    uint64_t pixel_data_offset;    /* value field of Pixel Data (first item if encapsulated) */
    uint64_t pixel_data_length;
    double image_position[3];
    double image_orientation[6];
    double pixel_spacing[2];
    double slice_thickness;
    uint32_t str_offset[SIAARA_INDEX_MAX_STRINGS];
    uint32_t str_length[SIAARA_INDEX_MAX_STRINGS];
} siaara_index_record;

/* One parse of a file (header and pixel data position; pixel data is not decoded). */
SIAARA_API siaara_status SIAARA_CALL siaara_extract_index(const char* utf8_path, size_t path_len,
                                                          siaara_index_record* out, char* buf,
                                                          size_t cap, size_t* needed);
SIAARA_API siaara_status SIAARA_CALL siaara_extract_index_memory(const uint8_t* data, size_t size,
                                                                 siaara_index_record* out, char* buf,
                                                                 size_t cap, size_t* needed);

/* ---------------------------------------------------------------- local store (appended) */
/* A folder holding copied (or in-place referenced) instances and a crash-safe index. One
 * process may open a store at a time (SIAARA_ERR_REJECTED otherwise). A siaara_store may be
 * used from many threads at once. */
typedef struct siaara_store siaara_store;
typedef struct siaara_store_result siaara_store_result;

typedef struct siaara_store_options {
    uint32_t struct_size;          /* set to sizeof(siaara_store_options) */
    int32_t create;                /* 0/1, default 1 */
    int32_t durable;               /* 0/1: fsync every change (slower, survives power loss) */
    int32_t rebuild_if_corrupt;    /* 0/1: re-index the data folder if the index is unreadable */
    uint64_t compact_min_records;  /* 0 = default (50,000) */
    /* Fields below are read only when struct_size covers them. */
    uint64_t max_total_bytes;      /* copied instances may take at most this much; 0 = no cap. Adding
                                      more fails with SIAARA_ERR_LIMIT_EXCEEDED (a Storage SCP answers 0xA700) */
    uint64_t min_free_disk_bytes;  /* free space adding leaves on the volume; 0 = default (64 MiB) */
    /* Permissions of what the store creates (R-092). 0 (default): owner only - folders 0700 and
     * files 0600 on POSIX, an owner-only DACL on Windows. Otherwise the POSIX mode of the files
     * (e.g. 0640); folders get it plus the matching search bits, and on Windows the folders and
     * files inherit the parent folder's permissions. */
    uint32_t file_mode;
    /* An existing store whose index or data folder is open to group / others beyond file_mode (made
     * by an older version, or copied): 0 (default): on POSIX the extra permission bits are removed
     * when this user owns the folder; 1: left as they are. Either way a warning is logged
     * (STORE_PERMISSIONS_TIGHTENED / STORE_PERMISSIONS_WIDE). Not checked on Windows. (This field
     * was reserved0, set to 0.) */
    uint32_t keep_permissions;
} siaara_store_options;

enum { SIAARA_DUP_KEEP = 0, SIAARA_DUP_REPLACE = 1 };
enum { SIAARA_ADD_COPY = 0, SIAARA_ADD_REFERENCE = 1 };
enum { SIAARA_ADD_ADDED = 0, SIAARA_ADD_REPLACED = 1, SIAARA_ADD_KEPT_EXISTING = 2 };

/* options may be NULL. Caller owns *out: free with siaara_store_free. */
SIAARA_API siaara_status SIAARA_CALL siaara_store_open(const char* utf8_root, size_t root_len,
                                                       const siaara_store_options* options,
                                                       siaara_store** out);
SIAARA_API void SIAARA_CALL siaara_store_free(siaara_store* store) SIAARA_NOEXCEPT;
/* policy SIAARA_DUP_*, mode SIAARA_ADD_*; *outcome (may be NULL) receives SIAARA_ADD_ADDED etc. */
SIAARA_API siaara_status SIAARA_CALL siaara_store_add_file(siaara_store* store, const char* utf8_path,
                                                           size_t path_len, int32_t policy, int32_t mode,
                                                           int32_t* outcome);
/* The bytes are copied into the store (a raw data set is stored as a Part 10 file). */
SIAARA_API siaara_status SIAARA_CALL siaara_store_add_memory(siaara_store* store, const uint8_t* data,
                                                             size_t size, int32_t policy, int32_t* outcome);

/* Filters: NUL-terminated UTF-8, NULL or "" = no filter. Text filters accept the C-FIND
 * wildcards '*' and '?'. Names and descriptions match case- and (Latin) accent-insensitively and
 * a name matches any of its alphabetic / ideographic / phonetic groups. Dates take a single date
 * or a range "YYYYMMDD-YYYYMMDD", "-YYYYMMDD", "YYYYMMDD-". modality may list several values
 * separated by backslashes. UIDs are exact. */
typedef struct siaara_store_filter {
    uint32_t struct_size;          /* set to sizeof(siaara_store_filter) */
    int32_t level;                 /* SIAARA_LEVEL_* of the rows returned */
    const char* patient_name;
    const char* patient_id;
    const char* patient_birth_date;
    const char* patient_sex;
    const char* study_date;
    const char* accession_number;
    const char* study_description;
    const char* referring_physician;
    const char* modality;
    const char* series_description;
    const char* body_part;
    const char* study_uid;
    const char* series_uid;
    const char* sop_instance_uid;
    uint32_t offset;               /* rows to skip (paging) */
    uint32_t limit;                /* 0 = all */
    uint32_t flags;                /* SIAARA_QUERY_* */
    uint32_t reserved0;
} siaara_store_filter;

/* The query may stop as soon as offset + limit rows are known: much faster paging over large
 * stores, but the total reported by siaara_store_result_count is then a lower bound. */
#define SIAARA_QUERY_NO_TOTAL 1u

/* Results are a snapshot: later changes to the store do not alter them. Caller owns *out. */
SIAARA_API siaara_status SIAARA_CALL siaara_store_query(const siaara_store* store, const siaara_store_filter* query,
                                                        siaara_store_result** out);
/* Series of a study (ordered by Series Number), instances of a series (ordered by Instance
 * Number, then position along the slice normal), each with SIAARA_IDX_FILE_PATH. */
SIAARA_API siaara_status SIAARA_CALL siaara_store_get_series(const siaara_store* store, const char* study_uid,
                                                             size_t len, siaara_store_result** out);
SIAARA_API siaara_status SIAARA_CALL siaara_store_get_instances(const siaara_store* store, const char* series_uid,
                                                                size_t len, siaara_store_result** out);
/* *count = rows held; *total (may be NULL) = matches before offset/limit; *total_exact (may be
 * NULL) = 0 when SIAARA_QUERY_NO_TOTAL stopped the query early and *total is a lower bound. */
SIAARA_API siaara_status SIAARA_CALL siaara_store_result_count(const siaara_store_result* result, uint32_t* count,
                                                               uint32_t* total, int32_t* total_exact);
SIAARA_API siaara_status SIAARA_CALL siaara_store_result_get(const siaara_store_result* result, uint32_t index,
                                                             siaara_index_record* out, char* buf, size_t cap,
                                                             size_t* needed);
SIAARA_API void SIAARA_CALL siaara_store_result_free(siaara_store_result* result) SIAARA_NOEXCEPT;

/* key: for SIAARA_LEVEL_PATIENT the key from siaara_store_patient_key, otherwise the Study,
 * Series or SOP Instance UID. Copied files are deleted;
 * referenced files are left alone. *removed (may be NULL) = instances removed. */
SIAARA_API siaara_status SIAARA_CALL siaara_store_delete(siaara_store* store, int32_t level, const char* key,
                                                         size_t len, uint32_t* removed);
/* Copies the instances into dest (replacing duplicates) and removes them from store. */
SIAARA_API siaara_status SIAARA_CALL siaara_store_move(siaara_store* store, siaara_store* dest, int32_t level,
                                                       const char* key, size_t len, uint32_t* moved);
/* The key identifying the patient of a row (Patient ID + issuer, or name + birth date). */
SIAARA_API siaara_status SIAARA_CALL siaara_store_patient_key(const siaara_store_result* result, uint32_t index,
                                                              char* buf, size_t cap, size_t* needed);

typedef struct siaara_store_verify_report {
    uint32_t struct_size;          /* set to sizeof(siaara_store_verify_report) */
    uint32_t checked;
    uint32_t missing;              /* indexed, file absent */
    uint32_t size_mismatch;
    uint32_t content_mismatch;     /* deep check: file holds another instance or is unreadable */
    uint32_t orphans;              /* files in the data folder the index does not know */
    uint32_t repaired;
    uint32_t reserved0;
} siaara_store_verify_report;

SIAARA_API siaara_status SIAARA_CALL siaara_store_verify(siaara_store* store, int32_t deep, int32_t repair,
                                                         siaara_store_verify_report* out);
/* Re-indexes the data folder and every referenced file still present. */
SIAARA_API siaara_status SIAARA_CALL siaara_store_rebuild(siaara_store* store);
SIAARA_API siaara_status SIAARA_CALL siaara_store_compact(siaara_store* store);

enum { SIAARA_EVENT_ADDED = 0, SIAARA_EVENT_REPLACED = 1, SIAARA_EVENT_DELETED = 2, SIAARA_EVENT_REBUILT = 3 };
/* Called after each change commits, on the thread that made it, with no store lock held (the
 * callback may query or change the store). The strings are valid during the call only; they
 * are "" for SIAARA_EVENT_REBUILT. */
typedef void (SIAARA_CALL* siaara_store_callback)(void* user, int32_t event, const char* sop_instance_uid,
                                                  const char* series_uid, const char* study_uid);
/* cb == NULL removes the callback. */
SIAARA_API siaara_status SIAARA_CALL siaara_store_set_callback(siaara_store* store, siaara_store_callback cb,
                                                               void* user);

typedef struct siaara_store_stats {
    uint32_t struct_size;          /* set to sizeof(siaara_store_stats) */
    uint32_t reserved0;
    uint64_t patients;
    uint64_t studies;
    uint64_t series;
    uint64_t instances;
    uint64_t data_bytes;
    uint64_t journal_bytes;
    uint64_t journal_records;
    uint64_t snapshot_bytes;
    uint64_t generation;
    uint64_t replayed_records;     /* journal records applied at open */
    uint64_t recovered_bytes;      /* torn journal tail removed at open */
} siaara_store_stats;

SIAARA_API siaara_status SIAARA_CALL siaara_store_stats_get(const siaara_store* store, siaara_store_stats* out);

/* ---------------------------------------------------------------- folder scan (appended) */
enum { SIAARA_SCAN_INSTANCE = 0, SIAARA_SCAN_DICOMDIR = 1, SIAARA_SCAN_NOT_DICOM = 2, SIAARA_SCAN_ERROR = 3 };

typedef struct siaara_scan_options {
    uint32_t struct_size;          /* set to sizeof(siaara_scan_options) */
    uint32_t threads;              /* 0 = hardware concurrency (at most 16) */
    int32_t recursive;             /* 0/1, default 1 */
    int32_t follow_symlinks;       /* 0/1 */
    int32_t allow_raw;             /* 0/1, default 1: accept files without preamble */
    int32_t report_skipped;        /* 0/1: also call back for non-DICOM files */
    int32_t use_dicomdir;          /* 0/1: scan only the files <root>/DICOMDIR lists, if present */
    uint32_t reserved0;
    uint64_t max_files;            /* 0 = no limit */
} siaara_scan_options;

typedef struct siaara_scan_stats {
    uint32_t struct_size;          /* set to sizeof(siaara_scan_stats) */
    int32_t used_dicomdir;
    uint64_t files;
    uint64_t instances;
    uint64_t dicomdirs;
    uint64_t skipped;
    uint64_t errors;
} siaara_scan_stats;

/* Called once per file (never concurrently; possibly from different threads). rec and strings
 * are valid during the call only (rec is NULL for SIAARA_SCAN_NOT_DICOM / _ERROR). Return 0 to
 * continue, non-zero to stop (the scan then returns SIAARA_ERR_CANCELLED). */
typedef int32_t (SIAARA_CALL* siaara_scan_callback)(void* user, const char* path, int32_t kind,
                                                    siaara_status status, const siaara_index_record* rec,
                                                    const char* strings);
/* options and stats may be NULL. */
SIAARA_API siaara_status SIAARA_CALL siaara_folder_scan(const char* utf8_root, size_t root_len,
                                                        const siaara_scan_options* options,
                                                        siaara_scan_callback cb, void* user,
                                                        siaara_scan_stats* stats);

/* ---------------------------------------------------------------- DICOMDIR (appended) */
typedef struct siaara_dicomdir_entry {
    uint32_t struct_size;          /* sizeof(siaara_dicomdir_entry) */
    uint32_t index;                /* position in the Directory Record Sequence */
    int32_t parent;                /* parent record index, -1 at the root */
    uint32_t depth;
    int32_t in_use;
    siaara_status file_status;     /* OK, SIAARA_ERR_REJECTED (unsafe File ID) or _NOT_FOUND */
    uint64_t offset;               /* file offset of the record */
} siaara_dicomdir_entry;

/* keys holds the record's keys plus those inherited from its ancestors; SIAARA_IDX_RECORD_TYPE,
 * and SIAARA_IDX_FILE_PATH = resolved absolute path of the referenced file (if any).
 * Return 0 to continue, non-zero to stop (SIAARA_ERR_CANCELLED). */
typedef int32_t (SIAARA_CALL* siaara_dicomdir_callback)(void* user, const siaara_dicomdir_entry* entry,
                                                        const siaara_index_record* keys, const char* strings);

typedef struct siaara_dicomdir_read_options {
    uint32_t struct_size;          /* set to sizeof(siaara_dicomdir_read_options) */
    int32_t include_inactive;      /* 0/1 */
    int32_t resolve_files;         /* 0/1, default 1 */
    int32_t case_insensitive;      /* 0/1, default 1 */
    uint32_t max_depth;            /* 0 = default (16) */
} siaara_dicomdir_read_options;

typedef struct siaara_dicomdir_summary {
    uint32_t struct_size;          /* set to sizeof(siaara_dicomdir_summary) */
    uint32_t records;
    uint32_t reported;
    uint32_t files;
    uint32_t missing_files;
    uint32_t rejected_paths;
    uint32_t unreachable;
    int32_t sequential_fallback;
    uint32_t warnings;
} siaara_dicomdir_summary;

/* options and summary may be NULL. */
SIAARA_API siaara_status SIAARA_CALL siaara_dicomdir_read(const char* utf8_path, size_t path_len,
                                                          const siaara_dicomdir_read_options* options,
                                                          siaara_dicomdir_callback cb, void* user,
                                                          siaara_dicomdir_summary* summary);

enum { SIAARA_PROFILE_GENERAL_CD = 0, SIAARA_PROFILE_GENERAL_USB = 1 };

typedef struct siaara_dicomdir_write_options {
    uint32_t struct_size;          /* set to sizeof(siaara_dicomdir_write_options) */
    int32_t profile;               /* SIAARA_PROFILE_* */
    int32_t overwrite;             /* 0/1 */
    int32_t allow_any_transfer_syntax; /* 0/1 */
    const char* file_set_id;       /* NULL or up to 16 of A-Z 0-9 _ space */
} siaara_dicomdir_write_options;

/* Copies the files to <root>/DICOM/... and writes <root>/DICOMDIR. files[i] has length lens[i].
 * options and warnings may be NULL; *warnings receives the number of warnings. */
SIAARA_API siaara_status SIAARA_CALL siaara_dicomdir_write(const char* utf8_root, size_t root_len,
                                                           const char* const* files, const size_t* lens,
                                                           size_t count,
                                                           const siaara_dicomdir_write_options* options,
                                                           uint32_t* warnings);

/* ---------------------------------------------------------------- de-identification (PS3.15 Annex E) */
/* Options of the Basic Application Level Confidentiality Profile (Table E.1-1 option columns). */
enum {
    SIAARA_DEID_RETAIN_SAFE_PRIVATE = 1u << 0,
    SIAARA_DEID_RETAIN_UIDS = 1u << 1,
    SIAARA_DEID_RETAIN_DEVICE_IDENTITY = 1u << 2,
    SIAARA_DEID_RETAIN_INSTITUTION_IDENTITY = 1u << 3,
    SIAARA_DEID_RETAIN_PATIENT_CHARACTERISTICS = 1u << 4,
    SIAARA_DEID_RETAIN_LONG_FULL_DATES = 1u << 5,
    SIAARA_DEID_RETAIN_LONG_MODIFIED_DATES = 1u << 6,
    SIAARA_DEID_CLEAN_DESCRIPTORS = 1u << 7,
    SIAARA_DEID_CLEAN_STRUCTURED_CONTENT = 1u << 8,
    SIAARA_DEID_CLEAN_GRAPHICS = 1u << 9
};
/* How X/Z, X/D, X/Z/D, Z/D and X/Z/U* are resolved (the attribute's Type is not known here). */
enum {
    SIAARA_DEID_COMBINED_CONFORMANT = 0, /* D, Z or U: never breaks a Type 1/2 requirement (default) */
    SIAARA_DEID_COMBINED_REMOVE = 1      /* X wherever allowed */
};
/* Result flags written to *out_flags by siaara_deidentify. */
enum {
    SIAARA_DEID_BURNED_IN_ANNOTATION_REMAINS = 1u << 0, /* pixel data, (0028,0301) YES or absent, not cleaned */
    SIAARA_DEID_RECOGNIZABLE_FEATURES_REMAIN = 1u << 1, /* (0028,0302) YES */
    SIAARA_DEID_NOT_FULLY_DEIDENTIFIED = 1u << 2,       /* either of the two above */
    SIAARA_DEID_PIXEL_DATA_CLEANED = 1u << 3,           /* regions were blanked */
    SIAARA_DEID_UNPARSEABLE_VALUES_REMOVED = 1u << 4    /* invalid dates/numbers were emptied */
};
#define SIAARA_DEID_ALL_FRAMES 0xFFFFFFFFu

/* A rectangle to blank in native Pixel Data (Clean Pixel Data Option). frame is 0-based or
 * SIAARA_DEID_ALL_FRAMES; x/y are the column/row of the top-left pixel. */
typedef struct siaara_deid_region {
    uint32_t frame;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} siaara_deid_region;

typedef struct siaara_deid_options {
    uint32_t struct_size;               /* set to sizeof(siaara_deid_options) */
    uint32_t flags;                     /* SIAARA_DEID_RETAIN_* / SIAARA_DEID_CLEAN_* */
    const uint8_t* key;                 /* secret key, at least 16 bytes; not retained after the call */
    size_t key_len;
    int32_t combined_policy;            /* SIAARA_DEID_COMBINED_* */
    int32_t pixel_data_clean;           /* 0/1: caller asserts the regions cover all burned-in text */
    const siaara_deid_region* regions;  /* may be NULL */
    size_t region_count;
    const char* safe_words;             /* words kept by the clean action, separated by spaces or
                                           line breaks (ASCII); may be NULL */
    size_t safe_words_len;
    uint32_t max_shift_days;            /* Modified Dates offset range; 0 = default (3652) */
} siaara_deid_options;

/* De-identifies the file in place (data set, file meta, preamble). On failure the file is
 * unchanged. out_flags (may be NULL) receives SIAARA_DEID_* result flags; check
 * SIAARA_DEID_NOT_FULLY_DEIDENTIFIED before releasing the result. Messages never contain values
 * from the data set. */
SIAARA_API siaara_status SIAARA_CALL siaara_deidentify(siaara_file* file, const siaara_deid_options* options,
                                                       uint32_t* out_flags);
/* The replacement siaara_deidentify uses for a UID with this key (string buffer rules). */
SIAARA_API siaara_status SIAARA_CALL siaara_deid_replacement_uid(const uint8_t* key, size_t key_len, const char* uid,
                                                                 size_t uid_len, char* buf, size_t cap, size_t* needed);

/* ---------------------------------------------------------------- TLS (PS3.15 B.12 / B.13) */
/* TLS uses the operating system's own implementation: SChannel on Windows, Network.framework on
 * macOS, the system OpenSSL 3 (loaded at run time) on Linux. TLS 1.2 minimum, 1.3 preferred,
 * ECDHE + AEAD cipher suites only, certificate validation on by default, never a fallback to
 * plain TCP. https:// base URLs are also accepted by siaara_web_client_create (OS trust store,
 * no client certificate).
 *
 * Returns 1 when TLS can be used in this process, else 0 with the reason in
 * siaara_last_error_message() (e.g. the system OpenSSL 3 library was not found). */
SIAARA_API int32_t SIAARA_CALL siaara_tls_available(void);

typedef struct siaara_tls_options {
    uint32_t struct_size;              /* sizeof(siaara_tls_options) */
    const char* ca_file;               /* PEM trust anchors; NULL: the OS trust store */
    const char* certificate_file;      /* client certificate chain, PEM (Linux/OpenSSL only) */
    const char* private_key_file;      /* its private key, PEM (Linux/OpenSSL only) */
    const char* pkcs12_file;           /* client identity as PKCS#12 (every OS) */
    const char* password;              /* PKCS#12 / encrypted key password; copied, wiped when freed */
    const char* os_identity;           /* Windows: certificate thumbprint in CurrentUser\MY;
                                          macOS: keychain identity label */
    const char* expected_identity;     /* name or IP the server certificate must carry; NULL: URL host */
    int32_t insecure_skip_validation;  /* nonzero: DANGEROUS, no certificate checks (warned per connection) */
    int32_t tls13_only;                /* nonzero: refuse TLS 1.2 */
} siaara_tls_options;

/* As siaara_web_client_create, for an https:// base_url with TLS options (options may be NULL).
 * All strings are NUL-terminated and copied. Caller owns *out. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_client_create_tls(const char* base_url, size_t len, int32_t timeout_ms,
                                                                  const siaara_tls_options* options,
                                                                  siaara_web_client** out);

/* ---------------------------------------------------------------- clinical objects (appended) */
/* Plane geometry, orientation, series sorting, Structured Reports, DICOM SEG and RT Structure
 * Sets (src/clinical). Image coordinates (e.g. RT contours mapped to an image) are PIXEL annotation
 * units (PS3.3 C.10.5.1.2): x = column, y = row, (0,0) = top left corner of the first pixel.
 * Geometry positions use pixel indices with pixel (0,0) centred on Image Position (Patient). */

enum {
    SIAARA_SPACING_NONE = 0,
    SIAARA_SPACING_PIXEL_SPACING = 1,          /* Pixel Spacing or Pixel Measures functional group */
    SIAARA_SPACING_IMAGER = 2,                 /* Imager Pixel Spacing at the detector */
    SIAARA_SPACING_IMAGER_MAGNIFIED = 3,       /* Imager Pixel Spacing / Estimated Radiographic Magnification Factor */
    SIAARA_SPACING_NOMINAL_SCANNED = 4,        /* Nominal Scanned Pixel Spacing */
    SIAARA_SPACING_ULTRASOUND_REGION = 5       /* Sequence of Ultrasound Regions physical deltas */
};

typedef struct siaara_plane_geometry {
    uint32_t struct_size;            /* set to sizeof(siaara_plane_geometry) */
    uint32_t rows;
    uint32_t columns;
    int32_t has_position;            /* 0/1 */
    int32_t has_orientation;         /* 0/1 */
    int32_t spacing_source;          /* SIAARA_SPACING_* */
    int32_t spacing_at_detector;     /* 0/1: spacing applies at the detector plane */
    int32_t reserved0;
    double position[3];              /* mm, LPS */
    double row_cosine[3];
    double column_cosine[3];
    double normal[3];
    double pixel_spacing[2];         /* row spacing, column spacing (mm); 0 when unknown */
    double slice_thickness;          /* 0 when absent */
    double spacing_between_slices;   /* 0 when absent */
    double magnification;            /* magnification factor applied to the spacing (1 = none) */
} siaara_plane_geometry;

/* Geometry of frame `frame` (0-based): functional groups first, then the classic attributes. */
SIAARA_API siaara_status SIAARA_CALL siaara_frame_geometry(const siaara_file* file, uint32_t frame,
                                                           siaara_plane_geometry* out);

typedef struct siaara_edge_labels {
    uint32_t struct_size;            /* set to sizeof(siaara_edge_labels) */
    char left[4];                    /* e.g. "R", "RA", "RAF"; NUL-terminated */
    char right[4];
    char top[4];
    char bottom[4];
} siaara_edge_labels;

/* Patient orientation letters at the image edges. SIAARA_ERR_NOT_FOUND without orientation. */
SIAARA_API siaara_status SIAARA_CALL siaara_orientation_labels(const siaara_file* file, uint32_t frame,
                                                               siaara_edge_labels* out);

/* Series sort warnings (bit mask). */
enum {
    SIAARA_SORT_MISSING_GEOMETRY = 1 << 0,
    SIAARA_SORT_MULTIPLE_STACKS = 1 << 1,
    SIAARA_SORT_MIXED_ORIENTATION = 1 << 2,
    SIAARA_SORT_DUPLICATE_POSITIONS = 1 << 3,
    SIAARA_SORT_UNEVEN_SPACING = 1 << 4,
    SIAARA_SORT_GANTRY_TILT = 1 << 5,
    SIAARA_SORT_MULTIPLE_FRAMES_OF_REFERENCE = 1 << 6,
    SIAARA_SORT_TEMPORAL_SPLIT = 1 << 7,
    SIAARA_SORT_SIZE_MISMATCH = 1 << 8
};
#define SIAARA_SORT_EXPAND_FRAMES 1u   /* flag: one entry per frame of enhanced multi-frame files */

/* Sorts files (NULL entries are skipped) along the slice normal, then Instance Number and
 * acquisition time, after splitting by stack, temporal position and orientation. Writes *needed
 * entries: out_instance[k] = index into files, out_frame[k] (may be NULL) = 0-based frame,
 * out_group[k] (may be NULL) = stack / temporal group number. *warnings (may be NULL) receives
 * SIAARA_SORT_* bits. cap < *needed: SIAARA_ERR_BUFFER_TOO_SMALL (warnings still set). */
SIAARA_API siaara_status SIAARA_CALL siaara_series_sort(const siaara_file* const* files, size_t count, uint32_t flags,
                                                        uint32_t* out_instance, uint32_t* out_frame,
                                                        uint32_t* out_group, size_t cap, size_t* needed,
                                                        uint32_t* warnings);

/* ---- Structured Reports */
typedef struct siaara_sr siaara_sr;

enum {
    SIAARA_SR_CONTAINER = 0, SIAARA_SR_TEXT = 1, SIAARA_SR_CODE = 2, SIAARA_SR_NUM = 3, SIAARA_SR_DATETIME = 4,
    SIAARA_SR_DATE = 5, SIAARA_SR_TIME = 6, SIAARA_SR_UIDREF = 7, SIAARA_SR_PNAME = 8, SIAARA_SR_COMPOSITE = 9,
    SIAARA_SR_IMAGE = 10, SIAARA_SR_WAVEFORM = 11, SIAARA_SR_SCOORD = 12, SIAARA_SR_SCOORD3D = 13,
    SIAARA_SR_TCOORD = 14, SIAARA_SR_TABLE = 15, SIAARA_SR_REFERENCE = 16, SIAARA_SR_UNKNOWN = 17
};
enum {
    SIAARA_REL_NONE = 0, SIAARA_REL_CONTAINS = 1, SIAARA_REL_HAS_PROPERTIES = 2, SIAARA_REL_HAS_OBS_CONTEXT = 3,
    SIAARA_REL_HAS_ACQ_CONTEXT = 4, SIAARA_REL_INFERRED_FROM = 5, SIAARA_REL_SELECTED_FROM = 6,
    SIAARA_REL_HAS_CONCEPT_MOD = 7, SIAARA_REL_UNKNOWN = 8
};

typedef struct siaara_sr_node_info {
    uint32_t struct_size;            /* set to sizeof(siaara_sr_node_info) */
    int32_t parent;                  /* -1 for the root */
    uint32_t depth;
    uint32_t child_count;
    int32_t value_type;              /* SIAARA_SR_* */
    int32_t relationship;            /* SIAARA_REL_* */
    int32_t referenced_node;         /* SIAARA_SR_REFERENCE: target node, -1 if unresolved */
    int32_t has_numeric;
    double numeric;                  /* NUM value */
    uint32_t reference_count;        /* COMPOSITE / IMAGE / WAVEFORM references */
    uint32_t graphic_point_values;   /* SCOORD / SCOORD3D: number of Graphic Data values */
} siaara_sr_node_info;

enum {
    SIAARA_SRF_CONCEPT_VALUE = 0, SIAARA_SRF_CONCEPT_SCHEME = 1, SIAARA_SRF_CONCEPT_MEANING = 2,
    SIAARA_SRF_TEXT = 3,             /* TEXT, DATETIME, DATE, TIME, UIDREF, PNAME value */
    SIAARA_SRF_CODE_VALUE = 4, SIAARA_SRF_CODE_SCHEME = 5, SIAARA_SRF_CODE_MEANING = 6,
    SIAARA_SRF_NUMERIC_TEXT = 7, SIAARA_SRF_UNITS_VALUE = 8, SIAARA_SRF_UNITS_SCHEME = 9, SIAARA_SRF_UNITS_MEANING = 10,
    SIAARA_SRF_REFERENCED_SOP_CLASS = 11, SIAARA_SRF_REFERENCED_SOP_INSTANCE = 12, /* first reference */
    SIAARA_SRF_GRAPHIC_TYPE = 13, SIAARA_SRF_VALUE_TYPE = 14, SIAARA_SRF_RELATIONSHIP = 15,
    SIAARA_SRF_CONTINUITY = 16, SIAARA_SRF_TEMPLATE_ID = 17, SIAARA_SRF_POSITION = 18 /* "1.2.3" */
};

/* Reads the SR content tree of the file. Caller owns *out: free with siaara_sr_free. */
SIAARA_API siaara_status SIAARA_CALL siaara_sr_open(const siaara_file* file, siaara_sr** out);
SIAARA_API void SIAARA_CALL siaara_sr_free(siaara_sr* sr) SIAARA_NOEXCEPT;
SIAARA_API siaara_status SIAARA_CALL siaara_sr_node_count(const siaara_sr* sr, uint32_t* out);
SIAARA_API siaara_status SIAARA_CALL siaara_sr_node(const siaara_sr* sr, uint32_t index, siaara_sr_node_info* out);
/* Child node indices (count elements). Buffer rules as for siaara_series_sort. */
SIAARA_API siaara_status SIAARA_CALL siaara_sr_node_children(const siaara_sr* sr, uint32_t index, uint32_t* buf,
                                                             size_t cap, size_t* needed);
/* A text field (SIAARA_SRF_*) of a node; string buffer rules. */
SIAARA_API siaara_status SIAARA_CALL siaara_sr_node_string(const siaara_sr* sr, uint32_t index, int32_t field,
                                                           char* buf, size_t cap, size_t* needed);
/* SCOORD / SCOORD3D Graphic Data values (count doubles). */
SIAARA_API siaara_status SIAARA_CALL siaara_sr_node_graphic_data(const siaara_sr* sr, uint32_t index, double* buf,
                                                                 size_t cap, size_t* needed);
/* Plain-text rendering of the whole tree; string buffer rules. */
SIAARA_API siaara_status SIAARA_CALL siaara_sr_to_text(const siaara_sr* sr, char* buf, size_t cap, size_t* needed);

/* ---- Segmentation (DICOM SEG) */
typedef struct siaara_seg siaara_seg;

enum { SIAARA_SEG_BINARY = 0, SIAARA_SEG_FRACTIONAL = 1, SIAARA_SEG_LABELMAP = 2 };

typedef struct siaara_segment_info {
    uint32_t struct_size;            /* set to sizeof(siaara_segment_info) */
    uint32_t number;                 /* Segment Number */
    uint32_t frame_count;            /* SEG frames holding the segment */
    int32_t has_color;
    uint8_t rgb[3];                  /* from Recommended Display CIELab Value */
    uint8_t reserved0;
    uint16_t cielab[3];
    uint16_t reserved1;
} siaara_segment_info;

/* Reads the segmentation description of the file. Caller owns *out: free with siaara_seg_free. */
SIAARA_API siaara_status SIAARA_CALL siaara_seg_open(const siaara_file* file, siaara_seg** out);
SIAARA_API void SIAARA_CALL siaara_seg_free(siaara_seg* seg) SIAARA_NOEXCEPT;
/* *type receives SIAARA_SEG_*, *frames the Number of Frames (both may be NULL). */
SIAARA_API siaara_status SIAARA_CALL siaara_seg_info(const siaara_seg* seg, int32_t* type, uint32_t* frames,
                                                     uint32_t* max_fractional);
SIAARA_API siaara_status SIAARA_CALL siaara_seg_segment_count(const siaara_seg* seg, uint32_t* out);
SIAARA_API siaara_status SIAARA_CALL siaara_seg_segment(const siaara_seg* seg, uint32_t index, siaara_segment_info* out);
SIAARA_API siaara_status SIAARA_CALL siaara_seg_segment_label(const siaara_seg* seg, uint32_t index, char* buf,
                                                              size_t cap, size_t* needed);
/* SEG frames (0-based) of segment `segment_number` derived from the source instance and 1-based
 * frame (0 = any). Array buffer rules. */
SIAARA_API siaara_status SIAARA_CALL siaara_seg_frames_for_source(const siaara_seg* seg, uint32_t segment_number,
                                                                  const char* sop_instance_uid, size_t len,
                                                                  uint32_t frame, uint32_t* buf, size_t cap,
                                                                  size_t* needed);
/* Mask of SEG frame `frame` (rows x columns bytes): BINARY 0/1, FRACTIONAL the stored value,
 * LABELMAP 1 where the label is segment_number. `file` must be the file the seg was opened from. */
SIAARA_API siaara_status SIAARA_CALL siaara_seg_mask(const siaara_file* file, const siaara_seg* seg, uint32_t frame,
                                                     uint32_t segment_number, uint8_t* buf, size_t cap,
                                                     size_t* needed);

/* ---- RT Structure Set */
typedef struct siaara_rtstruct siaara_rtstruct;

typedef struct siaara_rt_roi_info {
    uint32_t struct_size;            /* set to sizeof(siaara_rt_roi_info) */
    int32_t number;                  /* ROI Number */
    uint32_t contour_count;
    int32_t has_color;
    uint8_t rgb[3];                  /* ROI Display Color */
    uint8_t reserved0;
} siaara_rt_roi_info;

SIAARA_API siaara_status SIAARA_CALL siaara_rtstruct_open(const siaara_file* file, siaara_rtstruct** out);
SIAARA_API void SIAARA_CALL siaara_rtstruct_free(siaara_rtstruct* rt) SIAARA_NOEXCEPT;
SIAARA_API siaara_status SIAARA_CALL siaara_rtstruct_roi_count(const siaara_rtstruct* rt, uint32_t* out);
SIAARA_API siaara_status SIAARA_CALL siaara_rtstruct_roi(const siaara_rtstruct* rt, uint32_t index, siaara_rt_roi_info* out);
SIAARA_API siaara_status SIAARA_CALL siaara_rtstruct_roi_name(const siaara_rtstruct* rt, uint32_t index, char* buf,
                                                              size_t cap, size_t* needed);
/* Contour points as x y z triplets in patient coordinates (mm): *needed = 3 x points doubles.
 * *closed (may be NULL) = 1 for CLOSED_PLANAR. */
SIAARA_API siaara_status SIAARA_CALL siaara_rtstruct_contour_points(const siaara_rtstruct* rt, uint32_t roi,
                                                                    uint32_t contour, double* buf, size_t cap,
                                                                    size_t* needed, int32_t* closed);

/* ---- RT Dose (PS3.3 C.8.8.3)
 * For an RT Dose object every modality value the library produces (SIAARA_FRAME_FLOAT32_MODALITY,
 * volumes, fusion, siaara_frame_properties.rescale_slope) is the stored value
 * times Dose Grid Scaling (3004,000E), i.e. a dose in Dose Units (normally GY). Multi-frame RT
 * Dose volumes place the frames with the Grid Frame Offset Vector (3004,000C), relative (first
 * value 0) or absolute (first value = Image Position (Patient) z, transverse planes only). */
enum { SIAARA_GFOV_NONE = 0, SIAARA_GFOV_RELATIVE = 1, SIAARA_GFOV_ABSOLUTE = 2 };
typedef struct siaara_rt_dose_info {
    uint32_t struct_size;            /* set to sizeof(siaara_rt_dose_info) */
    int32_t is_rt_dose;              /* SOP Class RT Dose or Modality RTDOSE */
    int32_t has_dose_grid_scaling;   /* 0/1: (3004,000E) present and a positive number */
    int32_t grid_frame_offset_form;  /* SIAARA_GFOV_* */
    double dose_grid_scaling;        /* 1 when absent */
    uint32_t grid_frame_offset_count; /* values in (3004,000C) */
    uint32_t reserved0;
    char dose_units[20];             /* (3004,0002) GY / RELATIVE, NUL-terminated */
    char dose_type[20];              /* (3004,0004) PHYSICAL / EFFECTIVE / ERROR */
    char dose_summation_type[20];    /* (3004,000A) PLAN / MULTI_PLAN / FRACTION / BEAM / ... */
    char reserved1[4];
} siaara_rt_dose_info;
/* Fills *out from a data set (struct_size must be set; a smaller struct_size gets the fields that
 * fit). Works for any data set (is_rt_dose = 0 when it is not RT Dose). Thread safety: as reading. */
SIAARA_API siaara_status SIAARA_CALL siaara_rt_dose_info_get(const siaara_dataset* ds, siaara_rt_dose_info* out);

/* ---------------------------------------------------------------- transcoding */
#define SIAARA_OFFSET_TABLE_AUTO 0     /* Basic Offset Table, Extended when offsets exceed 32 bits */
#define SIAARA_OFFSET_TABLE_BASIC 1
#define SIAARA_OFFSET_TABLE_EXTENDED 2
#define SIAARA_OFFSET_TABLE_EMPTY 3

typedef struct siaara_transcode_params {
    uint32_t struct_size;        /* set to sizeof(siaara_transcode_params) */
    int32_t quality;             /* lossy JPEG / JPEG 2000: 1..100; 0 = default (90) */
    int32_t lossy;               /* 0/1: code lossily in syntaxes that allow both (.81, .91, .203) */
    int32_t near_lossless;       /* JPEG-LS NEAR for .81 when lossy; 0 = default (2) */
    int32_t allow_lossy;         /* 0/1: 0 refuses any transcode that applies lossy compression */
    int32_t predictor;           /* JPEG lossless .57 selection value 1..7; 0 = default (1) */
    int32_t chroma_subsampling;  /* JPEG lossy colour: 0 = 4:4:4 (YBR_FULL), 1 = 4:2:2 (YBR_FULL_422) */
    int32_t offset_table;        /* SIAARA_OFFSET_TABLE_* */
    int32_t keep_uid_on_lossy;   /* 0 (default): a transcode that applies lossy compression makes a
                                    new image (PS3.3 C.7.6.1.1.5): new SOP Instance UID (data set and
                                    meta), Image Type value 1 DERIVED, the predecessor in Source Image
                                    Sequence (CID 7202 121320 / 121330), Derivation Code Sequence
                                    (113040) and Derivation Description. 1: keep the UID and those
                                    attributes. Lossy Image Compression, Ratio and Method are always
                                    recorded (history appended). Needs struct_size to include it. */
} siaara_transcode_params;

/* Converts the file's pixel data to the transfer syntax ts_uid (NUL-terminated) and updates the
 * Image Pixel and Lossy Image Compression attributes and the file meta. params may be NULL
 * (lossless where the syntax allows, lossy allowed for lossy-only syntaxes such as JPEG baseline).
 * On failure the file is unchanged; SIAARA_ERR_UNSUPPORTED when the image cannot be
 * represented, SIAARA_ERR_REJECTED when lossy coding is needed and allow_lossy is 0. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_transcode(siaara_file* file, const char* ts_uid,
                                                           const siaara_transcode_params* params);

/* What a lossy transcode did with the history lists Lossy Image Compression Ratio (0028,2112) and
 * Method (0028,2114), which hold one value per lossy step in the same order (PS3.3
 * C.7.6.1.1.5.1). Nothing is guessed: a missing earlier Method value is filled in only when the
 * source's own coding proves it. */
#define SIAARA_LOSSY_HISTORY_NOT_APPLICABLE 0 /* no lossy compression applied: both untouched */
#define SIAARA_LOSSY_HISTORY_ALIGNED 1        /* the source had as many Ratio as Method values (or
                                                 neither): one value appended to each */
#define SIAARA_LOSSY_HISTORY_METHOD_FILLED 2  /* LOSSY_METHOD_FILLED: the source had one more Ratio
                                                 than Method value and its pixel data was lossy
                                                 coded in JPEG baseline/extended (.50/.51:
                                                 ISO_10918_1), JPEG-LS near-lossless (.81:
                                                 ISO_14495_1), JPEG 2000 (.91: ISO_15444_1) or
                                                 HTJ2K (.203: ISO_15444_15); that Method value
                                                 was filled in (lossy_method_filled), then one
                                                 value appended to each */
#define SIAARA_LOSSY_HISTORY_MISALIGNED 3     /* LOSSY_HISTORY_MISALIGNED: any other mismatch (source
                                                 not lossy coded, a gap of 2 or more, more Method
                                                 than Ratio values): the Ratio was appended, Method
                                                 was left exactly as it was */

typedef struct siaara_transcode_report {
    uint32_t struct_size;         /* set to sizeof(siaara_transcode_report) */
    int32_t pixel_data_recoded;   /* 0/1: frames were decoded and encoded again */
    int32_t lossy_applied;        /* 0/1: this transcode applied lossy compression */
    int32_t source_lossy;         /* 0/1: the source was lossy already (attribute or codestream) */
    int32_t lossy_history;        /* SIAARA_LOSSY_HISTORY_* */
    uint32_t reserved0;
    uint64_t encoded_bytes;       /* total encoded frame bytes (encapsulated targets) */
    uint64_t native_bytes;        /* total decoded frame bytes */
    char new_sop_instance_uid[65]; /* the new UID a lossy transcode gave the image, else "" */
    char lossy_method_filled[20]; /* METHOD_FILLED: the defined term written for the earlier step */
    char lossy_history_note[419]; /* METHOD_FILLED / MISALIGNED: "LOSSY_METHOD_FILLED: ..." or
                                     "LOSSY_HISTORY_MISALIGNED: ..." with the value counts found, what
                                     was written and why (NUL-terminated, cut when longer); else "" */
} siaara_transcode_report;

/* As siaara_file_transcode, filling *report (may be NULL; struct_size must be set, a smaller
 * struct_size gets the fields that fit). On failure *report is zeroed except struct_size. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_transcode_ex(siaara_file* file, const char* ts_uid,
                                                              const siaara_transcode_params* params,
                                                              siaara_transcode_report* report);

/* ---------------------------------------------------------------- volumes and 3D views */
/* A volume is a dense voxel grid built from the instances of one series (or one enhanced
 * multi-frame object), sorted along the slice normal, with the Modality LUT / Rescale applied per
 * frame. Voxel (i, j, k) - i fastest in memory - is at origin + i*spacing[0]*axis_i +
 * j*spacing[1]*axis_j + k*spacing[2]*axis_k (patient LPS coordinates, mm). A volume is immutable:
 * it may be used from many threads, and by several views at once. */
typedef struct siaara_volume siaara_volume;
typedef struct siaara_view siaara_view;

enum {
    SIAARA_VOXEL_AUTO = 0,    /* int16 when every modality value is an integer that fits, else float32 */
    SIAARA_VOXEL_INT16 = 1,
    SIAARA_VOXEL_FLOAT32 = 2
};

/* Volume flags (siaara_volume_properties.flags). */
enum {
    SIAARA_VOLUME_NON_UNIFORM_SPACING = 1 << 0,
    SIAARA_VOLUME_MISSING_SLICES = 1 << 1,
    SIAARA_VOLUME_DUPLICATE_POSITIONS = 1 << 2,
    SIAARA_VOLUME_GANTRY_TILT = 1 << 3,
    SIAARA_VOLUME_DESHEARED = 1 << 4,
    SIAARA_VOLUME_MULTI_PHASE = 1 << 5,
    SIAARA_VOLUME_SYNTHETIC_GEOMETRY = 1 << 6,
    SIAARA_VOLUME_SINGLE_SLICE = 1 << 7,
    SIAARA_VOLUME_VALUES_CLAMPED = 1 << 8,
    SIAARA_VOLUME_SPACING_FROM_HEADER = 1 << 9
};

typedef struct siaara_volume_options {
    uint32_t struct_size;        /* sizeof(siaara_volume_options) */
    int32_t voxel_type;          /* SIAARA_VOXEL_* */
    uint32_t phase;              /* phase of a 4D series, 0-based */
    uint32_t threads;            /* 0 = hardware concurrency */
    int32_t deshear_tilt;        /* nonzero: resample a gantry-tilted stack onto an orthogonal grid */
    int32_t strict;              /* nonzero: fail on non-uniform spacing, missing slices, duplicates */
    uint64_t memory_budget;      /* voxel buffer limit in bytes; 0 = 4 GiB */
    double spacing_tolerance;    /* relative; 0 = 0.01 */
} siaara_volume_options;

typedef struct siaara_volume_properties {
    uint32_t struct_size;        /* sizeof(siaara_volume_properties) */
    uint32_t dims[3];
    double spacing[3];           /* mm */
    double origin[3];            /* centre of voxel (0,0,0) */
    double direction[9];         /* row-major 3x3; column c is the unit patient direction of index axis c */
    int32_t voxel_type;          /* SIAARA_VOXEL_INT16 or SIAARA_VOXEL_FLOAT32 */
    uint32_t flags;              /* SIAARA_VOLUME_* */
    double value_min, value_max;
    uint32_t phase_count, phase_index;
    double tilt_degrees;
    uint32_t warning_count;
    uint32_t histogram_bins;     /* 256 */
    double histogram_min, histogram_max;
    uint64_t histogram[256];     /* equal-width bins over [histogram_min, histogram_max] */
    uint64_t data_bytes;
} siaara_volume_properties;

/* Builds a volume from `count` files of one series. path_lens may be NULL (paths NUL-terminated).
 * options may be NULL. Caller owns *out: free with siaara_volume_free. */
SIAARA_API siaara_status SIAARA_CALL siaara_volume_build(const char* const* utf8_paths, const size_t* path_lens,
                                                         uint32_t count, const siaara_volume_options* options,
                                                         siaara_volume** out);
SIAARA_API siaara_status SIAARA_CALL siaara_volume_info(const siaara_volume* volume, siaara_volume_properties* out);
/* Warning `index` (< warning_count) as text (string buffer rules). */
SIAARA_API siaara_status SIAARA_CALL siaara_volume_warning(const siaara_volume* volume, uint32_t index, char* buf,
                                                           size_t cap, size_t* needed);
/* Borrowed voxel buffer (int16_t or float, as voxel_type says); valid until the volume is freed. */
SIAARA_API siaara_status SIAARA_CALL siaara_volume_data(const siaara_volume* volume, const void** out_data,
                                                        uint64_t* out_bytes);
SIAARA_API void SIAARA_CALL siaara_volume_free(siaara_volume* volume) SIAARA_NOEXCEPT;

enum {
    SIAARA_VIEW_MPR = 0,
    SIAARA_VIEW_CURVED_STRAIGHTENED = 1,
    SIAARA_VIEW_CURVED_STRETCHED = 2,
    SIAARA_VIEW_VOLUME_RENDER = 3,
    SIAARA_VIEW_VOLUME_MIP = 4,
    SIAARA_VIEW_VOLUME_MINIP = 5
};
enum {
    SIAARA_INTERP_NEAREST = 0,
    SIAARA_INTERP_LINEAR = 1,
    SIAARA_INTERP_CUBIC = 2,          /* Catmull-Rom (interpolating) */
    SIAARA_INTERP_CUBIC_BSPLINE = 3   /* cubic B-spline without prefilter (smoothing) */
};
enum { SIAARA_SLAB_NONE = 0, SIAARA_SLAB_MIP = 1, SIAARA_SLAB_MINIP = 2, SIAARA_SLAB_AVERAGE = 3 };
enum { SIAARA_QUALITY_INTERACTIVE = 0, SIAARA_QUALITY_FINAL = 1 };
enum {
    SIAARA_COLORMAP_GRAY = 0,
    SIAARA_COLORMAP_INVERSE_GRAY = 1,
    SIAARA_COLORMAP_HOT_IRON = 2,      /* PS3.6 Annex B standard palettes */
    SIAARA_COLORMAP_PET = 3,
    SIAARA_COLORMAP_HOT_METAL_BLUE = 4,
    SIAARA_COLORMAP_PET_20_STEP = 5
};

/* Creates a view of `volume` (which the view keeps alive) with an output of width x height
 * pixels: an MPR through the middle slice, full-range window, fitted. Caller owns *out. A view
 * must not be changed while it renders; one view may render from one thread at a time. */
SIAARA_API siaara_status SIAARA_CALL siaara_view_create(const siaara_volume* volume, uint32_t width, uint32_t height,
                                                        siaara_view** out);
SIAARA_API void SIAARA_CALL siaara_view_free(siaara_view* view) SIAARA_NOEXCEPT;
/* Plane: centre point, viewing direction (into the screen) and up vector (patient mm, LPS);
 * screen right = normal x up. pixel_spacing in mm per pixel, 0 = fit the volume. */
SIAARA_API siaara_status SIAARA_CALL siaara_view_set_plane(siaara_view* view, const double point[3],
                                                           const double normal[3], const double up[3],
                                                           double pixel_spacing);
/* mode: SIAARA_VIEW_*; interpolation: SIAARA_INTERP_*; slab: SIAARA_SLAB_* with slab_thickness mm
 * (MPR only); quality: SIAARA_QUALITY_* (volume rendering). */
SIAARA_API siaara_status SIAARA_CALL siaara_view_set_mode(siaara_view* view, int32_t mode, int32_t interpolation,
                                                          int32_t slab, double slab_thickness, int32_t quality);
/* layer 0 = base, 1 = fusion overlay. width 0 = the layer's full value range. */
SIAARA_API siaara_status SIAARA_CALL siaara_view_set_window(siaara_view* view, int32_t layer, double center,
                                                            double width);
SIAARA_API siaara_status SIAARA_CALL siaara_view_set_colormap(siaara_view* view, int32_t layer, int32_t colormap);
/* Path for the curved modes: `count` (>= 2) points of 3 doubles; spline nonzero = Catmull-Rom spline. */
SIAARA_API siaara_status SIAARA_CALL siaara_view_set_curve(siaara_view* view, const double* points, uint32_t count,
                                                           int32_t spline);

typedef struct siaara_transfer_function {
    uint32_t struct_size;               /* sizeof(siaara_transfer_function) */
    const double* opacity_points;       /* opacity_count pairs (value, opacity 0..1), sorted by value */
    uint32_t opacity_count;
    const double* color_points;         /* color_count quadruples (value, r, g, b 0..1); 0: white */
    uint32_t color_count;
    const double* gradient_points;      /* gradient_count pairs (|gradient| per mm, factor 0..1); 0: off */
    uint32_t gradient_count;
    int32_t shading;                    /* nonzero: Phong shading (headlight) */
    double ambient, diffuse, specular, shininess;  /* all 0: defaults 0.25, 0.7, 0.25, 24 */
    double unit_distance;               /* mm the opacities refer to; 0 = 1 mm */
    double sample_distance;             /* mm; 0 = half the smallest voxel spacing */
} siaara_transfer_function;
SIAARA_API siaara_status SIAARA_CALL siaara_view_set_transfer_function(siaara_view* view,
                                                                       const siaara_transfer_function* tf);

typedef struct siaara_fusion_options {
    uint32_t struct_size;               /* sizeof(siaara_fusion_options) */
    int32_t colormap;                   /* SIAARA_COLORMAP_* */
    double window_center, window_width; /* width 0 = full range */
    double opacity;                     /* 0..1 */
    int32_t opaque_below_window;        /* 0 (default): values at or below the window are transparent */
    int32_t preresample;                /* nonzero: resample the overlay into the base grid once */
} siaara_fusion_options;
/* Sets (or with overlay == NULL removes) the fusion layer; the view keeps the overlay alive.
 * options may be NULL (PET colour map, full range, opacity 0.5). */
SIAARA_API siaara_status SIAARA_CALL siaara_view_set_fusion_layer(siaara_view* view, const siaara_volume* overlay,
                                                                  const siaara_fusion_options* options);
/* Renders width * height * 4 bytes of RGBA8 (buffer rules as for siaara_frame_decode). */
SIAARA_API siaara_status SIAARA_CALL siaara_view_render_rgba8(siaara_view* view, uint8_t* buf, size_t cap,
                                                              size_t* needed);

typedef struct siaara_pick_result {
    uint32_t struct_size;               /* sizeof(siaara_pick_result) */
    int32_t hit;                        /* the point maps into the scene (VR: a surface was hit) */
    int32_t inside;                     /* inside the base volume */
    double patient[3];                  /* mm, LPS */
    double index[3];                    /* continuous voxel index in the base volume */
    double value;                       /* base value */
    int32_t overlay_inside;
    double overlay_value;
} siaara_pick_result;
/* Screen point (pixel coordinates) -> patient point and values, for crosshair synchronisation. */
SIAARA_API siaara_status SIAARA_CALL siaara_view_pick(const siaara_view* view, double x, double y,
                                                      siaara_pick_result* out);

/* ---------------------------------------------------------------- GPU rendering of 3D views */
/* Views render on the CPU by default. With SIAARA_BACKEND_AUTO a view renders on a hardware GPU
 * (Vulkan on Windows / Linux, Metal on macOS; the OS graphics stack, loaded at run time) when one
 * is present and the view's mode is implemented there (MPR with nearest / trilinear sampling,
 * slabs, fusion, volume rendering, MIP / MinIP), and on the CPU otherwise. SIAARA_BACKEND_GPU
 * also accepts a software GPU device (e.g. Mesa lavapipe) and fails with SIAARA_ERR_UNSUPPORTED
 * when no device exists. Both fall back to the CPU for any frame the GPU cannot render. GPU
 * images match the CPU renderer within small tolerances (texture filtering and float precision). */
enum { SIAARA_GPU_API_VULKAN = 1, SIAARA_GPU_API_METAL = 2 };
enum {
    SIAARA_GPU_DEVICE_OTHER = 0,
    SIAARA_GPU_DEVICE_INTEGRATED = 1,
    SIAARA_GPU_DEVICE_DISCRETE = 2,
    SIAARA_GPU_DEVICE_VIRTUAL = 3,
    SIAARA_GPU_DEVICE_CPU = 4          /* a software implementation */
};
enum { SIAARA_BACKEND_AUTO = 0, SIAARA_BACKEND_CPU = 1, SIAARA_BACKEND_GPU = 2 };

typedef struct siaara_gpu_device_info {
    uint32_t struct_size;               /* sizeof(siaara_gpu_device_info), set by the caller */
    int32_t api;                        /* SIAARA_GPU_API_* */
    int32_t type;                       /* SIAARA_GPU_DEVICE_* */
    uint32_t vendor_id, device_id;      /* PCI ids where the API reports them */
    uint64_t memory_bytes;              /* device-local memory */
    uint32_t max_texture_3d;            /* largest 3D texture dimension */
    int32_t selected;                   /* nonzero: the device the toolkit renders with */
    int32_t float32_filtering;          /* hardware trilinear filtering of float32 volumes */
    int32_t int16_filtering;            /* hardware trilinear filtering of int16 volumes */
    char name[256];                     /* UTF-8, NUL-terminated */
    char driver[128];                   /* API / driver version text, NUL-terminated */
} siaara_gpu_device_info;
/* Lists the GPU devices (probed once per process). *count receives the number of devices; up to
 * cap entries are written to out (each entry's struct_size must be set). out may be NULL with
 * cap 0 to query the count. No device is not an error (count 0). */
SIAARA_API siaara_status SIAARA_CALL siaara_gpu_devices(siaara_gpu_device_info* out, uint32_t cap, uint32_t* count);
/* SIAARA_BACKEND_*: where siaara_view_render_rgba8 renders. */
SIAARA_API siaara_status SIAARA_CALL siaara_view_set_backend(siaara_view* view, int32_t backend);
/* SIAARA_BACKEND_CPU or SIAARA_BACKEND_GPU: the renderer of the last siaara_view_render_rgba8. */
SIAARA_API siaara_status SIAARA_CALL siaara_view_last_backend(const siaara_view* view, int32_t* out_backend);

/* ---------------------------------------------------------------- lenient frame decoding */
/* Adjustments siaara_frame_decode / siaara_render_rgba8 make (lenient reading, the default) when a
 * compressed frame's codestream header (JPEG SOF, JPEG-LS SOF55, JPEG 2000 SIZ) disagrees with the
 * Image Pixel module. The values are stable. Messages hold codestream / attribute values only. */
enum {
    SIAARA_FRAME_WARN_PRECISION_BELOW_BITS_STORED = 1,    /* values used as decoded */
    SIAARA_FRAME_WARN_PRECISION_ABOVE_BITS_STORED = 2,    /* values kept; pixel info bits_stored = precision */
    SIAARA_FRAME_WARN_SIGNEDNESS_MISMATCH = 3,            /* JPEG 2000: low precision bits reinterpreted per
                                                             Pixel Representation */
    SIAARA_FRAME_WARN_EXTRA_COMPONENTS = 4,               /* first Samples per Pixel components used */
    SIAARA_FRAME_WARN_CODESTREAM_CROPPED = 5,             /* larger than Rows x Columns: top-left part used */
    SIAARA_FRAME_WARN_PRECISION_ABOVE_BITS_ALLOCATED = 6, /* decodes only if the values fit Bits Allocated */
    SIAARA_FRAME_WARN_CONTAINER_WIDENED = 7,              /* decoded samples widened into Bits Allocated */
    SIAARA_FRAME_WARN_BITS_FROM_CODESTREAM = 8,           /* invalid Bits Allocated: depth from the codestream */
    SIAARA_FRAME_WARN_SIZE_FROM_CODESTREAM = 9,           /* single frame: Rows/Columns from the codestream */
    SIAARA_FRAME_WARN_RLE_RUN_CLAMPED = 10,               /* RLE run cut at its segment end; missing bytes 0 */
    SIAARA_FRAME_WARN_CORRUPT_ENTROPY_DATA = 11           /* JPEG: invalid Huffman codes read as symbol 0 */
};
/* Number of adjustments made for frame `frame` (0-based). Decodes the frame to find them. The pixel
 * info (siaara_file_pixel_info) already reports adjusted Bits Allocated / Bits Stored / size. */
SIAARA_API siaara_status SIAARA_CALL siaara_frame_warning_count(const siaara_file* file, uint32_t frame, uint32_t* out);
/* Adjustment `index` (< count) of frame `frame`: *code (may be NULL) receives SIAARA_FRAME_WARN_*;
 * the message follows the string buffer rules. */
SIAARA_API siaara_status SIAARA_CALL siaara_frame_warning(const siaara_file* file, uint32_t frame, uint32_t index,
                                                          int32_t* code, char* buf, size_t cap, size_t* needed);

/* ---------------------------------------------------------------- save options and stored bytes */
/* Sequence and item lengths. */
enum {
    SIAARA_SEQ_LENGTHS_KEEP = 0,      /* default: as read; new sequences get a sensible choice */
    SIAARA_SEQ_LENGTHS_EXPLICIT = 1,  /* every sequence and item gets an explicit length */
    SIAARA_SEQ_LENGTHS_UNDEFINED = 2  /* every sequence and item gets undefined length + delimiter */
};
/* Group length elements (gggg,0000) outside group 0002. */
enum {
    SIAARA_GROUP_LENGTHS_KEEP = 0,     /* default: kept where present; recomputed when the group
                                          changed, so a kept value is never stale */
    SIAARA_GROUP_LENGTHS_DROP = 1,     /* removed */
    SIAARA_GROUP_LENGTHS_RECOMPUTE = 2 /* a correct group length for every group, added if missing */
};
/* Odd value lengths. */
enum {
    SIAARA_ODD_LENGTHS_KEEP = 0,  /* default: values and fragments read odd stay odd; edited or new
                                     values are padded to even length */
    SIAARA_ODD_LENGTHS_PAD = 1    /* every odd value and fragment is padded to even length */
};
typedef struct siaara_save_options {
    uint32_t struct_size;      /* set to sizeof(siaara_save_options) */
    int32_t sequence_lengths;  /* SIAARA_SEQ_LENGTHS_* */
    int32_t group_lengths;     /* SIAARA_GROUP_LENGTHS_* */
    int32_t odd_lengths;       /* SIAARA_ODD_LENGTHS_* */
    int32_t allow_incomplete;  /* 0 (default): a file whose read was not complete (see
                                  siaara_file_read_report: skipped bytes, truncated input,
                                  header-only read) is refused with SIAARA_ERR_REJECTED, so a save
                                  never silently writes less than was read. 1: write it anyway. */
} siaara_save_options;
/* siaara_file_save with options (NULL = defaults, the same as siaara_file_save). The defaults keep
 * every unedited element as it was read: sequence/item length modes, group lengths, odd lengths,
 * the preamble. An element counts as edited when it was set through a setter or its value size
 * changed. When transfer_syntax_uid changes the syntax, "keep" still applies where it is valid;
 * group lengths are recomputed for the new encoding, and sequences that a reader could only
 * recognise through a (private) dictionary get undefined length when the target is implicit VR.
 * A struct_size smaller than this version's struct is accepted (missing fields use defaults). */
SIAARA_API siaara_status SIAARA_CALL siaara_file_save_ex(const siaara_file* file, const char* utf8_path,
                                                         size_t path_len, const char* transfer_syntax_uid,
                                                         const siaara_save_options* options);
/* The value bytes of element `tag` of `ds` (the file's data set, its meta, or an item borrowed
 * from it) exactly as stored in the file's transfer syntax: for Explicit VR Big Endian, big
 * endian (and the main data set's OW Pixel Data of the GE private syntax 1.2.840.113619.5.2, whose
 * pixels are big endian); otherwise the same bytes siaara_dataset_get_raw returns. Copied into buf (cap bytes);
 * *needed receives the value length. SIAARA_ERR_BUFFER_TOO_SMALL when cap < length (buf may be
 * NULL with cap 0 to query the length). The meta group is always little endian. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_get_raw_stored(const siaara_file* file, const siaara_dataset* ds,
                                                                uint32_t tag, uint8_t* buf, size_t cap,
                                                                size_t* needed);
/* ================================================================ viewer / PACS additions (wip-capi)
 * Everything below was added for the PACS application. The general rules at the top apply: every
 * new options / result struct starts with struct_size (set it to sizeof the struct), buffers
 * follow buf / cap / needed, strings are UTF-8, and each function states its thread safety. */

/* ---------------------------------------------------------------- decoded frames and 2D viewing */
/* A frame decoded once, for repeated rendering (window / level, colour maps, zoom, pan) without
 * decoding again. It holds its own copy of the data set: the file may be freed afterwards. A
 * siaara_frame is immutable: it may be used from many threads at once. */
typedef struct siaara_frame siaara_frame;
/* A softcopy presentation state (GSPS, colour / pseudo-colour / blending) read from a file. */
typedef struct siaara_pstate siaara_pstate;

/* Opens frame `frame` (0-based) of the file and decodes it. Caller owns *out: free with
 * siaara_frame_free. Thread safety: the file may be read by other threads meanwhile. */
SIAARA_API siaara_status SIAARA_CALL siaara_frame_open(const siaara_file* file, uint32_t frame, siaara_frame** out);
SIAARA_API void SIAARA_CALL siaara_frame_free(siaara_frame* frame) SIAARA_NOEXCEPT;

enum {
    SIAARA_FRAMEF_MONOCHROME = 1 << 0,       /* MONOCHROME1 or MONOCHROME2 */
    SIAARA_FRAMEF_INVERTED = 1 << 1,         /* MONOCHROME1 / Presentation LUT Shape INVERSE */
    SIAARA_FRAMEF_PALETTE = 1 << 2,          /* PALETTE COLOR palette present */
    SIAARA_FRAMEF_SUPPLEMENTAL_PALETTE = 1 << 3, /* Supplemental Palette Color LUT (C.7.6.19) */
    SIAARA_FRAMEF_MODALITY_LUT = 1 << 4,     /* Modality LUT Sequence instead of rescale */
    SIAARA_FRAMEF_PRESENTATION_LUT = 1 << 5, /* Presentation LUT Sequence */
    SIAARA_FRAMEF_SHUTTER_RECTANGULAR = 1 << 6,
    SIAARA_FRAMEF_SHUTTER_CIRCULAR = 1 << 7,
    SIAARA_FRAMEF_SHUTTER_POLYGONAL = 1 << 8,
    SIAARA_FRAMEF_SHUTTER_BITMAP = 1 << 9,
    SIAARA_FRAMEF_FLOAT = 1 << 10,           /* Float / Double Float Pixel Data */
    SIAARA_FRAMEF_VOI_FROM_FUNCTIONAL_GROUPS = 1 << 11,
    SIAARA_FRAMEF_DOSE_GRID_SCALING = 1 << 12 /* RT Dose: rescale_slope is Dose Grid Scaling (values in Dose Units) */
};

typedef struct siaara_frame_properties {
    uint32_t struct_size;            /* set to sizeof(siaara_frame_properties) */
    uint32_t frame;                  /* 0-based frame index */
    uint32_t number_of_frames;
    uint32_t rows;
    uint32_t columns;
    uint32_t samples_per_pixel;
    uint32_t bits_allocated;
    uint32_t bits_stored;
    uint32_t flags;                  /* SIAARA_FRAMEF_* */
    uint32_t voi_count;              /* windows + VOI LUTs (siaara_voi_get) */
    uint32_t overlay_count;          /* siaara_overlay_get */
    uint8_t shutter_rgb[3];          /* colour shown where shuttered */
    uint8_t reserved0;
    double rescale_slope;            /* 1 / 0 with a Modality LUT or without rescale */
    double rescale_intercept;
    double min_value;                /* minimum / maximum modality value present in the frame */
    double max_value;
    uint64_t decoded_bytes;          /* size of siaara_frame_pixels (DICOM native layout) */
    char photometric[20];            /* NUL-terminated (truncated if longer) */
} siaara_frame_properties;

/* Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_frame_info(const siaara_frame* frame, siaara_frame_properties* out);
/* Borrowed: the decoded frame in the DICOM native layout of siaara_frame_decode with
 * SIAARA_FRAME_NATIVE, valid until the frame is freed. Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_frame_pixels(const siaara_frame* frame, const uint8_t** out_data,
                                                         uint64_t* out_bytes);
/* Zero-copy access to frame `frame` of the file's native (uncompressed, little-endian, not
 * 1-bit) Pixel Data: *out_data points into the file's own value (borrowed, valid until the file
 * is freed or its Pixel Data changes). SIAARA_ERR_UNSUPPORTED when the frame cannot be viewed in
 * place (encapsulated, big endian, 1-bit): use siaara_frame_decode. Thread safety: as reading. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_frame_view(const siaara_file* file, uint32_t frame,
                                                            const uint8_t** out_data, uint64_t* out_bytes);

enum { SIAARA_VOI_IDENTITY = 3 };    /* siaara_view2d_options.voi_mode: the whole possible range */
enum { SIAARA_VOI_LINEAR = 0, SIAARA_VOI_LINEAR_EXACT = 1, SIAARA_VOI_SIGMOID = 2 }; /* VOI LUT Function */
enum { SIAARA_PIXELS_RGBA8 = 0, SIAARA_PIXELS_GRAY8 = 1 };

typedef struct siaara_view2d_options {
    uint32_t struct_size;            /* set to sizeof(siaara_view2d_options) */
    uint32_t viewport_width;         /* output size; 0 / 0 = the image size (zoom 0 then means 1) */
    uint32_t viewport_height;
    int32_t output_format;           /* SIAARA_PIXELS_*: 4 or 1 bytes per pixel */
    double zoom;                     /* output pixels per image pixel; 0 = fit the viewport */
    double pan_x;                    /* image centre offset from the viewport centre, output pixels */
    double pan_y;
    double rotation;                 /* clockwise degrees, after the flips */
    int32_t flip_horizontal;         /* 0/1 */
    int32_t flip_vertical;           /* 0/1 */
    int32_t interpolation;           /* SIAARA_INTERP_NEAREST or SIAARA_INTERP_LINEAR */
    int32_t colormap;                /* SIAARA_COLORMAP_* for grayscale images (GRAY = none) */
    int32_t voi_mode;                /* SIAARA_VOI_FILE (voi_index), _EXPLICIT, _FULL_RANGE, _IDENTITY */
    uint32_t voi_index;              /* SIAARA_VOI_FILE: n-th of siaara_voi_get */
    double window_center;            /* SIAARA_VOI_EXPLICIT */
    double window_width;
    int32_t voi_function;            /* SIAARA_VOI_LINEAR etc., SIAARA_VOI_EXPLICIT */
    int32_t invert;                  /* 0/1: on top of MONOCHROME1 / Presentation LUT Shape */
    int32_t no_overlays;             /* 0/1 */
    int32_t no_shutters;             /* 0/1 */
    uint32_t overlay_groups;         /* bit k shows group 0x6000 + 2k; 0 = all */
    uint8_t overlay_rgba[4];         /* all zero = opaque white */
    uint8_t background_rgba[4];      /* outside the image; written as given (all zero: transparent) */
    uint32_t reserved0;
    const siaara_pstate* presentation_state; /* NULL, or a state applied before the viewport (M3) */
} siaara_view2d_options;

/* Renders the frame into a viewport: VOI / colour pipeline at image resolution, then flips,
 * rotation, zoom and pan. Output viewport_width x viewport_height pixels of 4 (RGBA8) or 1
 * (GRAY8) bytes. options may be NULL (whole image, file VOI, RGBA8). Buffer rules as for
 * siaara_frame_decode. Thread safety: any thread, also concurrently on one frame. */
SIAARA_API siaara_status SIAARA_CALL siaara_frame_render(const siaara_frame* frame, const siaara_view2d_options* options,
                                                         uint8_t* buf, size_t cap, size_t* needed);
/* Maps a point between viewport and image PIXEL coordinates ((0,0) = top left corner of the
 * first pixel) for the given options: to_image nonzero maps viewport -> image, 0 image ->
 * viewport. With a presentation state, image coordinates are those of the source image.
 * Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_frame_map_point(const siaara_frame* frame, const siaara_view2d_options* options,
                                                            int32_t to_image, double x, double y, double* out_x,
                                                            double* out_y);

/* ---- VOI (window centre / width and VOI LUTs) of a frame */
enum { SIAARA_VOI_KIND_WINDOW = 0, SIAARA_VOI_KIND_LUT = 1 };

typedef struct siaara_voi_info {
    uint32_t struct_size;            /* set to sizeof(siaara_voi_info) */
    int32_t kind;                    /* SIAARA_VOI_KIND_* */
    int32_t function;                /* SIAARA_VOI_LINEAR etc. (windows) */
    int32_t from_functional_groups;  /* 0/1: Frame VOI LUT functional group */
    double center;                   /* windows */
    double width;
    uint32_t lut_entries;            /* VOI LUTs */
    int32_t lut_first_mapped;
    uint32_t lut_bits;
    uint32_t reserved0;
} siaara_voi_info;

/* Windows first (file order), then VOI LUT Sequence items; index is the voi_index for
 * SIAARA_VOI_FILE. Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_voi_count(const siaara_frame* frame, uint32_t* out);
/* Fills *out and writes the explanation (Window Center & Width Explanation / LUT Explanation)
 * to buf (string buffer rules; buf may be NULL with cap 0). */
SIAARA_API siaara_status SIAARA_CALL siaara_voi_get(const siaara_frame* frame, uint32_t index, siaara_voi_info* out,
                                                    char* buf, size_t cap, size_t* needed);
/* LUT Data of a VOI LUT (lut_entries values; array buffer rules as for siaara_series_sort). */
SIAARA_API siaara_status SIAARA_CALL siaara_voi_lut_data(const siaara_frame* frame, uint32_t index, uint16_t* buf,
                                                         size_t cap, size_t* needed);

/* ---- overlays (PS3.3 C.9.2) and display shutters (C.7.6.11, C.7.6.15) as masks */
typedef struct siaara_overlay_info {
    uint32_t struct_size;            /* set to sizeof(siaara_overlay_info) */
    uint32_t group;                  /* 0x6000 .. 0x601E */
    uint32_t rows;
    uint32_t columns;
    int32_t origin_row;              /* 1-based, may be < 1 */
    int32_t origin_column;
    int32_t type;                    /* 'G' graphics or 'R' ROI */
    uint32_t frames;                 /* Number of Frames in Overlay */
    uint32_t image_frame_origin;     /* 1-based */
    int32_t embedded;                /* 0/1: retired overlay in unused Pixel Data bits */
    int32_t covers_frame;            /* 0/1: the overlay has a plane for this frame */
    uint32_t reserved0;
} siaara_overlay_info;

enum { SIAARA_OVERLAY_LABEL = 0, SIAARA_OVERLAY_DESCRIPTION = 1, SIAARA_OVERLAY_SUBTYPE = 2 };

/* Thread safety for all overlay / shutter calls: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_overlay_get(const siaara_frame* frame, uint32_t index, siaara_overlay_info* out);
/* SIAARA_OVERLAY_* text of an overlay (string buffer rules). */
SIAARA_API siaara_status SIAARA_CALL siaara_overlay_text(const siaara_frame* frame, uint32_t index, int32_t field,
                                                         char* buf, size_t cap, size_t* needed);
/* rows x columns bytes of the image: 1 where overlay `index` is set for this frame, else 0
 * (all 0 when it does not cover the frame). Buffer rules as for siaara_frame_decode. */
SIAARA_API siaara_status SIAARA_CALL siaara_overlay_mask(const siaara_frame* frame, uint32_t index, uint8_t* buf,
                                                         size_t cap, size_t* needed);
/* rows x columns bytes: 1 = visible, 0 = covered by the display shutters (all 1 without shutters). */
SIAARA_API siaara_status SIAARA_CALL siaara_shutter_mask(const siaara_frame* frame, uint8_t* buf, size_t cap,
                                                         size_t* needed);

/* ---- palettes (PS3.3 C.7.6.3.1.5, C.7.6.19, PS3.6 Annex B) */
typedef struct siaara_palette_info {
    uint32_t struct_size;            /* set to sizeof(siaara_palette_info) */
    uint32_t entries;
    int32_t first_mapped;            /* first stored value mapped */
    uint32_t bits;                   /* 8 or 16: entry range */
    int32_t has_alpha;               /* 0/1: Alpha Palette Color LUT Data */
    int32_t segmented;               /* 0/1: read from segmented data */
    int32_t supplemental;            /* 0/1: Supplemental Palette (grayscale image) */
    uint32_t reserved0;
} siaara_palette_info;

/* The frame's palette: *info and entries * 4 values R, G, B, A in the palette's own precision
 * (A = 2^bits - 1 without alpha data). buf may be NULL with cap 0 to get *info and *needed only
 * (array buffer rules, count in uint16_t). SIAARA_ERR_NOT_FOUND without a palette.
 * Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_frame_palette(const siaara_frame* frame, siaara_palette_info* info,
                                                          uint16_t* buf, size_t cap, size_t* needed);
/* A standard colour map (SIAARA_COLORMAP_*) as 256 RGBA8 entries (1024 bytes; buffer rules as
 * for siaara_frame_decode). Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_colormap_rgba8(int32_t colormap, uint8_t* buf, size_t cap, size_t* needed);

/* ---------------------------------------------------------------- presentation states (PS3.3 A.33) */
/* Grayscale, colour, pseudo-colour and blending softcopy presentation states. A siaara_pstate
 * is immutable: many threads may use it at once. Apply one by setting
 * siaara_view2d_options.presentation_state: its modality / VOI / presentation LUT, shutters,
 * activated overlays, graphic annotations (text is not drawn), spatial transformation and
 * displayed area replace the image's own. */
enum {
    SIAARA_PSTATE_GRAYSCALE = 0, SIAARA_PSTATE_COLOR = 1, SIAARA_PSTATE_PSEUDO_COLOR = 2,
    SIAARA_PSTATE_BLENDING = 3, SIAARA_PSTATE_OTHER = 4
};

typedef struct siaara_pstate_properties {
    uint32_t struct_size;            /* set to sizeof(siaara_pstate_properties) */
    int32_t kind;                    /* SIAARA_PSTATE_* */
    int32_t has_spatial_transform;   /* 0/1 */
    int32_t rotation;                /* clockwise: 0, 90, 180, 270 */
    int32_t flip;                    /* 0/1: horizontal flip after rotation */
    uint32_t reference_count;        /* referenced images (siaara_pstate_reference) */
    uint32_t annotation_count;       /* Graphic Annotation Sequence items */
    uint32_t text_count;             /* text objects in all annotations */
    uint32_t graphic_count;          /* graphic objects in all annotations */
    uint32_t layer_count;
    uint32_t voi_count;              /* Softcopy VOI LUT items */
    uint32_t displayed_area_count;
    int32_t has_shutters;            /* 0/1 */
    int32_t has_modality;            /* 0/1: its own Modality LUT / rescale */
    uint32_t warning_count;
    uint32_t reserved0;
} siaara_pstate_properties;

enum {
    SIAARA_PSTATE_LABEL = 0, SIAARA_PSTATE_DESCRIPTION = 1, SIAARA_PSTATE_CREATOR = 2,
    SIAARA_PSTATE_CREATION_DATE = 3, SIAARA_PSTATE_CREATION_TIME = 4, SIAARA_PSTATE_SOP_INSTANCE_UID = 5,
    SIAARA_PSTATE_SOP_CLASS_UID = 6
};

/* Reads a presentation state. SIAARA_ERR_UNSUPPORTED for other SOP classes. Caller owns *out:
 * free with siaara_pstate_free. Thread safety: the file may be read by other threads meanwhile. */
SIAARA_API siaara_status SIAARA_CALL siaara_pstate_open(const siaara_file* file, siaara_pstate** out);
SIAARA_API void SIAARA_CALL siaara_pstate_free(siaara_pstate* ps) SIAARA_NOEXCEPT;
/* Thread safety for the calls below: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_pstate_info(const siaara_pstate* ps, siaara_pstate_properties* out);
/* SIAARA_PSTATE_LABEL etc. (string buffer rules). */
SIAARA_API siaara_status SIAARA_CALL siaara_pstate_string(const siaara_pstate* ps, int32_t field, char* buf, size_t cap,
                                                          size_t* needed);
/* SOP Instance UID of referenced image `index` (string buffer rules); *frame_count (may be NULL)
 * receives the number of Referenced Frame Numbers (0 = all frames). */
SIAARA_API siaara_status SIAARA_CALL siaara_pstate_reference(const siaara_pstate* ps, uint32_t index, char* buf, size_t cap,
                                                             size_t* needed, uint32_t* frame_count);
/* *out = 1 when the state applies to frame frame_1based (1-based) of that instance. */
SIAARA_API siaara_status SIAARA_CALL siaara_pstate_applies(const siaara_pstate* ps, const char* sop_instance_uid,
                                                           size_t len, uint32_t frame_1based, int32_t* out);

/* ---------------------------------------------------------------- cancellation */
/* A cancellation token for long calls (DIMSE, DICOMweb, volume build, store verify / rebuild,
 * DICOMDIR write). siaara_cancel_set may be called from any thread while calls that use the token
 * run; they then stop at the next safe point and return SIAARA_ERR_CANCELLED. Network calls abort
 * their association / connection at once (A-ABORT, socket shutdown), except while a TCP connect is
 * in progress (bounded by the connect timeout). A token stays set until siaara_cancel_reset. The
 * token must outlive the calls that use it. */
typedef struct siaara_cancel siaara_cancel;

SIAARA_API siaara_status SIAARA_CALL siaara_cancel_create(siaara_cancel** out);
SIAARA_API void SIAARA_CALL siaara_cancel_free(siaara_cancel* token) SIAARA_NOEXCEPT;
/* Thread safety: any thread, at any time. */
SIAARA_API siaara_status SIAARA_CALL siaara_cancel_set(siaara_cancel* token);
/* Clears the token for reuse; do not call while a call uses it. */
SIAARA_API siaara_status SIAARA_CALL siaara_cancel_reset(siaara_cancel* token);
/* *out = 1 when set. Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_cancel_is_set(const siaara_cancel* token, int32_t* out);

/* ---------------------------------------------------------------- logging */
enum { SIAARA_LOG_DEBUG = 0, SIAARA_LOG_INFO = 1, SIAARA_LOG_WARNING = 2, SIAARA_LOG_ERROR = 3 };
/* Receives the library's diagnostics: association and TLS events, network services, failures.
 * contains_phi = 1 when the message may hold patient data or instance identifiers (names,
 * IDs, SOP Instance UIDs); 0 when it holds only AE titles, addresses, statuses and error text.
 * message is UTF-8, valid during the call only. The callback may be called from
 * several threads at once and must not call siaara_set_log_callback. */
typedef void (SIAARA_CALL* siaara_log_callback)(void* user, int32_t level, int32_t contains_phi, const char* message);
/* Installs (cb NULL: removes) the process-wide log callback for messages at min_level and above.
 * Thread safety: any thread. Returns once calls of the previous callback still running have
 * finished, so its `user` may be freed afterwards. */
SIAARA_API siaara_status SIAARA_CALL siaara_set_log_callback(siaara_log_callback cb, void* user, int32_t min_level);
/* Messages that may hold patient data (contains_phi) are delivered only when include_phi is 1;
 * by default (0) they arrive as a fixed notice with contains_phi = 0 (PHI logging is off by default). */
SIAARA_API siaara_status SIAARA_CALL siaara_set_log_include_phi(int32_t include_phi);

/* ---------------------------------------------------------------- DIMSE networking (PS3.7, PS3.4) */
/* Every SCU call opens one association to the peer, performs the operation and releases the
 * association; calls are independent and may run on many threads at once. Return values:
 * SIAARA_OK when the final DIMSE status is Success or Warning; SIAARA_ERR_REJECTED when the
 * association was rejected or the operation failed (the DIMSE status is in result->status);
 * SIAARA_ERR_CANCELLED after siaara_cancel_set, a callback asking to stop, or a Cancel status;
 * SIAARA_ERR_NETWORK / SIAARA_ERR_TIMEOUT for transport failures. */
typedef struct siaara_dimse_peer {
    uint32_t struct_size;            /* set to sizeof(siaara_dimse_peer) */
    uint32_t port;                   /* 1..65535 */
    const char* host;                /* name or address, NUL-terminated */
    const char* calling_ae;          /* NULL: "SIAARA_SCU" */
    const char* called_ae;           /* NULL: "ANY-SCP" */
    const siaara_tls_options* tls;   /* NULL: plain TCP; else TLS (never a fallback to plain TCP) */
    int32_t connect_timeout_ms;      /* 0 = 15000 */
    int32_t acse_timeout_ms;         /* association negotiation and release; 0 = 30000 */
    int32_t dimse_timeout_ms;        /* each DIMSE response; 0 = 60000 */
    uint32_t max_pdu;                /* largest PDU we accept; 0 = 65536 */
    int32_t allow_lossy_transcode;   /* 0/1: C-STORE may code lossily when the peer accepts only that; the
                                        lossy transfer syntaxes the transcoder can write are then proposed too */
    uint32_t reserved0;
    /* Fields below are read only when struct_size covers them (0 when it does not). */
    int32_t allow_incomplete;        /* 0 (default): a file whose read was not complete (truncated input, skipped
                                        bytes; siaara_file_read_report) is not sent - it counts as failed, with
                                        SIAARA_ERR_REJECTED and SIAARA_STORE_INCOMPLETE_INPUT in its
                                        siaara_store_outcome - as siaara_file_save refuses it. 1: send it anyway */
    int32_t keep_uid_on_lossy;       /* 0 (default): a lossy transcode on the way gives the instance a new SOP
                                        Instance UID (R-093; reported in siaara_store_outcome and by the
                                        siaara_uid_change_callback). 1: keep the file's UID (see
                                        siaara_transcode_params.keep_uid_on_lossy) */
} siaara_dimse_peer;

typedef struct siaara_dimse_result {
    uint32_t struct_size;            /* set to sizeof(siaara_dimse_result) */
    uint32_t status;                 /* last / final DIMSE status (0 = Success, 0xFE00 = Cancel) */
    uint32_t completed;              /* sub-operations or files: done with Success */
    uint32_t failed;
    uint32_t warning;
    uint32_t remaining;              /* as reported by the peer (C-MOVE / C-GET), or files left */
    uint32_t matches;                /* C-FIND responses */
    uint32_t current;                /* progress callbacks: index of the item just handled */
    /* Written only when struct_size covers them: why no association was established (all 0 otherwise). */
    int32_t connect_failure;         /* SIAARA_CONNECT_* */
    uint32_t reject_result;          /* A-ASSOCIATE-RJ (PS3.8 9.3.4): 1 rejected-permanent, 2 rejected-transient */
    uint32_t reject_source;          /* 1 service-user, 2 service-provider (ACSE), 3 service-provider (presentation) */
    uint32_t reject_reason;          /* with source 1: 1 no reason given, 2 application context name not
                                        supported, 3 calling AE title not recognized, 7 called AE title not
                                        recognized; source 2: 1 no reason, 2 protocol version not supported;
                                        source 3: 1 temporary congestion, 2 local limit exceeded */
} siaara_dimse_result;

/* siaara_dimse_result.connect_failure: at which step an SCU call failed to get its association.
 * The return value stays SIAARA_ERR_NETWORK / _TIMEOUT / _REJECTED / _CANCELLED as before. */
enum {
    SIAARA_CONNECT_OK = 0,           /* the association was established (or the call failed earlier / later) */
    SIAARA_CONNECT_RESOLVE = 1,      /* the host name did not resolve */
    SIAARA_CONNECT_REFUSED = 2,      /* connection refused: nothing listens on that port */
    SIAARA_CONNECT_TIMEOUT = 3,      /* no TCP connection within connect_timeout_ms */
    SIAARA_CONNECT_TLS = 4,          /* TLS setup or handshake failed (certificate, protocol, no TLS on the peer) */
    SIAARA_CONNECT_REJECTED = 5,     /* A-ASSOCIATE-RJ: see reject_result / reject_source / reject_reason */
    SIAARA_CONNECT_ACSE_TIMEOUT = 6, /* connected, but no answer to the association request in acse_timeout_ms */
    SIAARA_CONNECT_CLOSED = 7,       /* the peer closed or aborted during the negotiation */
    SIAARA_CONNECT_CANCELLED = 8,    /* the cancel token was set while connecting */
    SIAARA_CONNECT_OTHER = 99        /* unreachable network, local failure ... (see siaara_last_error) */
};

/* Progress during C-MOVE / C-GET (every pending response) and C-STORE of several files (every
 * file). Called on the calling thread. Return 0 to continue, non-zero to cancel (C-CANCEL for
 * retrieves; the remaining files for C-STORE). */
typedef int32_t (SIAARA_CALL* siaara_dimse_progress_callback)(void* user, const siaara_dimse_result* progress);
/* One C-FIND match, borrowed during the call (read it with the data set functions). Return 0 to
 * continue, non-zero to send C-CANCEL. Called on the calling thread. */
typedef int32_t (SIAARA_CALL* siaara_find_callback)(void* user, const siaara_dataset* match);

enum {
    SIAARA_QR_PATIENT_ROOT = 0,      /* Patient Root Query/Retrieve Information Model */
    SIAARA_QR_STUDY_ROOT = 1,        /* Study Root Query/Retrieve Information Model */
    SIAARA_QR_WORKLIST = 2           /* Modality Worklist (C-FIND only) */
};

/* Thread safety for every siaara_dimse_* call: any thread; the arguments are only read. cancel,
 * callbacks and result may be NULL. */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_echo(const siaara_dimse_peer* peer, siaara_cancel* cancel,
                                                       siaara_dimse_result* result);
/* C-FIND. identifier: the query keys, with Query/Retrieve Level (0008,0052) for Q/R models
 * (PATIENT, STUDY, SERIES, IMAGE). */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_find(const siaara_dimse_peer* peer, int32_t model,
                                                       const siaara_dataset* identifier, siaara_cancel* cancel,
                                                       siaara_find_callback cb, void* user, siaara_dimse_result* result);
/* C-MOVE to destination_ae (NUL-terminated; the peer must know its address). */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_move(const siaara_dimse_peer* peer, int32_t model,
                                                       const siaara_dataset* identifier, const char* destination_ae,
                                                       siaara_cancel* cancel, siaara_dimse_progress_callback cb,
                                                       void* user, siaara_dimse_result* result);

typedef struct siaara_get_options {
    uint32_t struct_size;            /* set to sizeof(siaara_get_options) */
    int32_t duplicate_policy;        /* SIAARA_DUP_*, for the store and the folder: with KEEP an existing
                                        <SOP Instance UID>.dcm is left unchanged (logged as a warning) */
    siaara_store* store;             /* received instances are added here (may be NULL) */
    const char* folder;              /* and / or written here as <SOP Instance UID>.dcm (may be NULL),
                                        owner-only (mode 0600 / owner-only DACL). On Windows a file that
                                        replaces an existing one takes this owner-only DACL, not the DACL of
                                        the replaced file or of the folder (grant access on the files or use
                                        a store opened with file_mode where others must read them) */
    const char* const* sop_classes;  /* storage SOP classes to accept; NULL: the toolkit's list */
    uint32_t sop_class_count;
    int32_t native_only;             /* 0/1: propose only uncompressed transfer syntaxes */
    /* Fields below are read only when struct_size covers them. */
    int32_t strict_store_uids;       /* 0/1: refuse (0xA900) an instance whose C-STORE command UIDs differ from
                                        (0008,0016)/(0008,0018) of its data set; 0: keep it under the data set's
                                        UIDs and log a warning */
    int32_t incomplete_policy;       /* SIAARA_INCOMPLETE_* for received instances (default: refuse with 0xC000;
                                        the sub-operation then counts as failed at the SCP) */
} siaara_get_options;

/* C-GET; the instances arrive on the same association. options->store or ->folder is required. */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_get(const siaara_dimse_peer* peer, int32_t model,
                                                      const siaara_dataset* identifier, const siaara_get_options* options,
                                                      siaara_cancel* cancel, siaara_dimse_progress_callback cb, void* user,
                                                      siaara_dimse_result* result);
/* C-STORE of files over one association: one presentation context per SOP class and transfer
 * syntax found, plus uncompressed ones, so an instance the peer does not accept as stored is
 * transcoded (lossy only with allow_lossy_transcode). Every file is attempted; result counts
 * them. Returns SIAARA_OK when all were stored (Success or Warning), else SIAARA_ERR_REJECTED.
 * paths[i] has length lens[i] (lens may be NULL: NUL-terminated). */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_store_files(const siaara_dimse_peer* peer, const char* const* paths,
                                                              const size_t* lens, uint32_t count, siaara_cancel* cancel,
                                                              siaara_dimse_progress_callback cb, void* user,
                                                              siaara_dimse_result* result);
/* C-STORE of one file in memory (same transcoding rules). */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_store(const siaara_dimse_peer* peer, const siaara_file* file,
                                                        siaara_cancel* cancel, siaara_dimse_result* result);
/* Storage Commitment Push Model (PS3.4 J): requests commitment of `count` instances and waits
 * up to wait_ms (0 = 60000) for the N-EVENT-REPORT on the same association.
 * failure_reasons (may be NULL) receives per instance 0 = committed, else the Failure Reason
 * (0x0110 ...), or 0xFFFF when the report did not mention it. result->completed / failed count
 * them. SIAARA_ERR_TIMEOUT when no report arrived (a peer that reports on a new association is
 * not supported here). */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_commit(const siaara_dimse_peer* peer, const char* const* sop_classes,
                                                         const char* const* sop_instances, uint32_t count, int32_t wait_ms,
                                                         siaara_cancel* cancel, uint16_t* failure_reasons,
                                                         siaara_dimse_result* result);
/* A Modality Worklist query identifier (PS3.4 K.6) with the usual return keys; every argument
 * may be NULL (universal matching); date may be a range "YYYYMMDD-YYYYMMDD". Caller owns *out:
 * use its data set with siaara_dimse_find(SIAARA_QR_WORKLIST), free with siaara_file_free. */
SIAARA_API siaara_status SIAARA_CALL siaara_worklist_query(const char* station_ae, const char* modality, const char* date,
                                                           const char* patient_name, const char* patient_id,
                                                           siaara_file** out);

/* ---- Storage SCP */
typedef struct siaara_scp siaara_scp;

typedef struct siaara_scp_options {
    uint32_t struct_size;            /* set to sizeof(siaara_scp_options) */
    uint32_t port;                   /* 0 = an ephemeral port (siaara_scp_port) */
    const char* ae_title;            /* NULL: "SIAARA_SCP" */
    const char* bind_address;        /* NULL: all interfaces */
    const char* const* allowed_calling_aes; /* peers accepted; empty needs allow_any_calling_ae */
    uint32_t allowed_count;
    uint32_t max_associations;       /* 0 = 8; at most 64 */
    int32_t accept_any_called_ae;    /* 0/1: also accept associations addressed to another AE title */
    int32_t accept_any_sop_class;    /* 0/1: store any SOP class (default: the toolkit's storage list) */
    int32_t accept_compressed;       /* 0/1: also accept the compressed transfer syntaxes the toolkit knows */
    int32_t duplicate_policy;        /* SIAARA_DUP_* */
    int32_t idle_timeout_ms;         /* 0 = 300000 */
    int32_t dimse_timeout_ms;        /* 0 = 60000 */
    uint32_t max_pdu;                /* 0 = 65536 */
    int32_t allow_any_calling_ae;    /* 0/1: with no allowed_calling_aes, accept any peer (else start fails) */
    const siaara_tls_options* tls;   /* NULL: plain TCP; else every connection must use TLS (needs an identity) */
    /* Fields below are read only when struct_size covers them. */
    int32_t strict_store_uids;       /* 0/1: refuse (0xA900) a C-STORE whose command UIDs differ from
                                        (0008,0016)/(0008,0018) of the data set; 0: store it under the data set's
                                        UIDs (event uid_mismatch, command UIDs reported) */
    uint32_t reserved0;              /* set to 0 */
    /* Data sets larger than spool_threshold are received to disk, so memory per association stays
     * below it (at most max_associations x spool_threshold in all). */
    const char* spool_directory;     /* NULL: a private per-user folder under the system temp folder */
    uint64_t spool_threshold;        /* 0 = 16 MiB */
    uint64_t spool_max_total_bytes;  /* all spool files together; 0 = no cap. Over it: 0xA700 */
    uint64_t spool_min_free_disk_bytes; /* free space the spool leaves; 0 = default (64 MiB). Below: 0xA700 */
    uint32_t event_queue_limit;      /* events queued for the callback; 0 = 1024 (see siaara_scp_callback) */
    uint32_t reserved1;              /* set to 0 */
    uint64_t event_queue_max_bytes;  /* data sets held by queued events, approximate; 0 = 64 MiB. Events that
                                        do not fit are dropped by the same policy as a full queue (R-109) */
    int32_t incomplete_policy;       /* SIAARA_INCOMPLETE_*: a received C-STORE data set that could not be read
                                        completely (truncated, bytes skipped, recovered by a heuristic) */
    uint32_t reserved2;              /* set to 0 */
} siaara_scp_options;

/* What a receiver does with a data set whose read was not complete (siaara_read_report loss bits):
 * the Storage SCP (siaara_scp_options), C-GET (siaara_get_options), the STOW-RS server
 * (siaara_web_server_options) and siaara_web_wado_retrieve_to_ex share this one policy.
 * REFUSE: nothing is stored - C-STORE status 0xC000 (and SIAARA_SCP_STORE_FAILED with read_loss),
 *   STOW-RS Failure Reason 0xC000, a retrieved part counted as failed.
 * ACCEPT_AS_RECEIVED: the bytes are stored exactly as they arrived - never transcoded
 *   (store_transfer_syntax is skipped) and never rewritten - and flagged: SIAARA_SCP_STORED with
 *   read_loss != 0, STOW-RS Warning Reason 0xB006, the retrieve report.
 * A part without a Part 10 preamble that is incomplete is refused under both: storing it would
 * need a rewrite. SIAARA_INCOMPLETE_DEFAULT (0) is REFUSE. */
enum { SIAARA_INCOMPLETE_DEFAULT = 0, SIAARA_INCOMPLETE_REFUSE = 1, SIAARA_INCOMPLETE_ACCEPT_AS_RECEIVED = 2 };

enum { SIAARA_SCP_STORED = 0, SIAARA_SCP_STORE_FAILED = 1, SIAARA_SCP_ECHO = 2 };

typedef struct siaara_scp_event {
    uint32_t struct_size;            /* sizeof(siaara_scp_event) */
    int32_t kind;                    /* SIAARA_SCP_* */
    uint32_t dimse_status;           /* status returned to the peer */
    int32_t outcome;                 /* SIAARA_ADD_* for SIAARA_SCP_STORED */
    const char* calling_ae;          /* strings valid during the callback only */
    const char* sop_class_uid;       /* "" for SIAARA_SCP_ECHO */
    const char* sop_instance_uid;
    const char* peer_address;
    /* Present when struct_size covers them. */
    int32_t uid_mismatch;            /* 1: the C-STORE command's UIDs differed from the data set's; the instance
                                        was stored under the data set's (sop_class_uid / sop_instance_uid) */
    const char* command_sop_class_uid;    /* the command's Affected SOP Class UID ("" unless uid_mismatch) */
    const char* command_sop_instance_uid; /* the command's Affected SOP Instance UID ("" unless uid_mismatch) */
    /* Present when struct_size covers them (siaara_scp_start_ex services; "" / NULL / 0 otherwise). */
    const char* original_sop_instance_uid; /* SIAARA_SCP_UID_CHANGED, or SIAARA_SCP_STORED after a lossy conversion:
                                              the UID before; sop_instance_uid is the new one (R-093) */
    const char* transaction_uid;     /* Storage Commitment events */
    const char* destination_ae;      /* SIAARA_SCP_MOVE */
    const siaara_dataset* dataset;   /* MPPS attributes / modifications, the C-FIND or worklist identifier, the
                                        commitment request or report, UPS data: borrowed, NULL when none */
    uint32_t completed;              /* retrieve sub-operations / C-FIND matches / committed instances */
    uint32_t failed;
    uint32_t warning;
    uint32_t command_field;          /* DIMSE command field of the request (0x0020 C-FIND, 0x0140 N-CREATE ...) */
    /* Present when struct_size covers them. */
    uint64_t association_id;         /* the association the event belongs to: unique among the associations of
                                        this process run (never 0 for a request; 0 when not tied to one) */
    uint32_t read_loss;              /* SIAARA_READ_* loss bits of the received data set. SIAARA_SCP_STORED with
                                        a value != 0: stored exactly as received under
                                        SIAARA_INCOMPLETE_ACCEPT_AS_RECEIVED. SIAARA_SCP_STORE_FAILED with a
                                        value != 0: refused because it was incomplete */
    uint32_t reserved0;
} siaara_scp_event;

/* Called after each event, in order, from one dispatcher thread of the SCP (never concurrently
 * for one SCP), with no toolkit lock held: a slow callback delays only later events. Drop policy
 * (R-094): once event_queue_limit events (default 1024) wait for the callback, an association
 * that has a new event waits at most 1 second for room; then the event is dropped, counted in
 * siaara_scp_stats.events_dropped and a warning is logged (once per SCP). After siaara_scp_stop
 * nothing waits: events that do not fit are dropped at once, so stopping never depends on the
 * callback. The DIMSE operations themselves are never failed or delayed further because of the
 * callback. It must not call siaara_scp_stop (SIAARA_ERR_INVALID_ARGUMENT) or siaara_scp_free
 * (ignored) on its own SCP. */
typedef void (SIAARA_CALL* siaara_scp_callback)(void* user, const siaara_scp_event* event);

typedef struct siaara_scp_stats {
    uint32_t struct_size;            /* set to sizeof(siaara_scp_stats) */
    uint32_t reserved0;
    uint64_t active_associations;
    uint64_t associations_handled;
    uint64_t associations_rejected;
    uint64_t instances_stored;
    uint64_t store_failures;
    uint64_t echoes;
    uint64_t bytes_received;         /* data set bytes of stored instances */
    /* Present when struct_size covers them. */
    uint64_t events_dropped;         /* events not delivered to the callback (R-094 drop policy) */
    uint64_t finds;                  /* C-FIND requests answered (Q/R and worklist) */
    uint64_t retrieves;              /* C-MOVE and C-GET requests */
    uint64_t sub_operations;         /* C-STORE sub-operations of those */
    uint64_t mpps;                   /* MPPS N-CREATE and N-SET */
    uint64_t commitments;            /* Storage Commitment requests answered */
    uint64_t refused;                /* requests refused by a service allow-list or limit */
} siaara_scp_stats;

/* Starts a Storage SCP (and Verification SCP) that adds every received instance to `store`,
 * which must outlive the SCP. cb may be NULL. Caller owns *out: siaara_scp_free stops it.
 * Thread safety: the SCP runs on its own threads; the functions below may be called from any. */
SIAARA_API siaara_status SIAARA_CALL siaara_scp_start(const siaara_scp_options* options, siaara_store* store,
                                                      siaara_scp_callback cb, void* user, siaara_scp** out);
SIAARA_API siaara_status SIAARA_CALL siaara_scp_port(const siaara_scp* scp, uint32_t* out);
SIAARA_API siaara_status SIAARA_CALL siaara_scp_stats_get(const siaara_scp* scp, siaara_scp_stats* out);
/* Stops accepting, aborts running associations and waits for their threads. Events already
 * queued are still delivered until siaara_scp_free. Idempotent. Not from the SCP callback. */
SIAARA_API siaara_status SIAARA_CALL siaara_scp_stop(siaara_scp* scp);
SIAARA_API void SIAARA_CALL siaara_scp_free(siaara_scp* scp) SIAARA_NOEXCEPT;

/* ---------------------------------------------------------------- DICOMweb additions (PS3.18) */
/* Parts of a DICOMweb response (multipart bodies, or one body). Immutable: any thread. */
typedef struct siaara_web_parts siaara_web_parts;

/* Attaches a cancellation token to the client (NULL detaches): siaara_cancel_set then aborts the
 * request the client is running (every siaara_web_* call, including the older ones), and the
 * calls below refuse to start while it is set. The token must outlive the attachment.
 * Thread safety: as every client call (one thread at a time). */
SIAARA_API siaara_status SIAARA_CALL siaara_web_client_set_cancel(siaara_web_client* client, siaara_cancel* token);
/* WADO-RS metadata (application/dicom+json array) of a study, a series (instance_uid NULL) or an
 * instance; series_uid NULL for the whole study. String buffer rules; the body stays available
 * through siaara_web_last_response. HTTP errors as for siaara_web_qido. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_metadata(siaara_web_client* client, const char* study_uid,
                                                              const char* series_uid, const char* instance_uid, char* buf,
                                                              size_t cap, size_t* needed);
/* WADO-RS frames (1-based numbers) of an instance. transfer_syntaxes (ts_count entries, may be
 * NULL / 0: server default) lists acceptable syntaxes in preference order ("*" = as stored);
 * each part is one frame. Caller owns *out: free with siaara_web_parts_free. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_frames(siaara_web_client* client, const char* study_uid,
                                                            const char* series_uid, const char* instance_uid,
                                                            const uint32_t* frames, uint32_t count,
                                                            const char* const* transfer_syntaxes, uint32_t ts_count,
                                                            siaara_web_parts** out);
/* WADO-RS retrieve of a whole study (series_uid and instance_uid NULL), a series (instance_uid
 * NULL) or one instance: one Part 10 stream per part. Transfer syntaxes as above. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_retrieve(siaara_web_client* client, const char* study_uid,
                                                              const char* series_uid, const char* instance_uid,
                                                              const char* const* transfer_syntaxes, uint32_t ts_count,
                                                              siaara_web_parts** out);
/* WADO-URI (PS3.18 chapter 9): content_type NULL = "application/dicom", transfer_syntax may be
 * NULL. The body is the single part. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_uri(siaara_web_client* client, const char* study_uid,
                                                         const char* series_uid, const char* object_uid,
                                                         const char* content_type, const char* transfer_syntax,
                                                         siaara_web_parts** out);
SIAARA_API void SIAARA_CALL siaara_web_parts_free(siaara_web_parts* parts) SIAARA_NOEXCEPT;
SIAARA_API siaara_status SIAARA_CALL siaara_web_parts_count(const siaara_web_parts* parts, uint32_t* out);
/* Borrowed bytes of part `index` (valid until the parts are freed) and its Content-Type in buf
 * (string buffer rules; buf may be NULL with cap 0). */
SIAARA_API siaara_status SIAARA_CALL siaara_web_parts_get(const siaara_web_parts* parts, uint32_t index,
                                                          const uint8_t** out_data, uint64_t* out_size, char* buf,
                                                          size_t cap, size_t* needed);
/* Parses part `index` as a Part 10 stream. Caller owns *out. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_parts_open_file(const siaara_web_parts* parts, uint32_t index,
                                                                siaara_file** out);
/* Adds every part (Part 10 streams) to a store. *added / *failed may be NULL. Returns SIAARA_OK
 * when all were added. Thread safety: as siaara_store_add_memory. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_parts_add_to_store(const siaara_web_parts* parts, siaara_store* store,
                                                                   int32_t policy, uint32_t* added, uint32_t* failed);

/* ---------------------------------------------------------------- cancellable long calls */
/* Progress of a long call: (steps done, total; 0 = unknown), on the calling thread or a worker,
 * never concurrently. Return 0 to continue, non-zero to stop (SIAARA_ERR_CANCELLED). */
typedef int32_t (SIAARA_CALL* siaara_progress_callback)(void* user, uint64_t done, uint64_t total);

/* As siaara_volume_build, with a token and progress per file read (cancel, cb may be NULL).
 * Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_volume_build_ex(const char* const* utf8_paths, const size_t* path_lens,
                                                            uint32_t count, const siaara_volume_options* options,
                                                            siaara_cancel* cancel, siaara_progress_callback cb,
                                                            void* user, siaara_volume** out);
/* As siaara_store_verify, with a token and progress per instance checked. A cancelled verify
 * repairs nothing. Thread safety: as siaara_store_verify. */
SIAARA_API siaara_status SIAARA_CALL siaara_store_verify_ex(siaara_store* store, int32_t deep, int32_t repair,
                                                            siaara_cancel* cancel, siaara_progress_callback cb,
                                                            void* user, siaara_store_verify_report* out);
/* As siaara_store_rebuild, with a token and progress per file (total unknown). A cancelled
 * rebuild leaves the index unchanged. */
SIAARA_API siaara_status SIAARA_CALL siaara_store_rebuild_ex(siaara_store* store, siaara_cancel* cancel,
                                                             siaara_progress_callback cb, void* user);
/* As siaara_dicomdir_write, with a token and progress (two steps per file: read, copy). When
 * cancelled, files already copied stay in <root>/DICOM and no DICOMDIR is written. */
SIAARA_API siaara_status SIAARA_CALL siaara_dicomdir_write_ex(const char* utf8_root, size_t root_len,
                                                              const char* const* files, const size_t* lens, size_t count,
                                                              const siaara_dicomdir_write_options* options,
                                                              siaara_cancel* cancel, siaara_progress_callback cb,
                                                              void* user, uint32_t* warnings);

/* ---------------------------------------------------------------- image encoders and byte buffers */
/* An immutable byte buffer owned by the library (encoder output). Any thread. */
typedef struct siaara_buffer siaara_buffer;

SIAARA_API void SIAARA_CALL siaara_buffer_free(siaara_buffer* buffer) SIAARA_NOEXCEPT;
/* Borrowed bytes, valid until the buffer is freed. */
SIAARA_API siaara_status SIAARA_CALL siaara_buffer_data(const siaara_buffer* buffer, const uint8_t** out_data,
                                                        uint64_t* out_size);
/* Baseline JPEG (JFIF, YCbCr 4:4:4 for colour) of width x height pixels in SIAARA_PIXELS_RGBA8
 * (alpha ignored) or SIAARA_PIXELS_GRAY8 layout, rows top to bottom without padding. quality
 * 1..100, 0 = 90. Caller owns *out. Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_encode_jpeg(const uint8_t* pixels, size_t size, uint32_t width, uint32_t height,
                                                        int32_t format, int32_t quality, siaara_buffer** out);
/* PNG (8-bit RGBA or gray, adaptive filtering, the toolkit's own DEFLATE). level 0..9, -1 = 6.
 * Caller owns *out. Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_encode_png(const uint8_t* pixels, size_t size, uint32_t width, uint32_t height,
                                                       int32_t format, int32_t level, siaara_buffer** out);

/* ---------------------------------------------------------------- GPU device choice */
/* Selects device `index` of siaara_gpu_devices for this process instead of the automatic choice
 * (discrete, integrated, virtual, other, CPU). Views that already rendered on the GPU keep their
 * device; later renders use the new one. SIAARA_ERR_NOT_FOUND for an index past the list.
 * Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_gpu_select(uint32_t index);

/* ---------------------------------------------------------------- data set additions */
/* Appends an empty item to sequence `tag` (created with VR SQ when absent) and returns it,
 * borrowed and writable: valid until that sequence or its owner changes (re-fetch items with
 * siaara_dataset_item after adding more). *index (may be NULL) receives its 0-based position.
 * Thread safety: needs exclusive access to the data set. */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_append_item(siaara_dataset* ds, uint32_t tag, siaara_dataset** out,
                                                                uint32_t* index);
/* Removes item `index` of sequence `tag`. Thread safety: exclusive access. */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_remove_item(siaara_dataset* ds, uint32_t tag, uint32_t index);
/* All values of a string element at once: each value NUL-terminated, one after the other, in
 * buf (padding removed); *needed is the total size including every NUL and *count (may be NULL)
 * the number of values. utf8 nonzero decodes text with the Specific Character Set as
 * siaara_dataset_get_utf8 does. buf may be NULL with cap 0 to query. Thread safety: as reading. */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_strings(const siaara_dataset* ds, uint32_t tag, int32_t utf8,
                                                                char* buf, size_t cap, size_t* needed, uint32_t* count);

/* ---------------------------------------------------------------- diagnostics */
/* Number of handles of any type currently allocated by this library (files, frames, stores,
 * results, clients, ...), for leak tests. Thread safety: any thread (a snapshot). */
SIAARA_API siaara_status SIAARA_CALL siaara_debug_live_handles(int64_t* out);

/* ---------------------------------------------------------------- validation and repair */
/* Validation checks a file against the data dictionary (VR, VM, value length and format), the
 * structure rules of PS3.5 / PS3.10, the Specific Character Set, header-to-pixel consistency and
 * its IOD's module / attribute requirements and Enumerated Values (tables generated from PS3.3).
 * Repair never changes the input file: it writes a NEW file, only for the categories asked for, and
 * logs every change. Reports never contain values from the data sets unless detailed_report is
 * set; such a report has "contains_phi": true. */
enum {
    SIAARA_REPAIR_STRUCTURAL = 1u << 0, /* file meta, group lengths, odd lengths, offset tables ... */
    SIAARA_REPAIR_VALUES = 1u << 1,     /* lossless VR re-encoding, unambiguous value normalisation */
    SIAARA_REPAIR_UIDS = 1u << 2,       /* invalid UIDs replaced by 2.25 UIDs, references kept */
    SIAARA_REPAIR_CHARSET = 1u << 3,    /* Specific Character Set detected or set to options->charset */
    SIAARA_REPAIR_ATTRIBUTES = 1u << 4, /* missing Type 2 added empty, required UIDs generated */
    SIAARA_REPAIR_PIXELS = 1u << 5,     /* image attributes made consistent with the pixel data */
    SIAARA_REPAIR_RESCUE = 1u << 6,     /* complete (decodable) frames of damaged pixel data kept */
    SIAARA_REPAIR_DECOMPRESS = 1u << 7, /* pixel data written native (Explicit VR Little Endian) */
    SIAARA_REPAIR_STUDY = 1u << 8       /* cross-file consistency (siaara_repair_file: no effect) */
};
typedef struct siaara_repair_options {
    uint32_t struct_size;            /* set to sizeof(siaara_repair_options) */
    uint32_t fix;                    /* SIAARA_REPAIR_* bits; 0 = report only */
    int32_t write;                   /* 0 (default) = dry run: changes are planned and reported only */
    int32_t detailed_report;         /* 1 = old/new values and paths in the report: it CONTAINS PHI */
    const char* charset;             /* SIAARA_REPAIR_CHARSET: value to set when detection is ambiguous; may be NULL */
    size_t charset_len;
    int32_t recompute_group_lengths; /* SIAARA_REPAIR_STRUCTURAL: recompute instead of dropping them */
    int32_t keep_last_duplicate;     /* duplicated elements: keep the last copy (default: the first) */
    int32_t overwrite;               /* replace an existing output file (never the input) */
    const char* uid_seed;            /* seed for generated UIDs (repeatable runs); NULL = random */
    size_t uid_seed_len;
} siaara_repair_options;

/* Validates a file; the findings are written as JSON (string buffer rules). The file is only read. */
SIAARA_API siaara_status SIAARA_CALL siaara_validate_json(const char* utf8_path, size_t path_len, char* buf,
                                                          size_t cap, size_t* needed);
/* Validates a Part 10 stream in memory (the bytes are only read). */
SIAARA_API siaara_status SIAARA_CALL siaara_validate_memory_json(const uint8_t* data, size_t size, char* buf,
                                                                 size_t cap, size_t* needed);
/* Validates and repairs input_path into the NEW file output_path (written only when options->write
 * is 1; output_path may be NULL for a dry run). output_path must not be the input. The JSON report
 * (findings, every change, rescued frames, validation of the output, input SHA-256 before/after) is
 * copied to buf under the string buffer rules; buf may be NULL with cap 0. The repair
 * is complete before the report is copied, so SIAARA_ERR_BUFFER_TOO_SMALL only means the report was
 * cut; *needed has its full length. A per-file failure (unreadable input, output exists ...) returns
 * an error after the report is copied. A struct_size smaller than this version's struct is accepted. */
SIAARA_API siaara_status SIAARA_CALL siaara_repair_file(const char* input_path, size_t input_len,
                                                        const char* output_path, size_t output_len,
                                                        const siaara_repair_options* options, char* buf,
                                                        size_t cap, size_t* needed);

/* ================================================================ named data layer (wip-named)
 * Every PS3.3 module and macro as data (tables generated by tools/gen_modules.py from the
 * standard), a path API reaching any attribute of any depth, values at a point, waveforms, and
 * a few standard-defined lookups. The toolkit returns data as stored, interpreted only where the
 * standard defines the meaning (rescale, Dose Grid Scaling, waveform channel sensitivity, PS3.3
 * "unchanged values are not repeated" for control points); it does no clinical calculation.
 * See docs/DATA_ACCESS.md. */

/* ---------------------------------------------------------------- modules and macros */
enum {
    SIAARA_MODULE_KIND_MODULE = 1,
    SIAARA_MODULE_KIND_MACRO = 2,
    SIAARA_MODULE_KIND_ITEM = 3,      /* the item of a sequence defined inside a table */
    SIAARA_MODULE_KIND_TABLE = 4,     /* another PS3.3 attribute table */
    SIAARA_MODULE_KIND_FUNCTIONAL_GROUPS = 5 /* every Functional Group Macro (Annex A tables) */
};
/* Attribute Type as PS3.3 gives it (0 when the table has no Type column). */
enum {
    SIAARA_ATTR_TYPE_NONE = 0, SIAARA_ATTR_TYPE_1 = 1, SIAARA_ATTR_TYPE_1C = 2, SIAARA_ATTR_TYPE_2 = 3,
    SIAARA_ATTR_TYPE_2C = 4, SIAARA_ATTR_TYPE_3 = 5
};
typedef struct siaara_module_info {
    uint32_t struct_size;            /* set to sizeof(siaara_module_info) */
    uint32_t index;
    int32_t kind;                    /* SIAARA_MODULE_KIND_* */
    uint32_t attribute_count;        /* after Include rows are expanded (siaara_module_attribute_get) */
    const char* name;                /* static: "RT Beams Module", "Code Sequence Macro", "... > XSequence item" */
    const char* table_id;            /* static: "C.8-50"; "" for items */
    const char* class_name;          /* static: generated C# class (Siaara.Modules) */
} siaara_module_info;
typedef struct siaara_module_attribute {
    uint32_t struct_size;            /* set to sizeof(siaara_module_attribute) */
    uint32_t tag;                    /* repeating groups (60xx): xx positions zero, see mask */
    uint32_t mask;
    int32_t type;                    /* SIAARA_ATTR_TYPE_* */
    int32_t item_module;             /* sequence: module index of its item (siaara_module_get), else -1 */
    int32_t retired;
    const char* keyword;             /* static, PS3.6 */
    const char* vr;                  /* static: "US", or "US/SS" when the VR depends on context */
    const char* vm;                  /* static: "1", "2-n", ... */
} siaara_module_attribute;
/* Number of modules, macros, other tables and sequence items (indices 0 .. count-1). Thread safety: any. */
SIAARA_API siaara_status SIAARA_CALL siaara_module_count(uint32_t* out);
SIAARA_API siaara_status SIAARA_CALL siaara_module_get(uint32_t index, siaara_module_info* out);
/* Finds a module by name, case-insensitively: "RT Beams", "RT Beams Module", "C.8-50", "table_C.8-50"
 * or the C# class name. SIAARA_ERR_NOT_FOUND when nothing matches. */
SIAARA_API siaara_status SIAARA_CALL siaara_module_find(const char* name, size_t len, uint32_t* out);
/* Attribute `index` (< attribute_count) of a module, Include rows expanded recursively (a table
 * already being expanded is skipped; the first definition of a tag wins). */
SIAARA_API siaara_status SIAARA_CALL siaara_module_attribute_get(uint32_t module, uint32_t index,
                                                             siaara_module_attribute* out);
/* *out = 1 when at least one attribute of the module is present at the top level of the file's
 * main data set (or of ds), else 0. Thread safety: as reading. */
SIAARA_API siaara_status SIAARA_CALL siaara_module_present(const siaara_file* file, const char* name, size_t len,
                                                           int32_t* out);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_module_present(const siaara_dataset* ds, const char* name,
                                                                   size_t len, int32_t* out);

/* ---------------------------------------------------------------- path API
 * path   := segment ('.' segment)*         e.g. "RTBeamSequence[0].ControlPointSequence[3].GantryAngle"
 * segment:= name ['[' index ']']           index is 0-based
 * name   := Keyword | (gggg,eeee) | gggg,eeee | creator:gggg,xx | "creator":gggg,xx
 * A sequence that is not the last segment needs an item index. On the last segment an index
 * selects an item of a sequence or one value of a multi-valued element. Private attributes are
 * found through their Private Creator element (gggg,00bb) in the data set being walked (an
 * unquoted creator cannot contain '.', '[' or ']'). Missing elements fail with
 * SIAARA_ERR_NOT_FOUND (the message names the segment); nothing is ever defaulted. */
enum {
    SIAARA_VALUE_EMPTY = 0,          /* present with an empty value (vm 0) */
    SIAARA_VALUE_TEXT = 1,
    SIAARA_VALUE_INTEGER = 2,        /* IS, US, SS, UL, SL, SV, UV */
    SIAARA_VALUE_REAL = 3,           /* DS, FL, FD */
    SIAARA_VALUE_BINARY = 4,         /* OB, OW, OF, OD, OL, OV, UN, encapsulated data: no text */
    SIAARA_VALUE_SEQUENCE = 5,       /* item_count items */
    SIAARA_VALUE_TAG = 6,            /* AT: integer = the tag, text "(gggg,eeee)" */
    SIAARA_VALUE_ITEM = 7            /* last segment selected an item (siaara_dataset_by_path) */
};
typedef struct siaara_path_value {
    uint32_t struct_size;            /* set to sizeof(siaara_path_value) */
    uint32_t tag;                    /* the element found (private tags: the resolved tag) */
    char vr[4];                      /* as stored, NUL-terminated */
    uint32_t vm;                     /* values (items for a sequence) */
    int32_t kind;                    /* SIAARA_VALUE_* */
    int32_t value_index;             /* index given on the last segment, -1 when none */
    uint32_t item_count;
    int32_t has_integer;             /* integer holds the selected (or first) value */
    int32_t has_real;                /* real holds the selected (or first) value */
    uint32_t reserved0;
    int64_t integer;
    double real;
    uint64_t byte_length;            /* value bytes as held */
} siaara_path_value;
/* Looks the path up from ds. The text form (all values backslash separated, or the selected value;
 * UTF-8, decoded with the Specific Character Set in force along the path; numbers as stored) is
 * copied to buf under the string buffer rules (buf may be NULL with cap 0). Thread safety: as reading. */
SIAARA_API siaara_status SIAARA_CALL siaara_get_by_path(const siaara_dataset* ds, const char* path, size_t len,
                                                        siaara_path_value* out, char* buf, size_t cap,
                                                        size_t* needed);
/* Every value of a numeric element (binary numbers, DS, IS, OW / OF / OD / OL / OV) as doubles; an
 * index on the last segment selects one. *count receives the number of values; with cap < count
 * nothing is copied and SIAARA_ERR_BUFFER_TOO_SMALL is returned (buf may be NULL with cap 0). */
SIAARA_API siaara_status SIAARA_CALL siaara_get_doubles_by_path(const siaara_dataset* ds, const char* path,
                                                                size_t len, double* buf, size_t cap,
                                                                size_t* count);
/* The item the path selects (last segment "Sequence[i]"), borrowed as siaara_dataset_item. */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_by_path(siaara_dataset* ds, const char* path, size_t len,
                                                            siaara_dataset** out);

/* ---------------------------------------------------------------- values at a point */
/* Where the units of a modality value come from. */
enum {
    SIAARA_UNITS_NONE = 0,           /* the data set does not say */
    SIAARA_UNITS_RESCALE_TYPE = 1,   /* Rescale Type (0028,1054) */
    SIAARA_UNITS_MODALITY_LUT_TYPE = 2, /* Modality LUT Type (0028,3004) */
    SIAARA_UNITS_DOSE_UNITS = 3,     /* RT Dose: Dose Units (3004,0002) */
    SIAARA_UNITS_CT_HU = 4,          /* CT without Rescale Type: HU (PS3.3 C.8.2.1, Rescale Type 1C) */
    SIAARA_UNITS_PET_UNITS = 5       /* PET: Units (0054,1001) (PS3.3 C.8.9.1) */
};
typedef struct siaara_pixel_value {
    uint32_t struct_size;            /* set to sizeof(siaara_pixel_value) */
    uint32_t frame, column, row, sample;
    int32_t sample_kind;             /* SIAARA_SAMPLES_INTEGER, _FLOAT32, _FLOAT64 */
    int64_t stored;                  /* integer pixel data: the stored value (Pixel Representation applied) */
    double stored_real;              /* the stored value as a double (float pixel data: the value) */
    int32_t has_modality;            /* 1: grayscale integer data, a Modality LUT / rescale / Dose Grid Scaling applies */
    int32_t modality_lut;            /* the value came from a Modality LUT Sequence */
    int32_t dose_grid_scaling;       /* RT Dose: the slope is Dose Grid Scaling */
    int32_t units_source;            /* SIAARA_UNITS_* */
    double modality_value;           /* stored * slope + intercept, or the LUT entry */
    double rescale_slope;            /* as applied (1 with a LUT) */
    double rescale_intercept;        /* as applied (0 with a LUT) */
    char units[32];                  /* NUL-terminated, as stored (truncated if longer) */
} siaara_pixel_value;
/* The stored value at (column, row) of a frame (0-based) and its modality value, from the stored
 * data with the standard's Modality LUT / Rescale (functional groups first) or, for RT Dose,
 * Dose Grid Scaling. sample: 0 for grayscale; colour samples have no modality value. This call
 * decodes the frame each time: for many points use siaara_frame_open + siaara_frame_pixel_value.
 * Out of range: SIAARA_ERR_INVALID_ARGUMENT. Thread safety: as reading. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_pixel_value(const siaara_file* file, uint32_t frame, uint32_t column,
                                                             uint32_t row, uint32_t sample, siaara_pixel_value* out);
/* As above, on a decoded frame. Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_frame_pixel_value(const siaara_frame* frame, uint32_t column, uint32_t row,
                                                              uint32_t sample, siaara_pixel_value* out);
/* Voxel (i, j, k) of a volume (modality values, as built). Out of range: SIAARA_ERR_INVALID_ARGUMENT. */
SIAARA_API siaara_status SIAARA_CALL siaara_volume_voxel(const siaara_volume* volume, uint32_t i, uint32_t j,
                                                         uint32_t k, double* out);
typedef struct siaara_volume_sample {
    uint32_t struct_size;            /* set to sizeof(siaara_volume_sample) */
    int32_t inside;                  /* the point lies in the volume (voxel footprints) */
    int32_t interpolation;           /* as used: SIAARA_INTERP_NEAREST or SIAARA_INTERP_LINEAR */
    int32_t reserved0;
    double patient[3];               /* the point asked for (mm, LPS) */
    double index[3];                 /* continuous voxel index */
    int64_t voxel[3];                /* nearest voxel index (may be outside the grid) */
    double value;                    /* inside only: the nearest voxel, or trilinear when asked */
} siaara_volume_sample;
/* Value at a patient coordinate (mm). interpolation: SIAARA_INTERP_NEAREST (default meaning: the
 * stored voxel) or SIAARA_INTERP_LINEAR (trilinear, only on request). Outside the volume the call
 * succeeds with inside = 0 and value 0. Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_volume_value_at(const siaara_volume* volume, const double patient[3],
                                                            int32_t interpolation, siaara_volume_sample* out);

/* ---------------------------------------------------------------- waveforms (PS3.3 C.10.9) */
/* Multiplex group `group` = item of Waveform Sequence (5400,0100). */
typedef struct siaara_waveform_group {
    uint32_t struct_size;            /* set to sizeof(siaara_waveform_group) */
    uint32_t channels;               /* Number of Waveform Channels (003A,0005) */
    uint32_t samples;                /* Number of Waveform Samples (003A,0010) */
    uint32_t bits_allocated;         /* Waveform Bits Allocated (5400,1004) */
    double sampling_frequency;       /* Hz (003A,001A) */
    char sample_interpretation[4];   /* (5400,1006): SB UB MB AB SS US SL UL SV UV */
    int32_t has_padding;
    int64_t padding_value;           /* Waveform Padding Value (5400,100A), as a stored sample */
    int32_t has_time_offset;
    int32_t has_trigger_time_offset;
    double time_offset;              /* Multiplex Group Time Offset (0018,1068), ms */
    double trigger_time_offset;      /* Trigger Time Offset (0018,1069), ms */
    uint32_t data_samples;           /* samples per channel the Waveform Data actually holds */
    uint32_t reserved0;
} siaara_waveform_group;
SIAARA_API siaara_status SIAARA_CALL siaara_waveform_group_count(const siaara_dataset* ds, uint32_t* out);
SIAARA_API siaara_status SIAARA_CALL siaara_waveform_group_info(const siaara_dataset* ds, uint32_t group,
                                                                siaara_waveform_group* out);
typedef struct siaara_waveform_channel {
    uint32_t struct_size;            /* set to sizeof(siaara_waveform_channel) */
    uint32_t bits_stored;            /* Waveform Bits Stored (003A,021A) */
    int32_t has_channel_number;
    int32_t channel_number;          /* Waveform Channel Number (003A,0202) */
    int32_t has_sensitivity;         /* Channel Sensitivity (003A,0210) present */
    int32_t has_correction;          /* Channel Sensitivity Correction Factor (003A,0212) present */
    int32_t has_baseline;            /* Channel Baseline (003A,0213) present */
    int32_t has_time_skew;
    double sensitivity;
    double correction;               /* 1 when absent */
    double baseline;                 /* 0 when absent */
    double time_skew;                /* Channel Time Skew (003A,0214), s */
    int32_t has_sample_skew;
    int32_t has_offset;
    double sample_skew;              /* Channel Sample Skew (003A,0215), samples */
    double offset;                   /* Channel Offset (003A,0218), ms */
    int32_t has_filter_low, has_filter_high, has_notch, reserved0;
    double filter_low;               /* Filter Low Frequency (003A,0220), Hz */
    double filter_high;              /* Filter High Frequency (003A,0221), Hz */
    double notch;                    /* Notch Filter Frequency (003A,0222), Hz */
} siaara_waveform_channel;
SIAARA_API siaara_status SIAARA_CALL siaara_waveform_channel_info(const siaara_dataset* ds, uint32_t group,
                                                                  uint32_t channel, siaara_waveform_channel* out);
/* Text of a channel (UTF-8, string buffer rules); SIAARA_ERR_NOT_FOUND when absent. */
enum {
    SIAARA_WAVEFORM_LABEL = 0,           /* Channel Label (003A,0203) */
    SIAARA_WAVEFORM_STATUS = 1,          /* Channel Status (003A,0205), values backslash separated */
    SIAARA_WAVEFORM_UNITS_CODE = 2,      /* Channel Sensitivity Units Sequence > Code Value (UCUM) */
    SIAARA_WAVEFORM_UNITS_MEANING = 3,   /* ... > Code Meaning */
    SIAARA_WAVEFORM_SOURCE_CODE = 4,     /* Channel Source Sequence > Code Value */
    SIAARA_WAVEFORM_SOURCE_SCHEME = 5,   /* ... > Coding Scheme Designator */
    SIAARA_WAVEFORM_SOURCE_MEANING = 6,  /* ... > Code Meaning */
    SIAARA_WAVEFORM_GROUP_LABEL = 7      /* Multiplex Group Label (003A,0020) of the group (channel ignored) */
};
SIAARA_API siaara_status SIAARA_CALL siaara_waveform_channel_string(const siaara_dataset* ds, uint32_t group,
                                                                    uint32_t channel, int32_t which, char* buf,
                                                                    size_t cap, size_t* needed);
/* The channel's samples (data_samples of them) de-interleaved from Waveform Data. Raw: the stored
 * values (sign-extended per C.10.9.1.7; MB / AB: the 8-bit codes). scaled = 0: raw as doubles;
 * scaled = 1: (sample x Channel Sensitivity x Correction Factor) + Channel Baseline, in Channel
 * Sensitivity Units (MB / AB codes are first expanded per ITU-T G.711), and NaN for samples equal
 * to the Waveform Padding Value. scaled = 1 fails with SIAARA_ERR_NOT_FOUND when the channel has
 * no Channel Sensitivity (its samples are in arbitrary units). Buffer rules as
 * siaara_get_doubles_by_path. Thread safety: as reading. */
SIAARA_API siaara_status SIAARA_CALL siaara_waveform_samples(const siaara_dataset* ds, uint32_t group, uint32_t channel,
                                                             int32_t scaled, double* buf, size_t cap, size_t* count);
/* Raw stored samples as 64-bit integers (UV values above INT64_MAX wrap). */
SIAARA_API siaara_status SIAARA_CALL siaara_waveform_raw_samples(const siaara_dataset* ds, uint32_t group,
                                                                 uint32_t channel, int64_t* buf, size_t cap,
                                                                 size_t* count);

/* ---------------------------------------------------------------- RT control points
 * PS3.3 C.8.8.14.5 / C.8.8.25.6: control points after the first hold only the values that change,
 * so the value in force at control point k is the one of the nearest item at or before k that has
 * it. beam is an item of RT Beam Sequence (Control Point Sequence 300A,0111), Ion Beam Sequence
 * (Ion Control Point Sequence 300A,03A8) or a treatment record's Treatment Session (Ion) Beam
 * Sequence (Control Point Delivery Sequence 3008,0040 / Ion ... 3008,0041). */
/* *out_cp = index of the control point item that holds the value of `tag` in force at `cp`.
 * SIAARA_ERR_NOT_FOUND when no item at or before cp has it. */
SIAARA_API siaara_status SIAARA_CALL siaara_rt_control_point_source(const siaara_dataset* beam, uint32_t cp,
                                                                    uint32_t tag, uint32_t* out_cp);
/* Leaf / Jaw Positions (300A,011C) of the Beam Limiting Device Position Sequence item whose RT Beam
 * Limiting Device Type (300A,00B8) equals device_type ("MLCX", "ASYMY", ...). effective = 0: only
 * control point cp itself; effective = 1: the nearest item at or before cp that has that device.
 * *out_cp (may be NULL) receives the control point the values come from. Buffer rules as
 * siaara_get_doubles_by_path. */
SIAARA_API siaara_status SIAARA_CALL siaara_rt_device_positions(const siaara_dataset* beam, uint32_t cp,
                                                                const char* device_type, size_t len,
                                                                int32_t effective, double* buf, size_t cap,
                                                                size_t* count, uint32_t* out_cp);

/* ---------------------------------------------------------------- NM multi-frame index vectors */
/* Frame k of an NM image (PS3.3 C.8.4.8 / C.8.4.9): the values of the index vectors at k (1-based
 * values as stored, 0 when that vector is absent or too short). in_pointer bit n is set when the
 * vector is listed in Frame Increment Pointer (0028,0009); present bit n when it is in the data set. */
enum {
    SIAARA_NM_ENERGY_WINDOW = 1 << 0,  /* Energy Window Vector (0054,0010) */
    SIAARA_NM_DETECTOR = 1 << 1,       /* Detector Vector (0054,0020) */
    SIAARA_NM_PHASE = 1 << 2,          /* Phase Vector (0054,0030) */
    SIAARA_NM_ROTATION = 1 << 3,       /* Rotation Vector (0054,0050) */
    SIAARA_NM_RR_INTERVAL = 1 << 4,    /* R-R Interval Vector (0054,0060) */
    SIAARA_NM_TIME_SLOT = 1 << 5,      /* Time Slot Vector (0054,0070) */
    SIAARA_NM_SLICE = 1 << 6,          /* Slice Vector (0054,0080) */
    SIAARA_NM_ANGULAR_VIEW = 1 << 7,   /* Angular View Vector (0054,0090) */
    SIAARA_NM_TIME_SLICE = 1 << 8      /* Time Slice Vector (0054,0100) */
};
typedef struct siaara_nm_frame_index {
    uint32_t struct_size;            /* set to sizeof(siaara_nm_frame_index) */
    uint32_t frame;                  /* 0-based */
    uint32_t present;                /* SIAARA_NM_* bits */
    uint32_t in_pointer;             /* SIAARA_NM_* bits */
    uint32_t energy_window, detector, phase, rotation, rr_interval, time_slot, slice, angular_view, time_slice;
    uint32_t reserved0;
} siaara_nm_frame_index;
SIAARA_API siaara_status SIAARA_CALL siaara_nm_frame_index_get(const siaara_dataset* ds, uint32_t frame,
                                                               siaara_nm_frame_index* out);

/* ---------------------------------------------------------------- SOP classes, transfer syntaxes, dictionary */
enum {
    SIAARA_OBJECT_OTHER = 0,
    SIAARA_OBJECT_IMAGE = 1,
    SIAARA_OBJECT_RT_PLAN = 2,
    SIAARA_OBJECT_RT_DOSE = 3,
    SIAARA_OBJECT_RT_STRUCTURE_SET = 4,
    SIAARA_OBJECT_RT_IMAGE = 5,
    SIAARA_OBJECT_RT_TREATMENT_RECORD = 6,
    SIAARA_OBJECT_STRUCTURED_REPORT = 7,
    SIAARA_OBJECT_SEGMENTATION = 8,
    SIAARA_OBJECT_WAVEFORM = 9,
    SIAARA_OBJECT_PRESENTATION_STATE = 10,
    SIAARA_OBJECT_ENCAPSULATED_DOCUMENT = 11,
    SIAARA_OBJECT_REGISTRATION = 12,
    SIAARA_OBJECT_RAW_DATA = 13,
    SIAARA_OBJECT_SPECTROSCOPY = 14,
    SIAARA_OBJECT_SURFACE = 15
};
typedef struct siaara_sop_class_info {
    uint32_t struct_size;            /* set to sizeof(siaara_sop_class_info) */
    int32_t object_kind;             /* SIAARA_OBJECT_*, from the IOD's modules (PS3.3 Annex A) */
    int32_t retired;
    int32_t has_iod;                 /* PS3.4 B.5-1 names its IOD */
    const char* name;                /* static, PS3.6 Table A-1 */
    const char* keyword;             /* static */
    const char* iod_name;            /* static, e.g. "RT Plan"; "" when has_iod is 0 */
    const char* iod_table_id;        /* static, e.g. "A.20-1" */
} siaara_sop_class_info;
/* SIAARA_ERR_NOT_FOUND for UIDs that are not SOP Classes of PS3.6. */
SIAARA_API siaara_status SIAARA_CALL siaara_sop_class_info_get(const char* uid, size_t len, siaara_sop_class_info* out);
typedef struct siaara_transfer_syntax_info {
    uint32_t struct_size;            /* set to sizeof(siaara_transfer_syntax_info) */
    int32_t explicit_vr;
    int32_t big_endian;
    int32_t encapsulated;            /* pixel data compressed (or referenced) */
    int32_t lossy;
    int32_t deflated;
    int32_t retired;
    int32_t reserved0;
    const char* name;                /* static */
} siaara_transfer_syntax_info;
SIAARA_API siaara_status SIAARA_CALL siaara_transfer_syntax_info_get(const char* uid, size_t len,
                                                                     siaara_transfer_syntax_info* out);
/* Dictionary enumeration (PS3.6 / PS3.7 public elements; repeating groups once, xx = 00). */
SIAARA_API siaara_status SIAARA_CALL siaara_dict_count(uint32_t* out);
SIAARA_API siaara_status SIAARA_CALL siaara_dict_entry_at(uint32_t index, siaara_dict_entry* out);
enum {
    SIAARA_DICT_MATCH_NAME = 1 << 0,     /* text is a case-insensitive substring of the name */
    SIAARA_DICT_MATCH_KEYWORD = 1 << 1,  /* ... of the keyword */
    SIAARA_DICT_INCLUDE_RETIRED = 1 << 2
};
/* Indices (for siaara_dict_entry_at) of the entries that match: text (may be empty: any) per
 * flags, and vr (two characters, may be NULL: any VR; matches the first or the alternative VR).
 * *count receives the number of matches; at most cap indices are written (buf may be NULL). */
SIAARA_API siaara_status SIAARA_CALL siaara_dict_search(const char* text, size_t len, const char* vr, int32_t flags,
                                                        uint32_t* buf, size_t cap, size_t* count);

/* ================================================================ networking: full service set (RIS / PACS)
 * Everything below extends the DIMSE and DICOMweb sections above; docs/NETWORKING.md is the
 * integration guide. */

/* ---------------------------------------------------------------- SCP: all services on one port */
enum {
    SIAARA_SCP_FIND = 3,             /* C-FIND answered from the store: completed = matches, dataset = identifier */
    SIAARA_SCP_MOVE = 4,             /* C-MOVE finished: destination_ae, completed / failed / warning */
    SIAARA_SCP_GET = 5,              /* C-GET finished: completed / failed / warning */
    SIAARA_SCP_WORKLIST = 6,         /* Modality Worklist C-FIND answered: completed = items sent */
    SIAARA_SCP_MPPS_CREATE = 7,      /* MPPS N-CREATE: sop_instance_uid, dataset = the attributes */
    SIAARA_SCP_MPPS_SET = 8,         /* MPPS N-SET: sop_instance_uid, dataset = the modifications */
    SIAARA_SCP_COMMIT_REQUEST = 9,   /* Storage Commitment request decided: transaction_uid, completed = committed,
                                        failed; outcome 1 when the report was delivered, 0 when not */
    SIAARA_SCP_COMMIT_REPORT = 10,   /* Storage Commitment report received (SCU role, siaara_dimse_commit_ex) */
    SIAARA_SCP_UPS = 11,             /* UPS request served: command_field, sop_instance_uid, dimse_status */
    SIAARA_SCP_REFUSED = 12,         /* a request refused by a service allow-list or limit: command_field, sop_class_uid */
    SIAARA_SCP_UID_CHANGED = 13,     /* a C-GET / C-MOVE sub-operation needed lossy compression, which gave the
                                        instance a new SOP Instance UID: original_sop_instance_uid -> sop_instance_uid */
    SIAARA_SCP_IAN = 14              /* Instance Availability Notification received: dataset */
};

/* Services, for siaara_scp_acl. */
enum {
    SIAARA_SVC_VERIFICATION = 0, SIAARA_SVC_STORAGE = 1, SIAARA_SVC_FIND = 2, SIAARA_SVC_MOVE = 3,
    SIAARA_SVC_GET = 4, SIAARA_SVC_WORKLIST = 5, SIAARA_SVC_MPPS = 6, SIAARA_SVC_COMMITMENT = 7,
    SIAARA_SVC_UPS = 8, SIAARA_SVC_IAN = 9
};

/* A remote application entity the SCP opens associations to: C-MOVE destinations, receivers of
 * Storage Commitment reports sent on a new association, UPS event subscribers. */
typedef struct siaara_ae_entry {
    uint32_t struct_size;            /* sizeof(siaara_ae_entry) */
    uint32_t port;                   /* 1..65535 */
    const char* ae_title;
    const char* host;                /* name or address */
    int32_t tls;                     /* 0/1: associations to it use TLS (siaara_scp_services.outbound_tls). With 1
                                        and no outbound_tls the default client settings apply (system trust
                                        store, host name check): a peer with a private certificate is then
                                        refused, never contacted in plain text */
    uint32_t reserved0;              /* set to 0 */
} siaara_ae_entry;

/* A per-service allow-list: only these calling AE titles may use the service (each must also be
 * accepted by siaara_scp_options.allowed_calling_aes). A service without an entry is open to
 * every accepted peer. A refused request is answered 0x0124 (Refused: Not Authorized), for
 * C-STORE 0xA700 is not used for it, and an event SIAARA_SCP_REFUSED is raised. Verification
 * may be listed too. */
typedef struct siaara_scp_acl {
    uint32_t struct_size;            /* sizeof(siaara_scp_acl) */
    int32_t service;                 /* SIAARA_SVC_* */
    const char* const* calling_aes;
    uint32_t count;                  /* 0: nobody may use the service */
    uint32_t reserved0;
} siaara_scp_acl;

/* Modality Worklist SCP fed by the application. For each C-FIND the toolkit calls the callback
 * (on a helper thread, while the association's thread waits for it up to
 * siaara_scp_services.worklist_timeout_ms without progress; several associations may call it at
 * once; worklist_user must stay valid until every call has returned) with the identifier;
 * the callback passes candidate scheduled procedure steps to emit() - every one, or a
 * pre-filtered set - and returns the final status (0 = Success). The toolkit applies the C-FIND
 * matching rules (PS3.4 C.2.2.2 and K.6, including Scheduled Procedure Step Sequence matching)
 * and returns only the requested keys, so emitting the whole list is correct. emit() returns 0
 * to continue, non-zero once the requestor cancelled or the association failed (stop then). The
 * item is only read during emit(). */
typedef int32_t (SIAARA_CALL* siaara_worklist_emit)(void* emit_ctx, const siaara_dataset* item);
typedef uint32_t (SIAARA_CALL* siaara_worklist_callback)(void* user, const char* calling_ae,
                                                          const siaara_dataset* identifier,
                                                          siaara_worklist_emit emit, void* emit_ctx);

enum {
    SIAARA_COMMIT_REPORT_SAME_THEN_NEW = 0, /* on the requestor's association; a new one when it is gone */
    SIAARA_COMMIT_REPORT_SAME = 1,          /* only on the requestor's association */
    SIAARA_COMMIT_REPORT_NEW = 2            /* always on a new association to the requestor (ae_table) */
};

typedef struct siaara_scp_services {
    uint32_t struct_size;            /* set to sizeof(siaara_scp_services) */
    /* ---- which services run (Verification always) */
    int32_t storage;                 /* 0/1: Storage SCP into the store */
    int32_t find;                    /* 0/1: C-FIND, Patient and Study Root, every level, from the store */
    int32_t move;                    /* 0/1: C-MOVE from the store to ae_table destinations */
    int32_t get;                     /* 0/1: C-GET from the store */
    int32_t worklist;                /* 0/1: Modality Worklist (worklist_cb, else siaara_scp_set_worklist) */
    int32_t mpps;                    /* 0/1: MPPS SCP (N-CREATE / N-SET; F.7.2.2 state rules; siaara_scp_mpps_get) */
    int32_t commitment;              /* 0/1: Storage Commitment Push Model SCP, decided from the store. Every AE
                                        allowed to use it learns which SOP Instance UIDs the store holds:
                                        limit it with an allow-list (acls) */
    int32_t commitment_report_mode;  /* SIAARA_COMMIT_REPORT_* */
    int32_t commitment_reports;      /* 0/1: receive Storage Commitment reports sent on new associations (the
                                        SCU role; siaara_dimse_commit_ex with this SCP as receiver) */
    int32_t ups;                     /* 0/1: UPS Push / Pull / Watch / Query SCP (events to subscribers; in memory,
                                        or kept in state_folder). DIMSE-N commands may name the UPS Push SOP
                                        Class (PS3.4 CC.2) or the negotiated one */
    int32_t instance_availability;   /* 0/1: Instance Availability Notification SCP (events only) */
    int32_t allow_lossy;             /* 0/1: store_transfer_syntax and C-GET / C-MOVE transcodes may be lossy */
    /* ---- storage */
    const char* const* storage_sop_classes; /* NULL: the toolkit's storage list (or any with accept_any_sop_class) */
    uint32_t storage_sop_class_count;
    uint32_t transfer_syntax_count;
    const char* const* transfer_syntaxes;   /* accepted for storage, in preference order. NULL: native, plus the
                                               compressed ones with siaara_scp_options.accept_compressed */
    const char* store_transfer_syntax;      /* NULL: keep as received; else convert before storing */
    /* ---- query / retrieve */
    const siaara_ae_entry* ae_table; /* C-MOVE destinations, commitment and UPS report receivers */
    uint32_t ae_count;
    uint32_t max_find_results;       /* 0 = 10000; past it the C-FIND ends with 0xA700 after the matches sent */
    const siaara_tls_options* outbound_tls; /* TLS client settings for ae_table entries with tls = 1 */
    uint32_t max_retrieve_instances; /* 0 = 65535; more: 0xA701 without sub-operations */
    int32_t relational;              /* 0/1: accept relational queries and retrieval (extended negotiation) */
    int32_t fuzzy_person_names;      /* 0/1: accept fuzzy semantic matching of person names */
    int32_t combined_datetime;       /* 0/1: accept combined date-time matching */
    /* ---- worklist */
    siaara_worklist_callback worklist_cb;   /* NULL: the list of siaara_scp_set_worklist */
    void* worklist_user;
    uint32_t max_worklist_items;     /* 0 = 10000 */
    /* ---- MPPS, commitment */
    uint32_t max_mpps_instances;     /* IN PROGRESS instances; 0 = 10000; more N-CREATEs get 0xA700 (finished
                                        ones: max_finished_instances) */
    uint32_t max_commitment_items;   /* per request; 0 = 10000; larger requests fail every item (Failure Reason
                                        0x0213 Resource Limitation) */
    /* ---- allow-lists */
    uint32_t acl_count;
    const siaara_scp_acl* acls;
    /* ---- appended; read only when struct_size includes them */
    int32_t allow_incomplete;        /* 0 (default): a C-GET / C-MOVE sub-operation for a stored file whose read
                                        was not complete (truncated input, skipped bytes, heuristic recovery;
                                        siaara_file_read_report) is refused: that sub-operation fails and is
                                        logged, the peer never gets less than the file holds. 1: send what was
                                        read (as siaara_dimse_peer.allow_incomplete) */
    /* ---- appended (security review 4); read only when struct_size includes them */
    uint32_t max_service_dataset_bytes; /* largest N-service data set or Q/R identifier accepted, enforced while
                                        it arrives; 0 = 4 MiB. Larger: the association is aborted (R-109) */
    uint32_t max_ups_items;          /* UPS workitems not yet COMPLETED / CANCELED; 0 = 10000; more: 0xA700 */
    uint64_t max_state_bytes;        /* data sets held by the MPPS and by the UPS model, each; 0 = 256 MiB.
                                        Finished instances are evicted oldest first to make room; then 0xA700 */
    uint32_t max_finished_instances; /* COMPLETED / DISCONTINUED MPPS instances and final UPS workitems kept;
                                        0 = 10000; the oldest are evicted first (R-119). max_mpps_instances
                                        counts only IN PROGRESS instances */
    uint32_t max_ups_subscribers;    /* receiving AEs per workitem; 0 = 16; more: 0xA700 (R-117) */
    const char* state_folder;        /* NULL: MPPS instances and UPS workitems live in memory only. Else a folder
                                        (created owner-only) where each is kept in an owner-only file, written
                                        atomically, and reloaded by the next siaara_scp_start_ex */
    int32_t bind_updates_to_creator; /* 0 (default) / 1: MPPS N-SET and N-SET of a SCHEDULED UPS only from the
                                        AE that created the instance; others get 0x0124 (R-131) */
    int32_t worklist_timeout_ms;     /* worklist_cb must return within this; 0 = 30000. Past it the C-FIND ends
                                        with 0x0110 and the association goes on (the callback keeps running on
                                        a helper thread; a second query waits for it) (R-130) */
    const struct siaara_move_acl* move_acls; /* C-MOVE destinations per calling AE (R-129); NULL: any ae_table entry */
    uint32_t move_acl_count;
    uint32_t reserved0;              /* set to 0 */
} siaara_scp_services;

/* The destinations one calling AE may name in C-MOVE. A calling AE without an entry may name any
 * ae_table entry; a destination outside the list is answered 0xA801. */
typedef struct siaara_move_acl {
    uint32_t struct_size;            /* sizeof(siaara_move_acl) */
    uint32_t count;                  /* 0: this AE may not move anywhere */
    const char* calling_ae;
    const char* const* destination_aes;
} siaara_move_acl;

/* Starts an SCP with the services enabled in `services` on one port (Verification always), with
 * every limit and check of siaara_scp_start (allowed peers, per-peer limits, lifetime, spool,
 * UID checks). store holds what Storage receives and what Q/R, Storage Commitment and the
 * DICOMweb server answer from; it must outlive the SCP. services may be NULL: Storage only (as
 * siaara_scp_start). The returned handle works with siaara_scp_port / stats / stop / free.
 * Thread safety: as siaara_scp_start. */
SIAARA_API siaara_status SIAARA_CALL siaara_scp_start_ex(const siaara_scp_options* options,
                                                         const siaara_scp_services* services, siaara_store* store,
                                                         siaara_scp_callback cb, void* user, siaara_scp** out);
/* Replaces the worklist served when no worklist_cb is set: `count` data sets (copied), each one
 * scheduled procedure step with its patient / visit / request attributes and a Scheduled
 * Procedure Step Sequence. Thread safety: any thread; running queries see the old or new list. */
SIAARA_API siaara_status SIAARA_CALL siaara_scp_set_worklist(siaara_scp* scp, const siaara_dataset* const* items,
                                                             uint32_t count);
/* As above from a DICOM JSON array (PS3.18 F.2) of such data sets. */
SIAARA_API siaara_status SIAARA_CALL siaara_scp_set_worklist_json(siaara_scp* scp, const char* json, size_t len);
/* The current state of an MPPS instance received by the SCP (created attributes with every N-SET
 * applied). SIAARA_ERR_NOT_FOUND when unknown. Caller owns *out. Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_scp_mpps_get(const siaara_scp* scp, const char* sop_instance_uid,
                                                         siaara_file** out);
/* Schedules a UPS workitem locally (as a UPS Push N-CREATE would; Procedure Step State SCHEDULED).
 * uid NULL: a new UID, copied to buf (string buffer rules; buf may be NULL with cap 0). */
SIAARA_API siaara_status SIAARA_CALL siaara_scp_ups_create(siaara_scp* scp, const char* uid,
                                                           const siaara_dataset* attributes, char* buf, size_t cap,
                                                           size_t* needed);
/* The current state of a UPS workitem. Caller owns *out. */
SIAARA_API siaara_status SIAARA_CALL siaara_scp_ups_get(const siaara_scp* scp, const char* uid, siaara_file** out);

/* ---------------------------------------------------------------- SCU additions */
/* Q/R levels (SIAARA_LEVEL_* values): PATIENT, STUDY, SERIES, IMAGE. */
typedef struct siaara_qr_keys {
    uint32_t struct_size;            /* sizeof(siaara_qr_keys) */
    int32_t return_keys;             /* 0/1: also add the usual return keys of the level (empty, universal) */
    /* Matching values (NULL or "": universal / return key only). Wildcards and ranges as in C-FIND. */
    const char* patient_id;
    const char* patient_name;
    const char* patient_birth_date;
    const char* patient_sex;
    const char* study_instance_uid;  /* may list several UIDs separated by backslashes */
    const char* study_date;
    const char* study_time;
    const char* accession_number;
    const char* study_id;
    const char* study_description;
    const char* referring_physician;
    const char* modalities_in_study;
    const char* series_instance_uid;
    const char* modality;
    const char* series_number;
    const char* series_description;
    const char* sop_instance_uid;
    const char* sop_class_uid;
    const char* instance_number;
} siaara_qr_keys;

/* A Query/Retrieve identifier: Query/Retrieve Level for `level` (SIAARA_LEVEL_*; INSTANCE is
 * "IMAGE"), the given keys, Specific Character Set ISO_IR 192 (UTF-8), and with return_keys the
 * level's usual return keys. Use it with siaara_dimse_find / move / get (and the _ex forms);
 * add more keys with the data set functions. Caller owns *out. Thread safety: any thread. */
SIAARA_API siaara_status SIAARA_CALL siaara_qr_identifier(int32_t level, const siaara_qr_keys* keys,
                                                          siaara_file** out);

/* SOP Class Extended Negotiation for Query/Retrieve (PS3.4 C.5). */
enum {
    SIAARA_QR_RELATIONAL = 1,        /* relational queries (C-FIND) / relational retrieval (C-MOVE, C-GET) */
    SIAARA_QR_COMBINED_DATETIME = 2, /* C-FIND only */
    SIAARA_QR_FUZZY_NAMES = 4,       /* C-FIND only: fuzzy semantic matching of person names */
    SIAARA_QR_TIMEZONE = 8           /* C-FIND only: timezone query adjustment */
};
enum { SIAARA_QR_UPS = 3 };          /* model for siaara_dimse_find(_ex): UPS Query (C-FIND of UPS workitems) */

typedef struct siaara_qr_options {
    uint32_t struct_size;            /* sizeof(siaara_qr_options) */
    uint32_t flags;                  /* SIAARA_QR_* proposed */
    uint32_t accepted_flags;         /* out: what the peer accepted */
    uint32_t max_matches;            /* C-FIND: send C-CANCEL after this many (0 = all); not an error */
} siaara_qr_options;

/* As siaara_dimse_find, proposing extended negotiation; options may be NULL. options->accepted_flags
 * is written. With max_matches reached the call returns SIAARA_OK. */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_find_ex(const siaara_dimse_peer* peer, int32_t model,
                                                          const siaara_dataset* identifier, siaara_qr_options* options,
                                                          siaara_cancel* cancel, siaara_find_callback cb, void* user,
                                                          siaara_dimse_result* result);
/* As siaara_dimse_move / siaara_dimse_get with SIAARA_QR_RELATIONAL (options may be NULL). */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_move_ex(const siaara_dimse_peer* peer, int32_t model,
                                                          const siaara_dataset* identifier, const char* destination_ae,
                                                          siaara_qr_options* options, siaara_cancel* cancel,
                                                          siaara_dimse_progress_callback cb, void* user,
                                                          siaara_dimse_result* result);
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_get_ex(const siaara_dimse_peer* peer, int32_t model,
                                                         const siaara_dataset* identifier,
                                                         const siaara_get_options* get_options,
                                                         siaara_qr_options* options, siaara_cancel* cancel,
                                                         siaara_dimse_progress_callback cb, void* user,
                                                         siaara_dimse_result* result);

/* A C-STORE (or C-GET / C-MOVE sub-operation) needed lossy compression, which gave the instance a
 * new SOP Instance UID (R-093). index: the file's position in the call. Called on the calling
 * thread; strings valid during the call. */
typedef void (SIAARA_CALL* siaara_uid_change_callback)(void* user, uint32_t index, const char* original_uid,
                                                       const char* new_uid);
/* As siaara_dimse_store_files / siaara_dimse_store, reporting UID changes (uid_cb may be NULL). The
 * command, the data set and the peer all see the new UID; the source files are unchanged. */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_store_files_ex(const siaara_dimse_peer* peer, const char* const* paths,
                                                                 const size_t* lens, uint32_t count,
                                                                 siaara_cancel* cancel, siaara_dimse_progress_callback cb,
                                                                 siaara_uid_change_callback uid_cb, void* user,
                                                                 siaara_dimse_result* result);
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_store_ex(const siaara_dimse_peer* peer, const siaara_file* file,
                                                           siaara_cancel* cancel, siaara_uid_change_callback uid_cb,
                                                           void* user, siaara_dimse_result* result);

/* What happened to one instance of a C-STORE call (siaara_dimse_store_files_report /
 * siaara_dimse_store_report). The UIDs are what the peer received, so sop_class_uid /
 * sop_instance_uid can be passed to siaara_dimse_commit(_ex) as they are. */
enum {
    SIAARA_STORE_SENT = 1,              /* a C-STORE request was sent; dimse_status is the peer's answer */
    SIAARA_STORE_UID_CHANGED = 2,       /* lossy compression gave the instance a new SOP Instance UID (R-093) */
    SIAARA_STORE_LOSSY = 4,             /* this transfer applied lossy compression */
    SIAARA_STORE_INCOMPLETE_INPUT = 8,  /* not sent: the file was not read completely (allow_incomplete = 0) */
    SIAARA_STORE_LOSSY_METHOD_FILLED = 16,      /* the lossy transcode on the way filled in a missing earlier
                                                   Lossy Image Compression Method value proven by the file's
                                                   own transfer syntax (SIAARA_LOSSY_HISTORY_METHOD_FILLED) */
    SIAARA_STORE_LOSSY_HISTORY_MISALIGNED = 32  /* the file's Ratio / Method lists did not match and could not
                                                   be aligned: Ratio appended, Method left as it was
                                                   (SIAARA_LOSSY_HISTORY_MISALIGNED) */
};
typedef struct siaara_store_outcome {
    uint32_t struct_size;            /* set to sizeof(siaara_store_outcome) in every element */
    int32_t error;                   /* SIAARA_OK when sent; else why the instance was not sent
                                        (SIAARA_ERR_REJECTED for incomplete input, SIAARA_ERR_UNSUPPORTED when
                                        no accepted context / transcode fits, SIAARA_ERR_IO ...) */
    uint32_t dimse_status;           /* the peer's C-STORE status; 0xFFFF when not sent */
    uint32_t flags;                  /* SIAARA_STORE_* */
    char sop_class_uid[65];          /* NUL-terminated; "" when the file could not be read */
    char sop_instance_uid[65];       /* as sent (the new UID after a lossy transcode), else the file's */
    char original_sop_instance_uid[65]; /* the file's own SOP Instance UID */
    char transfer_syntax[65];        /* of the presentation context used; "" when not sent */
} siaara_store_outcome;

/* As siaara_dimse_store_files, writing outcomes[i] (count elements, may be NULL) for file i. Files
 * that were not attempted (cancel, a lost association) keep error SIAARA_ERR_CANCELLED or
 * SIAARA_ERR_NETWORK and dimse_status 0xFFFF. */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_store_files_report(const siaara_dimse_peer* peer,
                                                                     const char* const* paths, const size_t* lens,
                                                                     uint32_t count, siaara_cancel* cancel,
                                                                     siaara_dimse_progress_callback cb, void* user,
                                                                     siaara_store_outcome* outcomes,
                                                                     siaara_dimse_result* result);
/* As siaara_dimse_store, writing *outcome (may be NULL). */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_store_report(const siaara_dimse_peer* peer, const siaara_file* file,
                                                               siaara_cancel* cancel, siaara_store_outcome* outcome,
                                                               siaara_dimse_result* result);

/* Modality Performed Procedure Step SCU (PS3.4 F.7). N-CREATE with attributes (Performed
 * Procedure Step Status IN PROGRESS ...): uid NULL creates a new SOP Instance UID; the UID used
 * is copied to buf (string buffer rules; buf may be NULL with cap 0). result->status is the
 * DIMSE status. */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_mpps_create(const siaara_dimse_peer* peer, const char* uid,
                                                              const siaara_dataset* attributes, siaara_cancel* cancel,
                                                              char* buf, size_t cap, size_t* needed,
                                                              siaara_dimse_result* result);
/* N-SET of an MPPS instance (e.g. COMPLETED with the Performed Series Sequence). */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_mpps_set(const siaara_dimse_peer* peer, const char* uid,
                                                           const siaara_dataset* modifications, siaara_cancel* cancel,
                                                           siaara_dimse_result* result);

/* Storage Commitment with the report on the same or a new association (PS3.4 J.3.3). */
typedef struct siaara_commit_options {
    uint32_t struct_size;            /* sizeof(siaara_commit_options) */
    int32_t wait_ms;                 /* total wait for the report; 0 = 60000 */
    siaara_scp* receiver;            /* a running SCP started with commitment_reports = 1 that the peer reaches
                                        (its ae_table entry for us); NULL: same association only */
    int32_t same_association_ms;     /* with a receiver: how long the association stays open for a report on it
                                        before it is released (0 = 2000; -1 = release at once) */
    const char* transaction_uid;     /* NULL: a new one */
    char* transaction_uid_out;       /* receives the Transaction UID (65 bytes); may be NULL */
} siaara_commit_options;

/* Requests commitment of `count` instances and waits for the report, which may arrive on this
 * association or (with options->receiver) on a new association to the receiver SCP, matched by
 * Transaction UID. SIAARA_ERR_TIMEOUT when no report arrived in time. failure_reasons and result
 * as for siaara_dimse_commit; result->status is the event type (1 all committed, 2 failures). */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_commit_ex(const siaara_dimse_peer* peer,
                                                            const siaara_commit_options* options,
                                                            const char* const* sop_classes,
                                                            const char* const* sop_instances, uint32_t count,
                                                            siaara_cancel* cancel, uint16_t* failure_reasons,
                                                            siaara_dimse_result* result);

/* A generic DIMSE-N request on its own association: UPS Push / Pull / Watch (N-CREATE, N-SET,
 * N-GET, N-ACTION), Instance Availability Notification (N-CREATE), or any other N-service. */
enum {
    SIAARA_N_EVENT_REPORT = 0x0100, SIAARA_N_GET = 0x0110, SIAARA_N_SET = 0x0120,
    SIAARA_N_ACTION = 0x0130, SIAARA_N_CREATE = 0x0140, SIAARA_N_DELETE = 0x0150
};
typedef struct siaara_n_request {
    uint32_t struct_size;            /* sizeof(siaara_n_request) */
    uint32_t command_field;          /* SIAARA_N_* */
    const char* abstract_syntax;     /* the presentation context; NULL: sop_class_uid */
    const char* sop_class_uid;
    const char* sop_instance_uid;    /* NULL for N-CREATE: the SCP assigns one (copied to buf) */
    uint32_t type_id;                /* Event Type ID / Action Type ID */
    uint32_t attribute_count;        /* N-GET Attribute Identifier List (0: all) */
    const uint32_t* attribute_tags;
    const siaara_dataset* dataset;   /* NULL: no data set */
    int32_t propose_scp_role;        /* 0/1: role selection with us as SCP (e.g. N-EVENT-REPORT to a subscriber) */
    uint32_t reserved0;
} siaara_n_request;
/* *out (may be NULL) receives the response data set, NULL when there was none (caller owns it).
 * The SOP Instance UID of the response is copied to buf (string buffer rules; buf may be NULL).
 * result->status: the DIMSE status. */
SIAARA_API siaara_status SIAARA_CALL siaara_dimse_n_request(const siaara_dimse_peer* peer, const siaara_n_request* request,
                                                            siaara_cancel* cancel, siaara_file** out, char* buf,
                                                            size_t cap, size_t* needed, siaara_dimse_result* result);

/* A new file holding a copy of `ds` (for keeping a C-FIND match or an SCP event data set beyond
 * its callback; values are shared, not duplicated). Caller owns *out. Thread safety: as reading. */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_clone(const siaara_dataset* ds, siaara_file** out);

/* ---------------------------------------------------------------- DICOMweb server (PS3.18) */
/* An origin server over a siaara_store: QIDO-RS (studies / series / instances, at every level,
 * with paging), WADO-RS (study, series, instance, metadata, frames, bulk data), WADO-URI and
 * STOW-RS. Rendered resources answer 406. */
typedef struct siaara_web_server siaara_web_server;

/* Access control, called for every request (from server threads, several at once) before it is
 * served: return 1 to allow, 0 to answer 401. authorization is the Authorization header ("" when
 * absent); strings valid during the call. Must not call siaara_web_server_free. */
typedef int32_t (SIAARA_CALL* siaara_web_authorize_callback)(void* user, const char* method, const char* target,
                                                            const char* authorization);

typedef struct siaara_web_server_options {
    uint32_t struct_size;            /* sizeof(siaara_web_server_options) */
    uint32_t port;                   /* 0: ephemeral (siaara_web_server_port) */
    const char* bind_address;        /* NULL: "127.0.0.1". Off loopback the server needs TLS plus authorize or
                                        client certificates, unless allow_insecure_remote */
    const char* base_path;           /* NULL: "/dicomweb" */
    const char* public_base_url;     /* absolute URL of base_path for BulkDataURI / RetrieveURL; NULL: from Host */
    const siaara_tls_options* tls;   /* HTTPS with this identity (pkcs12_file, or certificate + key); NULL: HTTP */
    int32_t require_client_certificate; /* 0/1: mutual TLS */
    int32_t allow_insecure_remote;   /* 0/1: DANGEROUS, plain HTTP / no access control off loopback */
    int32_t read_only;               /* 0/1: STOW-RS is not offered (501) */
    int32_t duplicate_policy;        /* SIAARA_DUP_* for STOW-RS */
    siaara_web_authorize_callback authorize; /* NULL: every request is allowed */
    void* authorize_user;
    const char* cors_allow_origin;   /* NULL: no CORS; "*" is refused with authorize */
    uint32_t max_connections;        /* 0 = 32 */
    uint32_t max_qido_results;       /* 0 = 10000 */
    uint64_t max_body_bytes;         /* STOW-RS request body; 0 = 256 MiB */
    /* ---- appended (security review 4); read only when struct_size includes them */
    uint64_t max_response_bytes;     /* a retrieve whose stored instances are larger in total is answered 413
                                        before anything is read; 0 = 1 GiB */
    int32_t response_timeout_ms;     /* the whole response must be written within this; 0 = 10 minutes, -1 = no
                                        limit (R-120) */
    uint32_t min_response_rate;      /* bytes per second a client must read, averaged over 30 s windows;
                                        0 = 1024. Slower: the connection is closed */
    int32_t incomplete_policy;       /* SIAARA_INCOMPLETE_* for STOW-RS parts */
    uint32_t reserved0;              /* set to 0 */
} siaara_web_server_options;

/* Starts the server; store must outlive it. Caller owns *out: free with siaara_web_server_free
 * (stops it). Thread safety: the server runs on its own threads. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_server_start(const siaara_web_server_options* options,
                                                             siaara_store* store, siaara_web_server** out);
SIAARA_API siaara_status SIAARA_CALL siaara_web_server_port(const siaara_web_server* server, uint32_t* out);
SIAARA_API void SIAARA_CALL siaara_web_server_free(siaara_web_server* server) SIAARA_NOEXCEPT;

/* ---------------------------------------------------------------- DICOMweb client additions */
typedef struct siaara_web_limits {
    uint32_t struct_size;            /* sizeof(siaara_web_limits) */
    int32_t read_timeout_ms;         /* each wait for incoming bytes; 0 = keep */
    uint64_t max_body;               /* largest response body; 0 = keep (default 256 MiB) */
    int32_t body_timeout_ms;         /* the whole response body; 0 = keep (default 10 min), -1 = no limit */
    uint32_t max_parts;              /* multipart responses; 0 = keep */
    uint64_t max_part_bytes;         /* one part; 0 = keep */
} siaara_web_limits;
/* Changes the client's limits for later requests. Thread safety: as every client call. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_client_set_limits(siaara_web_client* client,
                                                                  const siaara_web_limits* limits);

/* Progress of a streaming retrieve: parts stored so far, response bytes received, and the total
 * (Content-Length; 0 when unknown). Called on the calling thread; return non-zero to cancel. */
typedef int32_t (SIAARA_CALL* siaara_web_progress_callback)(void* user, uint32_t parts, uint64_t bytes,
                                                            uint64_t total_bytes);
/* WADO-RS retrieve of a study / series / instance that handles each part as it arrives: added to
 * store (policy SIAARA_DUP_*) and / or written to folder as <SOP Instance UID>.dcm (owner-only),
 * so memory holds at most one part (max_part_bytes). Transfer syntaxes as for
 * siaara_web_wado_retrieve. *stored / *failed (may be NULL) count the parts. Cancellation: the
 * attached token (siaara_web_client_set_cancel) or the callback. Parts kept before a failure or a
 * cancel stay kept. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_retrieve_to(siaara_web_client* client, const char* study_uid,
                                                                 const char* series_uid, const char* instance_uid,
                                                                 const char* const* transfer_syntaxes, uint32_t ts_count,
                                                                 siaara_store* store, int32_t policy, const char* folder,
                                                                 siaara_web_progress_callback cb, void* user,
                                                                 uint32_t* stored, uint32_t* failed);

typedef struct siaara_web_retrieve_options {
    uint32_t struct_size;            /* sizeof(siaara_web_retrieve_options) */
    int32_t duplicate_policy;        /* SIAARA_DUP_* for the store and the folder */
    siaara_store* store;             /* parts are added here (may be NULL when folder is set) */
    const char* folder;              /* and / or written here as <SOP Instance UID>.dcm, owner-only (may be NULL) */
    const char* const* transfer_syntaxes; /* as for siaara_web_wado_retrieve; NULL: the default */
    uint32_t transfer_syntax_count;
    int32_t incomplete_policy;       /* SIAARA_INCOMPLETE_*: a part that cannot be read completely */
    int32_t allow_other_uids;        /* 0 (default): a part whose Study / Series / SOP Instance UID is not the
                                        one requested is not kept (counted in uid_mismatch and failed).
                                        1: keep whatever the server sends */
    uint32_t reserved0;              /* set to 0 */
} siaara_web_retrieve_options;

typedef struct siaara_web_retrieve_report {
    uint32_t struct_size;            /* set to sizeof(siaara_web_retrieve_report) */
    uint32_t stored;                 /* parts kept */
    uint32_t failed;                 /* parts not kept (including the two counts below) */
    uint32_t incomplete_refused;     /* not kept: the part could not be read completely (REFUSE, or a part
                                        without a Part 10 preamble, which could only be stored rewritten) */
    uint32_t uid_mismatch;           /* not kept: not the requested study / series / instance */
    uint32_t incomplete_accepted;    /* kept exactly as received although the read was not complete */
    uint32_t rewritten;              /* kept, but the part had no Part 10 preamble: it was written as a new
                                        Part 10 file (file meta added, the data set re-encoded as read) */
    uint32_t reserved0;
} siaara_web_retrieve_report;

/* As siaara_web_wado_retrieve_to, with the receive policy and a report (may be NULL). A part is
 * checked first, then written to the folder, then added to the store; when the store refuses it
 * the file just written is removed again (unless it already existed and was kept), so a failed
 * part leaves nothing behind (R-152). siaara_web_wado_retrieve_to is this call with the default
 * options (incomplete parts refused, UIDs checked). */
SIAARA_API siaara_status SIAARA_CALL siaara_web_wado_retrieve_to_ex(siaara_web_client* client, const char* study_uid,
                                                                    const char* series_uid, const char* instance_uid,
                                                                    const siaara_web_retrieve_options* options,
                                                                    siaara_web_progress_callback cb, void* user,
                                                                    siaara_web_retrieve_report* report);


typedef struct siaara_web_stow_options {
    uint32_t struct_size;            /* set to sizeof(siaara_web_stow_options) */
    int32_t allow_incomplete;        /* 0 (default): a file whose read was not complete is not sent
                                        (SIAARA_ERR_REJECTED, "INCOMPLETE_INPUT ..."). 1: what was read is
                                        written and sent; the server stores an instance that holds less than
                                        the source (as siaara_dimse_peer.allow_incomplete) */
} siaara_web_stow_options;
/* As siaara_web_stow with options (may be NULL: the defaults). */
SIAARA_API siaara_status SIAARA_CALL siaara_web_stow_ex(siaara_web_client* client, const siaara_file* file,
                                                        const char* study_uid,
                                                        const siaara_web_stow_options* options);

#ifdef __cplusplus
}
#endif

#endif /* SIAARA_SIAARA_H */
