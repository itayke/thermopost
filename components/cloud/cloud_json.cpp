#include "cloud_json.hpp"

#include <cstdio>
#include <cstdlib>

namespace thermopost::cloud::json {
namespace {

// Append `"key":` to out.
void key(std::string& out, const char* k) {
  out += '"';
  out += k;
  out += "\":";
}

void int_field(std::string& out, const char* k, long long v) {
  key(out, k);
  out += std::to_string(v);
}

void bool_field(std::string& out, const char* k, bool v) {
  key(out, k);
  out += v ? "true" : "false";
}

// A temperature field, or `null` when the interval produced no valid reading.
void temp_field(std::string& out, const char* k, int16_t c100) {
  key(out, k);
  out += (c100 == kInvalidTempC100) ? "null" : fmt_c100(c100);
}

// A battery figure, or `null` when the post has no usable reading. Same choice as
// temp_field above: a sentinel on the wire is a number some reader eventually
// charts.
void batt_field(std::string& out, const char* k, int32_t v) {
  key(out, k);
  if (v < 0) {
    out += "null";
  } else {
    out += std::to_string(v);
  }
}

void str_field(std::string& out, const char* k, const std::string& v) {
  key(out, k);
  out += '"';
  out += escape(v);
  out += '"';
}

// A string, or `null` when it is empty.
void opt_str_field(std::string& out, const char* k, const std::string& v) {
  if (v.empty()) {
    key(out, k);
    out += "null";
  } else {
    str_field(out, k, v);
  }
}

}  // namespace

std::string escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (c < 0x20) {
          char u[8];
          std::snprintf(u, sizeof(u), "\\u%04x", c);
          out += u;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

std::string fmt_c100(int16_t c100) {
  // The sign is carried separately so -5 renders as "-0.05" rather than "0.-5":
  // C division truncates toward zero, so the whole part of a value between -99
  // and 0 is 0 and loses the minus entirely.
  const bool neg = c100 < 0;
  // Via int32_t, because -INT16_MIN does not fit in an int16_t. The invalid
  // sentinel is exactly that value, and callers are expected to have filtered
  // it — but overflowing here would be undefined behaviour rather than a wrong
  // number, so the widening is not optional.
  const int32_t mag = neg ? -static_cast<int32_t>(c100) : static_cast<int32_t>(c100);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%s%ld.%02ld", neg ? "-" : "",
                static_cast<long>(mag / 100), static_cast<long>(mag % 100));
  return std::string(buf);
}

void append_record(std::string& out, const RecordJson& r) {
  out += '{';
  int_field(out, "index", r.index);
  out += ',';
  int_field(out, "v", r.version);
  out += ',';
  temp_field(out, "c", r.temp_c100);
  out += ',';
  temp_field(out, "min", r.min_c100);
  out += ',';
  temp_field(out, "max", r.max_c100);
  out += ',';
  // Booleans rather than the raw flags byte. The byte is three characters
  // shorter but it is opaque on the wire, and a bit position that disagrees
  // with the Apps Script's idea of it writes silently into the wrong column —
  // a class of bug the reader of a spreadsheet has no way to notice.
  bool_field(out, "high", (r.flags & bits::kFlagAlarmHigh) != 0);
  out += ',';
  bool_field(out, "low", (r.flags & bits::kFlagAlarmLow) != 0);
  out += ',';
  bool_field(out, "fault", (r.flags & bits::kFlagSensorFault) != 0);
  // Omitted, not zeroed: an absent `epoch` says "this record predates the
  // clock", where 1970 would be a timestamp the Sheet would happily plot.
  if (r.epoch_s != 0) {
    out += ',';
    int_field(out, "epoch", r.epoch_s);
  }
  out += ',';
  int_field(out, "up", r.uptime_s);
  out += '}';
}

std::string build_payload(const Envelope& env, const RecordJson* recs, size_t n) {
  std::string out;
  // Roughly what a full batch measures, so the string grows once instead of a
  // dozen times while a backlog is being drained.
  out.reserve(128 + n * 128);
  out += '{';
  str_field(out, "token", env.token);
  out += ',';
  str_field(out, "device", env.device);
  out += ',';
  str_field(out, "name", env.name);
  out += ',';
  batt_field(out, "batt_mv", env.batt_mv);
  out += ',';
  batt_field(out, "batt_pct", env.batt_pct);
  out += ',';
  bool_field(out, "ext", env.external);
  out += ',';
  key(out, "gap_s");
  out += env.gap_s == 0 ? "null" : std::to_string(env.gap_s);
  out += ',';
  opt_str_field(out, "fw", env.firmware);
  out += ',';
  opt_str_field(out, "fw_rejected", env.rejected);
  out += ',';
  // The same shape as the band in the reply, so the two read as one exchange.
  key(out, "band");
  out += '{';
  temp_field(out, "min_c", env.band_min_c100);
  out += ',';
  temp_field(out, "max_c", env.band_max_c100);
  out += "},";
  key(out, "records");
  out += '[';
  for (size_t i = 0; i < n; ++i) {
    if (i) out += ',';
    append_record(out, recs[i]);
  }
  out += "]}";
  return out;
}

}  // namespace thermopost::cloud::json
