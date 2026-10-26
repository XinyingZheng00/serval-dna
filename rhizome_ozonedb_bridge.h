/* Bridge between rhizome_database.c / rhizome_store.c and the OzoneDB shim.
 * Not upstream serval-dna -- added for the real swap-in benchmark
 * comparison. Scope: rhizome_add_manifest_to_store(),
 * rhizome_retrieve_manifest(), rhizome_exists(), and the FILEBLOBS
 * write/read paths only. rhizome_find_duplicate() is stubbed elsewhere
 * (rhizome_database.c) to always report "not found"; rhizome list's
 * filtered/sorted query is out of scope entirely.
 */
#ifndef RHIZOME_OZONEDB_BRIDGE_H
#define RHIZOME_OZONEDB_BRIDGE_H

#include "rhizome.h"

/* Lazily opens the OzoneDB store on first call if the
 * RHIZOME_OZONEDB_CONFIG env var is set to a shared_config path. Returns 1
 * if OzoneDB should be used for this process, 0 for the normal SQLite path.
 * Cached after the first call. */
int rhizome_ozonedb_enabled(void);

enum rhizome_bundle_status ozonedb_retrieve_manifest(const rhizome_bid_t *bidp, rhizome_manifest *m);
enum rhizome_bundle_status ozonedb_add_manifest(rhizome_manifest *m, time_ms_t now);

enum rhizome_payload_status ozonedb_check_exists(const rhizome_filehash_t *hashp);

int ozonedb_write_get_lock(struct rhizome_write *write_state);
int ozonedb_write_data(struct rhizome_write *write_state, uint64_t file_offset, const uint8_t *buffer, size_t data_size);
int ozonedb_write_finish(struct rhizome_write *write, uint64_t file_length);

enum rhizome_payload_status ozonedb_read_open(struct rhizome_read *read, const rhizome_filehash_t *hashp);
ssize_t ozonedb_read_data(struct rhizome_read *read_state, unsigned char *buffer, size_t bufsz);

#endif // RHIZOME_OZONEDB_BRIDGE_H
