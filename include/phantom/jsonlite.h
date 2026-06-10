#ifndef PHANTOM_JSONLITE_H
#define PHANTOM_JSONLITE_H
/* Minimal JSON helpers shared by the catalog, config, and operation modules.
 * This is intentionally not a full JSON parser: the bundled catalogs are
 * trusted, integrity-checked documents, and these helpers only extract
 * string/bool fields from objects inside them. */
#include "phantom/common.h"

/* Read an entire file into a NUL-terminated heap buffer. Caller frees. */
char *ph_read_all_file(const char *path);

const char *ph_json_skip_ws(const char *p);

/* Given a pointer at '{', return the matching '}' (string/escape aware). */
const char *ph_json_object_end(const char *start);

/* Extract a string field from the object delimited by [start, end].
 * Returns true and writes the (escape-stripped) value when found. */
bool ph_json_string_field(const char *start, const char *end, const char *key, char *out, size_t out_len);

/* True when the object contains "key": true. */
bool ph_json_bool_field(const char *start, const char *end, const char *key);

/* Iterate the objects of a top-level JSON array. Pass *cursor = document
 * text on the first call; returns false when no more objects exist. */
bool ph_json_next_object(const char **cursor, const char **start, const char **end);

/* Load a catalog document (path1, falling back to path2) and locate the
 * object whose `field` equals `value`. On success the caller owns *text_out
 * and must free() it; start/end delimit the matched object inside it. */
bool ph_json_load_object_by_field(const char *path1, const char *path2,
                                  const char *field, const char *value,
                                  char **text_out, const char **start_out, const char **end_out,
                                  ph_error *err);
#endif
