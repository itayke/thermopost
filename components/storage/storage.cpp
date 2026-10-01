#include "storage.hpp"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

namespace thermopost::storage {
namespace {

constexpr const char* kTag          = "storage";
constexpr const char* kPartitionLbl = "storage";  // matches partitions.csv
constexpr const char* kLittlefsTag  = "esp_littlefs";  // the library's own log tag
constexpr const char* kBasePath     = "/littlefs";
constexpr const char* kLogPath      = "/littlefs/temps.bin";
constexpr const char* kLogTmpPath   = "/littlefs/temps.bin.tmp";
constexpr uint8_t     kRecordVersion = 2;

// The v1 layout, kept only so records written before the aggregate schema can be
// read. Same 16 bytes as v2 — that is the whole reason the two can share a file
// and why nothing had to be rewritten when the schema changed. Private: callers
// never see a v1 record, because every read upgrades on the way out.
struct __attribute__((packed)) TempRecordV1 {
  uint8_t  version;
  uint8_t  flags;
  int16_t  temp_c100;
  uint32_t epoch_s;
  int64_t  uptime_us;
};
static_assert(sizeof(TempRecordV1) == sizeof(TempRecord),
              "the v1 and v2 layouts must be the same size to share one file");

constexpr int64_t kUsPerSec = 1'000'000;

// Reinterpret a record read from disk as the current schema. A v1 record is a
// single instantaneous sample, so it is its own minimum and maximum — which is
// honest rather than a fudge: that really is all the interval's history that was
// ever captured, and pretending otherwise would be worse than saying so.
void upgrade_in_place(TempRecord* r) {
  if (r == nullptr || r->version >= kRecordVersion) return;

  TempRecordV1 old;
  std::memcpy(&old, r, sizeof(old));  // copy first: the fields overlap

  r->version   = kRecordVersion;
  r->flags     = old.flags;
  r->temp_c100 = old.temp_c100;
  r->min_c100  = old.temp_c100;
  r->max_c100  = old.temp_c100;
  r->epoch_s   = old.epoch_s;
  r->uptime_s  = static_cast<uint32_t>(old.uptime_us / kUsPerSec);
}

// NVS holds the base index — the absolute record number of the record currently
// at file offset 0. See the rotation transaction below for why there are two
// keys rather than one. (NVS keys are capped at 15 characters.)
constexpr const char* kNvsNamespace = "storage";
constexpr const char* kNvsKeyBase   = "base";
constexpr const char* kNvsKeyPending = "rot_pending";

bool     s_mounted = false;
uint32_t s_base    = 0;  // mirrors kNvsKeyBase; NVS is the source of truth
// Records currently in the file. Cached because count() is on the UI's path —
// the status strip asks for it several times a second — and the only way to get
// it from littlefs is a stat(), which takes the filesystem's own lock. That put
// the LVGL task behind whatever else was touching flash: an append, the cloud
// uploader reading a batch, or worst of all a rotation rewriting ninety thousand
// records. A frozen screen with dead buttons, for as long as the background work
// ran. Refreshed from the file at every point that changes it, always under
// s_lock, so it is re-derived rather than counted up and cannot drift.
uint32_t s_records = 0;

// append + read + rotate all touch the same file. Today the sample task is the
// only writer and the LVGL task the only reader, but that is two tasks already.
SemaphoreHandle_t s_lock = nullptr;

struct Guard {
  bool ok;
  explicit Guard(TickType_t timeout = portMAX_DELAY)
      : ok(xSemaphoreTake(s_lock, timeout) == pdTRUE) {}
  ~Guard() { if (ok) xSemaphoreGive(s_lock); }
};

// --- NVS helpers -----------------------------------------------------------

bool nvs_read_u32(const char* key, uint32_t* out) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;
  const esp_err_t err = nvs_get_u32(h, key, out);
  nvs_close(h);
  return err == ESP_OK;
}

bool nvs_write_pair(const char* k1, uint32_t v1, const char* k2, uint32_t v2) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_set_u32(h, k1, v1) == ESP_OK &&
                  nvs_set_u32(h, k2, v2) == ESP_OK &&
                  nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

// --- file helpers ----------------------------------------------------------

// Records currently in the file. Cheap — stat() then divide.
uint32_t file_count() {
  struct stat st = {};
  if (stat(kLogPath, &st) != 0) return 0;  // not created yet
  return static_cast<uint32_t>(st.st_size / sizeof(TempRecord));
}

bool file_exists(const char* path) {
  struct stat st = {};
  return stat(path, &st) == 0;
}

// ---------------------------------------------------------------------------
// Rotation
// ---------------------------------------------------------------------------
// Dropping the oldest kRotateBlock records shifts every surviving record's file
// offset, so the base index has to advance by exactly the same amount or the
// absolute numbering that cloud sync depends on silently breaks.
//
// That makes rotation a two-store transaction — a file rename plus an NVS write
// — and a power loss between them corrupts the numbering in one direction or the
// other. Committing NVS first and crashing would label every surviving record
// kRotateBlock too high; renaming first and crashing would REUSE indices already
// uploaded, which the Apps Script's index dedupe would then silently drop.
//
// So the pending target is journalled before either store is touched:
//
//   1. build temps.bin.tmp (the survivors)
//   2. NVS: rot_pending = base + kRotateBlock      <-- intent is now durable
//   3. rename tmp -> temps.bin
//   4. NVS: base = rot_pending, rot_pending = 0    <-- transaction closed
//
// A crash anywhere leaves rot_pending set, and init() rolls the transaction
// forward: if the temp file is still there step 3 never ran, so finish it;
// either way adopt the pending base. The operation is idempotent.
esp_err_t rotate_locked(uint32_t drop_count) {
  const uint32_t have = file_count();
  if (drop_count == 0 || have <= drop_count) return ESP_OK;  // nothing sensible to drop

  FILE* in = std::fopen(kLogPath, "rb");
  if (!in) {
    ESP_LOGE(kTag, "rotate: open %s for read failed", kLogPath);
    return ESP_FAIL;
  }
  if (std::fseek(in, static_cast<long>(drop_count) * sizeof(TempRecord),
                 SEEK_SET) != 0) {
    std::fclose(in);
    ESP_LOGE(kTag, "rotate: seek past the dropped block failed");
    return ESP_FAIL;
  }

  unlink(kLogTmpPath);  // clear any leftover from an interrupted attempt
  FILE* out = std::fopen(kLogTmpPath, "wb");
  if (!out) {
    std::fclose(in);
    ESP_LOGE(kTag, "rotate: open %s for write failed", kLogTmpPath);
    return ESP_FAIL;
  }

  // Copy the survivors in batches — a per-record fread/fwrite pair across
  // ~90k records is needlessly slow on littlefs.
  constexpr size_t kBatch = 64;
  TempRecord buf[kBatch];
  size_t moved = 0;
  for (;;) {
    const size_t got = std::fread(buf, sizeof(TempRecord), kBatch, in);
    if (got == 0) break;
    if (std::fwrite(buf, sizeof(TempRecord), got, out) != got) {
      ESP_LOGE(kTag, "rotate: short write");
      std::fclose(in);
      std::fclose(out);
      unlink(kLogTmpPath);
      return ESP_FAIL;
    }
    moved += got;
  }
  std::fflush(out);
  const int outfd = fileno(out);
  if (outfd >= 0) fsync(outfd);
  std::fclose(out);
  std::fclose(in);

  // Step 2 — journal the intent before either store changes.
  const uint32_t pending = s_base + drop_count;
  if (!nvs_write_pair(kNvsKeyPending, pending, kNvsKeyBase, s_base)) {
    ESP_LOGE(kTag, "rotate: could not journal intent — abandoning, log untouched");
    unlink(kLogTmpPath);
    return ESP_FAIL;
  }

  // Step 3 — littlefs's rename replaces the target in one metadata operation.
  if (std::rename(kLogTmpPath, kLogPath) != 0) {
    ESP_LOGE(kTag, "rotate: rename failed — init() will roll this forward");
    return ESP_FAIL;
  }

  // Step 4 — close the transaction.
  if (!nvs_write_pair(kNvsKeyBase, pending, kNvsKeyPending, 0)) {
    ESP_LOGE(kTag, "rotate: could not close transaction — init() will finish it");
    return ESP_FAIL;
  }
  s_base = pending;
  s_records = file_count();

  ESP_LOGW(kTag, "rotated: dropped %u oldest records, %u kept, base index now %u",
           static_cast<unsigned>(drop_count), static_cast<unsigned>(moved),
           static_cast<unsigned>(s_base));
  return ESP_OK;
}

// Roll a rotation interrupted by power loss forward to completion. See the
// comment on rotate_locked() for the state machine this closes out.
void recover_rotation() {
  uint32_t pending = 0;
  if (!nvs_read_u32(kNvsKeyPending, &pending) || pending == 0) return;

  ESP_LOGW(kTag, "rotation was interrupted (pending base %u) — rolling forward",
           static_cast<unsigned>(pending));

  if (file_exists(kLogTmpPath)) {
    // Step 3 never ran. The temp file holds the survivors; finish the swap.
    if (std::rename(kLogTmpPath, kLogPath) != 0) {
      ESP_LOGE(kTag, "recover: rename failed — dropping the temp file and keeping "
                     "the un-rotated log at base %u",
               static_cast<unsigned>(s_base));
      unlink(kLogTmpPath);
      nvs_write_pair(kNvsKeyPending, 0, kNvsKeyBase, s_base);
      return;
    }
  }
  // Either the rename had already landed or we just completed it; adopt the
  // pending base and close the transaction.
  if (nvs_write_pair(kNvsKeyBase, pending, kNvsKeyPending, 0)) {
    s_base = pending;
    ESP_LOGW(kTag, "rotation recovered, base index now %u",
             static_cast<unsigned>(s_base));
  }
}

// A never-written flash partition reads back as all 0xFF, so a handful of bytes
// at offset 0 is enough to tell "no filesystem yet" from "filesystem damaged".
// Reading the superblock region is sufficient — littlefs writes there first.
constexpr size_t kBlankProbeBytes = 256;

bool partition_is_blank() {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPartitionLbl);
  if (part == nullptr) return false;

