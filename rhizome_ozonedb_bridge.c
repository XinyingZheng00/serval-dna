/* See rhizome_ozonedb_bridge.h. Not upstream serval-dna -- added for the
 * real swap-in benchmark comparison.
 *
 * Value layouts (host-endian, single-machine/single-experiment only, no
 * cross-version compatibility needed):
 *
 *   manifest:<hex bid>   -> [u32 blob_len][blob bytes][u64 version]
 *                            [i64 inserttime][u8 has_author][32 bytes author if has_author]
 *   files:<hex filehash> -> [u64 length][u8 datavalid][i64 inserttime][i64 last_verified]
 *   fileblob:<hex filehash> -> raw payload bytes
 */
#include "rhizome_ozonedb_bridge.h"
#include "ozonedb_shim.h"
#include "serval.h"
#include "conf.h"
#include "str.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int g_checked = 0;
static int g_enabled = 0;

int rhizome_ozonedb_enabled(void)
{
  if (!g_checked) {
    g_checked = 1;
    const char *cfg = getenv("RHIZOME_OZONEDB_CONFIG");
    if (cfg && *cfg) {
      if (ozshim_open(cfg) == -1) {
        WHYF("Failed to open OzoneDB backend at %s -- falling back to SQLite", cfg);
        g_enabled = 0;
      } else {
        INFOF("Rhizome using OzoneDB backend: %s", cfg);
        g_enabled = 1;
      }
    }
  }
  return g_enabled;
}

// ---------------------------------------------------------------------------
// small serialization helpers
// ---------------------------------------------------------------------------

static void append_bytes(unsigned char **buf, size_t *len, size_t *cap, const void *data, size_t n)
{
  if (*len + n > *cap) {
    size_t newcap = (*cap == 0 ? 256 : *cap * 2);
    while (newcap < *len + n) newcap *= 2;
    *buf = realloc(*buf, newcap);
    *cap = newcap;
  }
  memcpy(*buf + *len, data, n);
  *len += n;
}

static void manifest_key(char *out, size_t outsz, const rhizome_bid_t *bidp)
{
  snprintf(out, outsz, "manifest:%s", alloca_tohex_rhizome_bid_t(*bidp));
}
static void files_key(char *out, size_t outsz, const rhizome_filehash_t *hashp)
{
  snprintf(out, outsz, "files:%s", alloca_tohex_rhizome_filehash_t(*hashp));
}
static void fileblob_key(char *out, size_t outsz, const rhizome_filehash_t *hashp)
{
  snprintf(out, outsz, "fileblob:%s", alloca_tohex_rhizome_filehash_t(*hashp));
}

// ---------------------------------------------------------------------------
// manifest retrieve / add
// ---------------------------------------------------------------------------

enum rhizome_bundle_status ozonedb_retrieve_manifest(const rhizome_bid_t *bidp, rhizome_manifest *m)
{
  char key[128];
  manifest_key(key, sizeof key, bidp);
  void *val = NULL;
  size_t vlen = 0;
  int rc = ozshim_get(key, &val, &vlen);
  if (rc == 1)
    return RHIZOME_BUNDLE_STATUS_NEW; // not found
  if (rc != 0)
    return RHIZOME_BUNDLE_STATUS_ERROR;

  const unsigned char *p = (const unsigned char *)val;
  const unsigned char *end = p + vlen;
  if (p + 4 > end) { free(val); return RHIZOME_BUNDLE_STATUS_ERROR; }
  uint32_t blob_len; memcpy(&blob_len, p, 4); p += 4;
  if (blob_len > MAX_MANIFEST_BYTES || p + blob_len > end) { free(val); return RHIZOME_BUNDLE_STATUS_ERROR; }
  memcpy(m->manifestdata, p, blob_len);
  p += blob_len;
  m->manifest_all_bytes = blob_len;

  if (p + 8 + 8 + 1 > end) { free(val); return RHIZOME_BUNDLE_STATUS_ERROR; }
  uint64_t q_version; memcpy(&q_version, p, 8); p += 8;
  int64_t q_inserttime; memcpy(&q_inserttime, p, 8); p += 8;
  uint8_t has_author = *p; p += 1;

