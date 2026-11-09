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
 *  - Ownership is stated per function: "borrowed" pointers stay valid until the owning handle is freed.
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
    SIAARA_ERR_INTERNAL = 99
};

/* ---------------------------------------------------------------- version */
#define SIAARA_ABI_VERSION 1u
SIAARA_API uint32_t SIAARA_CALL siaara_abi_version(void);
/* Static string, never freed. */
SIAARA_API const char* SIAARA_CALL siaara_version_string(void);
/* Message for the last failed call on this thread; valid until the next call on this thread. */
SIAARA_API const char* SIAARA_CALL siaara_last_error_message(void);

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
SIAARA_API void SIAARA_CALL siaara_file_free(siaara_file* file);

/* Creates an empty file (explicit VR little endian). Caller owns *out. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_create(siaara_file** out);

/* Borrowed: the main data set and the file meta group. Valid until the file is freed. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_dataset(siaara_file* file, siaara_dataset** out);
SIAARA_API siaara_status SIAARA_CALL siaara_file_meta(siaara_file* file, siaara_dataset** out);
SIAARA_API siaara_status SIAARA_CALL siaara_file_transfer_syntax(const siaara_file* file, char* buf,
                                                                 size_t cap, size_t* needed);
SIAARA_API siaara_status SIAARA_CALL siaara_file_warning_count(const siaara_file* file, uint32_t* out);

/* Writes the file. transfer_syntax_uid may be NULL to keep the current one. */
SIAARA_API siaara_status SIAARA_CALL siaara_file_save(const siaara_file* file, const char* utf8_path,
                                                      size_t path_len, const char* transfer_syntax_uid);

/* ---------------------------------------------------------------- data set access */
/* Tags are (group << 16) | element. vm_index is 0-based. */
SIAARA_API int32_t SIAARA_CALL siaara_dataset_contains(const siaara_dataset* ds, uint32_t tag);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_vr(const siaara_dataset* ds, uint32_t tag, char out_vr[3]);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_vm(const siaara_dataset* ds, uint32_t tag, uint32_t* out);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_string(const siaara_dataset* ds, uint32_t tag,
                                                               uint32_t vm_index, char* buf, size_t cap,
                                                               size_t* needed);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_int64(const siaara_dataset* ds, uint32_t tag,
                                                              uint32_t vm_index, int64_t* out);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_double(const siaara_dataset* ds, uint32_t tag,
                                                               uint32_t vm_index, double* out);
/* Raw value bytes, borrowed (valid until the owning file is freed or the element is changed). */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_raw(const siaara_dataset* ds, uint32_t tag,
                                                            const uint8_t** out_data, size_t* out_size);
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_item_count(const siaara_dataset* ds, uint32_t tag,
                                                               uint32_t* out);
/* Borrowed item data set of a sequence. */
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
 * above. A sequence item that has no (0008,0005) of its own is decoded with the default
 * repertoire here; use siaara_dataset_get_utf8_cs to pass the enclosing data set's value. */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_get_utf8(const siaara_dataset* ds, uint32_t tag,
                                                             uint32_t vm_index, char* buf, size_t cap,
                                                             size_t* needed);
/* Encodes utf8 (all values, backslash separated) into the data set's Specific Character Set and
 * stores it. Fails with SIAARA_ERR_UNSUPPORTED, leaving the data set unchanged, when a character
 * cannot be represented; the message gives the count and the first byte offset. */
SIAARA_API siaara_status SIAARA_CALL siaara_dataset_set_utf8(siaara_dataset* ds, uint32_t tag,
                                                             const char* vr, const char* utf8,
                                                             size_t len);
/* As above, with the Specific Character Set to use when ds has no (0008,0005) of its own
 * (the enclosing data set's raw value, for sequence items). inherited may be NULL. */
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
    SIAARA_JSON_STRICT_CHARSET = 2      /* fail instead of writing U+FFFD for unconvertible text */
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

/* base_url: "http://host:port/path" (https is not available yet). timeout_ms applies to the
 * connect and to every network wait; 0 = default (30 s). Caller owns *out. */
SIAARA_API siaara_status SIAARA_CALL siaara_web_client_create(const char* base_url, size_t len, int32_t timeout_ms,
                                                              siaara_web_client** out);
SIAARA_API void SIAARA_CALL siaara_web_client_free(siaara_web_client* client);
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
 * Returns SIAARA_OK when stored, SIAARA_ERR_REJECTED when the server refused the instance. */
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
} siaara_store_options;

enum { SIAARA_DUP_KEEP = 0, SIAARA_DUP_REPLACE = 1 };
enum { SIAARA_ADD_COPY = 0, SIAARA_ADD_REFERENCE = 1 };
enum { SIAARA_ADD_ADDED = 0, SIAARA_ADD_REPLACED = 1, SIAARA_ADD_KEPT_EXISTING = 2 };

/* options may be NULL. Caller owns *out: free with siaara_store_free. */
SIAARA_API siaara_status SIAARA_CALL siaara_store_open(const char* utf8_root, size_t root_len,
                                                       const siaara_store_options* options,
                                                       siaara_store** out);
SIAARA_API void SIAARA_CALL siaara_store_free(siaara_store* store);
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
SIAARA_API void SIAARA_CALL siaara_store_result_free(siaara_store_result* result);

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
 * plain TCP. Note: https:// base URLs are also accepted by siaara_web_client_create (OS trust
 * store, no client certificate).
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
SIAARA_API void SIAARA_CALL siaara_volume_free(siaara_volume* volume);

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
SIAARA_API void SIAARA_CALL siaara_view_free(siaara_view* view);
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

#ifdef __cplusplus
}
#endif

#endif /* SIAARA_SIAARA_H */