  uint8_t probe[kBlankProbeBytes];
  if (esp_partition_read(part, 0, probe, sizeof(probe)) != ESP_OK) return false;
  for (const uint8_t b : probe) {
    if (b != 0xFF) return false;
  }
  return true;
}

}  // namespace

esp_err_t init() {
  if (s_mounted) return ESP_ERR_INVALID_STATE;

  s_lock = xSemaphoreCreateMutex();
  if (s_lock == nullptr) return ESP_ERR_NO_MEM;

  esp_vfs_littlefs_conf_t conf = {
      .base_path              = kBasePath,
      .partition_label        = kPartitionLbl,
      .partition              = nullptr,
      .format_if_mount_failed = false,  // see below — we format deliberately
      .read_only              = false,
      .dont_mount             = false,
      .grow_on_mount          = false,
  };

  // Mounting a blank partition necessarily fails, and littlefs logs that at
  // ERROR ("corrupted dir pair") — which on a first boot reads as a fault when
  // it is the expected outcome. So: check whether the partition has ever been
  // written, try the mount with littlefs's own logging muted, and say which of
  // the two cases actually happened. A format that destroys real data is worth
  // a warning; a format of empty flash is not.
  const bool blank = partition_is_blank();
  esp_log_level_set(kLittlefsTag, ESP_LOG_NONE);
  esp_err_t err = esp_vfs_littlefs_register(&conf);
  esp_log_level_set(kLittlefsTag, static_cast<esp_log_level_t>(CONFIG_LOG_DEFAULT_LEVEL));

  if (err != ESP_OK) {
    if (blank) {
      ESP_LOGI(kTag, "storage partition is blank — formatting (first boot)");
    } else {
      ESP_LOGW(kTag,
               "littlefs mount failed (%s) on a written partition — reformatting; "
               "stored records are lost",
               esp_err_to_name(err));
    }
    conf.format_if_mount_failed = true;
    err = esp_vfs_littlefs_register(&conf);
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "littlefs mount failed: %s", esp_err_to_name(err));
    return err;
  }

  // A missing key is the first-boot case, not an error — base stays 0.
  if (!nvs_read_u32(kNvsKeyBase, &s_base)) s_base = 0;
  recover_rotation();

  s_mounted = true;
  s_records = file_count();

  size_t total = 0, used = 0;
  if (esp_littlefs_info(kPartitionLbl, &total, &used) == ESP_OK) {
    ESP_LOGI(kTag, "littlefs mounted at %s (%u/%u KB used)", kBasePath,
             static_cast<unsigned>(used / 1024), static_cast<unsigned>(total / 1024));
  }
  ESP_LOGI(kTag, "log: %u records on disk, absolute index range [%u, %u)",
           static_cast<unsigned>(count()), static_cast<unsigned>(first_index()),
           static_cast<unsigned>(next_index()));
  return ESP_OK;
}