  if (rhizome_manifest_parse(m) == -1 || !rhizome_manifest_validate(m)) {
    free(val);
    return WHYF("Manifest bid=%s in ozonedb store but invalid", alloca_tohex_rhizome_bid_t(*bidp));
  }
  if (has_author) {
    if (p + sizeof(sid_t) > end) { free(val); return RHIZOME_BUNDLE_STATUS_ERROR; }
    sid_t author;
    memcpy(author.binary, p, sizeof author.binary);
    p += sizeof author.binary;
    rhizome_manifest_set_author(m, &author);
  }
  if (m->version != q_version)
    WARNF("Version mismatch, manifest is %"PRIu64", ozonedb is %"PRIu64, m->version, q_version);
  // Synthetic rowid: real serval-dna relies on SQLite's monotonically
  // increasing autoincrement rowid for a few peripheral purposes (mainly
  // `rhizome list` ordering, which is out of scope here). inserttime in ms
  // is monotonic enough for anything add/extract touch.
  rhizome_manifest_set_rowid(m, (uint64_t) q_inserttime);
  rhizome_manifest_set_inserttime(m, q_inserttime);
  free(val);
  return RHIZOME_BUNDLE_STATUS_SAME;
}

// rhizome_add_manifest_to_store()'s post-insert bookkeeping asserts
// max_rowid < m->rowid (a *process-local* strictly-increasing sequence,
// not a global one -- real SQLite guarantees this via autoincrement
// ROWID). Wall-clock ms is NOT safe here: this bridge exists specifically
// to benchmark concurrent adds, i.e. the exact condition under which two
// adds in the same process could land in the same millisecond. A
// per-process monotonic counter is strictly increasing regardless.
static uint64_t next_rowid(void)
{
  static uint64_t counter = 0;
  return ++counter;
}

enum rhizome_bundle_status ozonedb_add_manifest(rhizome_manifest *m, time_ms_t now)
{
  char key[128];
  manifest_key(key, sizeof key, &m->keypair.public_key);

  unsigned char *buf = NULL;
  size_t len = 0, cap = 0;
  uint32_t blob_len = (uint32_t) m->manifest_all_bytes;
  append_bytes(&buf, &len, &cap, &blob_len, 4);
  append_bytes(&buf, &len, &cap, m->manifestdata, blob_len);
  uint64_t version = m->version;
  append_bytes(&buf, &len, &cap, &version, 8);
  int64_t inserttime = (int64_t) now;
  append_bytes(&buf, &len, &cap, &inserttime, 8);
  uint8_t has_author = (m->authorship == AUTHOR_AUTHENTIC) ? 1 : 0;
  append_bytes(&buf, &len, &cap, &has_author, 1);
  if (has_author)
    append_bytes(&buf, &len, &cap, m->author.binary, sizeof m->author.binary);

  int rc = ozshim_put(key, buf, len);
  free(buf);
  if (rc != 0)
    return RHIZOME_BUNDLE_STATUS_ERROR;

  rhizome_manifest_set_rowid(m, next_rowid());
  rhizome_manifest_set_inserttime(m, inserttime);
  return RHIZOME_BUNDLE_STATUS_NEW;
}

// ---------------------------------------------------------------------------
// payload exists / write / read
// ---------------------------------------------------------------------------

enum rhizome_payload_status ozonedb_check_exists(const rhizome_filehash_t *hashp)
{
  char key[144];
  files_key(key, sizeof key, hashp);
  void *val = NULL;
  size_t vlen = 0;
  int rc = ozshim_get(key, &val, &vlen);
  if (rc == 1)
    return RHIZOME_PAYLOAD_STATUS_NEW;
  if (rc != 0)
    return RHIZOME_PAYLOAD_STATUS_ERROR;
  int datavalid = (vlen >= 9) ? ((const unsigned char *)val)[8] : 0;
  free(val);
  return datavalid ? RHIZOME_PAYLOAD_STATUS_STORED : RHIZOME_PAYLOAD_STATUS_NEW;
}

