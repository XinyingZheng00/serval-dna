// See ozonedb_shim.h. Not upstream serval-dna -- added for the real
// swap-in benchmark comparison against OzoneDB.
#include "ozonedb_shim.h"
#include "db.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {
ozonedb::DB* g_db = nullptr;

// Read-your-own-writes cache. DB::get() reads the metadata view that a
// background thread refreshes about every 100 ms
// (src/db/metadata_log_handler.cpp, rollForwardMetadataLogPeriodically), so
// a get() right after a put() on the same handle can miss that put for up to
// ~100 ms. Rhizome does exactly that: rhizome_add_manifest_to_store() stores
// a payload, then checks rhizome_exists() before writing the manifest. This
// cache makes a process's own puts visible to its later gets at once. Other
// processes still see a write only after they catch up (at openDB() or on
// the next background refresh). Unbounded for the life of the process,
// which is fine for a bounded benchmark run.
std::unordered_map<std::string, std::string> g_write_cache;
std::mutex g_cache_mutex;
}  // namespace

extern "C" int ozshim_open(const char* config_path) {
  if (g_db != nullptr) return 0;  // already open (each process opens once)
  ozonedb::DB* db = nullptr;
  ozonedb::Status st = ozonedb::DB::openDB(db, std::string(config_path));
  if (st != ozonedb::Status::kSuccess) return -1;
  g_db = db;
  return 0;
}

extern "C" int ozshim_put(const char* key, const void* value, size_t value_len) {
  if (!g_db) return -1;
  std::string v(static_cast<const char*>(value), value_len);
  ozonedb::Status st = g_db->put(std::string(key), v);
  if (st != ozonedb::Status::kSuccess) return -1;
  {
    std::lock_guard<std::mutex> lk(g_cache_mutex);
    g_write_cache[key] = std::move(v);
  }
  return 0;
}

extern "C" int ozshim_get(const char* key, void** value_out, size_t* value_len_out) {
  if (!g_db) return -1;
  {
    std::lock_guard<std::mutex> lk(g_cache_mutex);
    auto it = g_write_cache.find(key);
    if (it != g_write_cache.end()) {
      void* buf = std::malloc(it->second.size());
      if (!buf) return -1;
      std::memcpy(buf, it->second.data(), it->second.size());
      *value_out = buf;
      *value_len_out = it->second.size();
      return 0;
    }
  }
  std::string const* v = nullptr;
  ozonedb::Status st = g_db->get(std::string(key), v);
  if (st != ozonedb::Status::kSuccess || v == nullptr) return 1;  // not found
  void* buf = std::malloc(v->size());
  if (!buf) return -1;
  std::memcpy(buf, v->data(), v->size());
  *value_out = buf;
  *value_len_out = v->size();
  return 0;
}

extern "C" int ozshim_remove(const char* key) {
  if (!g_db) return -1;
  ozonedb::Status st = g_db->remove(std::string(key));
  if (st != ozonedb::Status::kSuccess) return -1;
  {
    std::lock_guard<std::mutex> lk(g_cache_mutex);
    g_write_cache.erase(key);
  }
  return 0;
}

extern "C" void ozshim_close(void) {
  if (g_db) {
    ozonedb::DB::closeDB(g_db);
    g_db = nullptr;
  }
  std::lock_guard<std::mutex> lk(g_cache_mutex);
  g_write_cache.clear();
}