esp_err_t append(const TempRecord& record) {
  if (!s_mounted) return ESP_ERR_INVALID_STATE;

  TempRecord r = record;
  r.version = kRecordVersion;

  Guard g;
  if (!g.ok) return ESP_ERR_TIMEOUT;

  FILE* f = std::fopen(kLogPath, "ab");
  if (!f) {
    ESP_LOGE(kTag, "fopen %s failed", kLogPath);
    return ESP_FAIL;
  }
  const size_t written = std::fwrite(&r, 1, sizeof(r), f);
  // fflush + fsync: without fsync the FILE* buffer is RAM-only and a power loss
  // before the next superblock flush loses the sample.
  std::fflush(f);
  const int fd = fileno(f);
  if (fd >= 0) fsync(fd);
  std::fclose(f);

  if (written != sizeof(r)) {
    ESP_LOGE(kTag, "short write (%u/%u bytes)", static_cast<unsigned>(written),
             static_cast<unsigned>(sizeof(r)));
    return ESP_FAIL;
  }

  // Rotate AFTER the append has been committed. A rotation failure is logged but
  // deliberately not propagated — the caller's reading is safely on disk, and
  // failing the append to report a housekeeping problem would trade real data
  // for tidiness. The log simply grows past the cap until a later attempt wins.
  s_records = file_count();
  if (s_records > kMaxRecords) {
    const esp_err_t rot_err = rotate_locked(kRotateBlock);
    if (rot_err != ESP_OK) {
      // rotate_locked only refreshes the cache on the path that succeeds, so do
      // it here too — the log is over cap and the count still has to be true.
      s_records = file_count();
      ESP_LOGE(kTag, "rotation failed (%s) — log is over cap at %u records",
               esp_err_to_name(rot_err), static_cast<unsigned>(s_records));
    }
  }
  return ESP_OK;
}