int ozonedb_write_get_lock(struct rhizome_write *write_state)
{
  if (write_state->oz_buf == NULL && write_state->file_length != RHIZOME_SIZE_UNSET) {
    write_state->oz_buf_cap = write_state->file_length;
    write_state->oz_buf = malloc(write_state->oz_buf_cap ? write_state->oz_buf_cap : 1);
    write_state->oz_buf_len = 0;
  }
  return 0;
}

int ozonedb_write_data(struct rhizome_write *write_state, uint64_t file_offset, const uint8_t *buffer, size_t data_size)
{
  size_t need = (size_t) file_offset + data_size;
  if (need > write_state->oz_buf_cap) {
    size_t newcap = write_state->oz_buf_cap == 0 ? 4096 : write_state->oz_buf_cap;
    while (newcap < need) newcap *= 2;
    unsigned char *nb = realloc(write_state->oz_buf, newcap);
    if (!nb) return WHY("out of memory buffering payload for OzoneDB write");
    write_state->oz_buf = nb;
    write_state->oz_buf_cap = newcap;
  }
  memcpy(write_state->oz_buf + file_offset, buffer, data_size);
  if (need > write_state->oz_buf_len)
    write_state->oz_buf_len = need;
  return 0;
}

int ozonedb_write_finish(struct rhizome_write *write, uint64_t file_length)
{
  char fbkey[144], fkey[144];
  fileblob_key(fbkey, sizeof fbkey, &write->id);
  files_key(fkey, sizeof fkey, &write->id);

  if (ozshim_put(fbkey, write->oz_buf ? (const void *)write->oz_buf : (const void *)"", write->oz_buf_len) != 0)
    return -1;

  unsigned char fbuf[8 + 1 + 8 + 8];
  unsigned char *p = fbuf;
  uint64_t length = file_length;
  memcpy(p, &length, 8); p += 8;
  uint8_t datavalid = 1;
  memcpy(p, &datavalid, 1); p += 1;
  int64_t now = (int64_t) gettime_ms();
  memcpy(p, &now, 8); p += 8; // inserttime
  memcpy(p, &now, 8); p += 8; // last_verified
  if (ozshim_put(fkey, fbuf, sizeof fbuf) != 0)
    return -1;

  free(write->oz_buf);
  write->oz_buf = NULL;
  write->oz_buf_len = write->oz_buf_cap = 0;
  return 0;
}

enum rhizome_payload_status ozonedb_read_open(struct rhizome_read *read, const rhizome_filehash_t *hashp)
{
  char fkey[144], fbkey[144];
  files_key(fkey, sizeof fkey, hashp);
  fileblob_key(fbkey, sizeof fbkey, hashp);

  void *fval = NULL; size_t flen = 0;
  int rc = ozshim_get(fkey, &fval, &flen);
  if (rc == 1)
    return RHIZOME_PAYLOAD_STATUS_NEW;
  if (rc != 0 || flen < 9)
    return RHIZOME_PAYLOAD_STATUS_ERROR;
  uint64_t length; memcpy(&length, fval, 8);
  uint8_t datavalid = ((const unsigned char *)fval)[8];
  free(fval);
  if (!datavalid)
    return RHIZOME_PAYLOAD_STATUS_NEW;

  void *bval = NULL; size_t blen = 0;
  rc = ozshim_get(fbkey, &bval, &blen);
  if (rc != 0)
    return RHIZOME_PAYLOAD_STATUS_ERROR;

  read->length = length;
  read->oz_buf = (unsigned char *) bval;
  read->oz_buf_len = blen;
  crypto_hash_sha512_init(&read->sha512_context);
  return RHIZOME_PAYLOAD_STATUS_STORED;
}

ssize_t ozonedb_read_data(struct rhizome_read *read_state, unsigned char *buffer, size_t bufsz)
{
  if (read_state->offset >= read_state->length)
    return 0;
  size_t avail = (size_t)(read_state->length - read_state->offset);
  size_t n = bufsz < avail ? bufsz : avail;
  if (n == 0)
    return 0;
  if (read_state->offset + n > read_state->oz_buf_len)
    return WHY("OzoneDB payload buffer shorter than recorded FILES length");
  if (buffer)
    memcpy(buffer, read_state->oz_buf + read_state->offset, n);
  return (ssize_t) n;
}
