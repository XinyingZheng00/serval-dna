/* C-linkage shim exposing just enough of OzoneDB's put/get API for
 * rhizome_database.c / rhizome_store.c to use as an alternate backend to
 * SQLite, for the real-swap-in benchmark comparison. Not upstream serval-dna
 * -- added for this experiment.
 *
 * Scope: only rhizome_add_manifest_to_store(),
 * rhizome_retrieve_manifest(), rhizome_exists(), and the FILEBLOBS
 * write/read paths are routed through this. rhizome_find_duplicate() is
 * stubbed to always report "not found" and rhizome list's filtered/sorted
 * query is out of scope entirely -- neither needs a real backend for this
 * comparison.
 */
#ifndef OZONEDB_SHIM_H
#define OZONEDB_SHIM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 on success, -1 on failure. config_path is an OzoneDB
 * shared_config JSON path (same format used elsewhere in this project). */
int ozshim_open(const char *config_path);

/* Returns 0 on success, -1 on failure. */
int ozshim_put(const char *key, const void *value, size_t value_len);

/* On success, returns 0 and sets *value_out to a freshly malloc()'d buffer
 * (caller must free() it) with *value_len_out bytes. Returns 1 if the key
 * does not exist (not an error). Returns -1 on a real failure. */
int ozshim_get(const char *key, void **value_out, size_t *value_len_out);

/* Returns 0 on success (including "key didn't exist"), -1 on failure. */
int ozshim_remove(const char *key);

void ozshim_close(void);

#ifdef __cplusplus
}
#endif

#endif // OZONEDB_SHIM_H