uint32_t first_index() { return s_base; }

uint32_t count() {
  if (!s_mounted) return 0;
  return s_records;  // cached; see the declaration for why it is not a stat()
}

uint32_t next_index() { return s_base + count(); }

size_t read_from(uint32_t from_index, TempRecord* out, size_t max,
                 uint32_t* out_first_index) {
  if (out_first_index != nullptr) *out_first_index = from_index;
  if (!s_mounted || out == nullptr || max == 0) return 0;

  Guard g;
  if (!g.ok) return 0;

  // Clamp a request that rotation has already outrun. Reporting the real start
  // is the only way the caller can tell records were skipped — the count alone
  // looks identical to a normal read.
  uint32_t start = from_index;
  if (start < s_base) {
    ESP_LOGW(kTag, "read_from(%u) is below the rotated-away boundary %u — "
                   "starting at %u; %u records were dropped before they were read",
             static_cast<unsigned>(from_index), static_cast<unsigned>(s_base),
             static_cast<unsigned>(s_base), static_cast<unsigned>(s_base - from_index));
    start = s_base;
  }
  if (out_first_index != nullptr) *out_first_index = start;

  FILE* f = std::fopen(kLogPath, "rb");
  if (!f) return 0;  // nothing logged yet — not an error

  // A seek past EOF simply reads zero records, which is the correct "nothing
  // above the high-water mark" answer.
  const long offset = static_cast<long>(start - s_base) * sizeof(TempRecord);
  if (std::fseek(f, offset, SEEK_SET) != 0) {
    std::fclose(f);
    return 0;
  }
  const size_t n = std::fread(out, sizeof(TempRecord), max, f);
  std::fclose(f);

  // Every read funnels through here, so this one hook is the whole migration:
  // callers above this line never have to know v1 records exist.
  for (size_t i = 0; i < n; ++i) upgrade_in_place(&out[i]);
  return n;
}

size_t read_recent(TempRecord* out, size_t max) {
  if (!s_mounted || out == nullptr || max == 0) return 0;

  const uint32_t have = count();
  if (have == 0) return 0;
  const uint32_t want = (have < max) ? have : static_cast<uint32_t>(max);
  return read_from(next_index() - want, out, want, nullptr);
}

esp_err_t force_rotate(uint32_t drop_count) {
  if (!s_mounted) return ESP_ERR_INVALID_STATE;
  Guard g;
  if (!g.ok) return ESP_ERR_TIMEOUT;
  return rotate_locked(drop_count);
}

esp_err_t erase_all() {
  if (!s_mounted) return ESP_ERR_INVALID_STATE;

  Guard g;
  if (!g.ok) return ESP_ERR_TIMEOUT;

  unlink(kLogTmpPath);
  if (file_exists(kLogPath) && unlink(kLogPath) != 0) {
    ESP_LOGE(kTag, "erase: unlink %s failed", kLogPath);
    return ESP_FAIL;
  }
  if (!nvs_write_pair(kNvsKeyBase, 0, kNvsKeyPending, 0)) {
    ESP_LOGE(kTag, "erase: could not reset the base index");
    return ESP_FAIL;
  }
  s_base = 0;
  s_records = 0;
  ESP_LOGW(kTag, "log erased; numbering resumes above the upload mark once cloud "
                 "sync next looks");
  return ESP_OK;
}

esp_err_t raise_index_floor(uint32_t floor) {
  if (!s_mounted) return ESP_ERR_INVALID_STATE;
  Guard g;
  if (!g.ok) return ESP_ERR_TIMEOUT;
  if (floor <= s_base + s_records) return ESP_OK;
  // Rebased to the floor itself, so anything written since the loss is renumbered
  // above the mark and still gets sent.
  if (!nvs_write_pair(kNvsKeyBase, floor, kNvsKeyPending, 0)) return ESP_FAIL;
  ESP_LOGW(kTag, "log is behind the upload mark (%u < %u); numbering resumes at %u",
           static_cast<unsigned>(s_base + s_records), static_cast<unsigned>(floor),
           static_cast<unsigned>(floor));
  s_base = floor;
  return ESP_OK;
}

void dump() {
  if (!s_mounted) {
    ESP_LOGW(kTag, "dump: storage not mounted");
    return;
  }

  // Stream in fixed batches so the whole log dumps with no print cap and no
  // per-record allocation, regardless of length.
  constexpr size_t kBatch = 32;
  TempRecord buf[kBatch];
  uint32_t idx = first_index();
  const uint32_t end = next_index();
  while (idx < end) {
    uint32_t got_first = 0;
    const size_t got = read_from(idx, buf, kBatch, &got_first);
    if (got == 0) break;
    for (size_t i = 0; i < got; ++i) {
      const TempRecord& r = buf[i];
      if (r.temp_c100 == kInvalidTempC100) {
        ESP_LOGI(kTag, "dump[%u]: ver=%u FAULT flags=0x%02x epoch=%u up_s=%u",
                 static_cast<unsigned>(got_first + i), static_cast<unsigned>(r.version),
                 static_cast<unsigned>(r.flags), static_cast<unsigned>(r.epoch_s),
                 static_cast<unsigned>(r.uptime_s));
      } else {
        ESP_LOGI(kTag,
                 "dump[%u]: ver=%u mean=%.2f min=%.2f max=%.2f flags=0x%02x "
                 "epoch=%u up_s=%u",
                 static_cast<unsigned>(got_first + i), static_cast<unsigned>(r.version),
                 static_cast<double>(c100_to_temp(r.temp_c100)),
                 static_cast<double>(c100_to_temp(r.min_c100)),
                 static_cast<double>(c100_to_temp(r.max_c100)),
                 static_cast<unsigned>(r.flags), static_cast<unsigned>(r.epoch_s),
                 static_cast<unsigned>(r.uptime_s));
      }
    }
    idx = got_first + got;
  }
  ESP_LOGI(kTag, "dump complete: %u records, absolute range [%u, %u)",
           static_cast<unsigned>(count()), static_cast<unsigned>(first_index()),
           static_cast<unsigned>(end));
}

}  // namespace thermopost::storage
