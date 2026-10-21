#include "catch.hpp"  // main() lives in main.cpp

#include "cloud_json.hpp"

using namespace thermopost::cloud::json;

namespace {

// A record with nothing unusual about it, so each test can change the one field
// it is actually about.
RecordJson plain() {
  RecordJson r{};
  r.index     = 7;
  r.version   = 2;
  r.flags     = 0;
  r.temp_c100 = 2135;
  r.min_c100  = 2090;
  r.max_c100  = 2210;
  r.epoch_s   = 1'756'000'000;
  r.uptime_s  = 3600;
  return r;
}

std::string one(const RecordJson& r) {
  std::string out;
  append_record(out, r);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Fixed-point formatting
// ---------------------------------------------------------------------------
// The whole reason fmt_c100 does integer arithmetic instead of dividing into a
// double is that these have to be exact, so they are tested as exact strings.

TEST_CASE("hundredths format with two decimal places", "[json][fmt]") {
  CHECK(fmt_c100(2135) == "21.35");
  CHECK(fmt_c100(0) == "0.00");
  CHECK(fmt_c100(100) == "1.00");
  CHECK(fmt_c100(5) == "0.05");
  CHECK(fmt_c100(12500) == "125.00");
}

TEST_CASE("a negative fraction keeps its sign", "[json][fmt]") {
  // The interesting case: C division truncates toward zero, so -5/100 is 0 and
  // the minus would vanish if the sign were not carried separately.
  CHECK(fmt_c100(-5) == "-0.05");
  CHECK(fmt_c100(-99) == "-0.99");
  CHECK(fmt_c100(-100) == "-1.00");
  CHECK(fmt_c100(-4000) == "-40.00");
}

TEST_CASE("trailing zeros are kept, not trimmed", "[json][fmt]") {
  // 20.90 must not become 20.9: the column is hundredths of a degree and the
  // width is what makes a sheet of them readable.
  CHECK(fmt_c100(2090) == "20.90");
  CHECK(fmt_c100(2200) == "22.00");
}

// ---------------------------------------------------------------------------
// Escaping
// ---------------------------------------------------------------------------

TEST_CASE("ordinary text passes through untouched", "[json][escape]") {
  CHECK(escape("North Tank 3") == "North Tank 3");
  CHECK(escape("") == "");
}

TEST_CASE("quotes and backslashes are escaped", "[json][escape]") {
  CHECK(escape("say \"hi\"") == "say \\\"hi\\\"");
  CHECK(escape("a\\b") == "a\\\\b");
}

TEST_CASE("control characters get their short escapes", "[json][escape]") {
  CHECK(escape("a\nb") == "a\\nb");
  CHECK(escape("a\tb") == "a\\tb");
  CHECK(escape("a\rb") == "a\\rb");
}

TEST_CASE("any other control byte becomes a \\u escape", "[json][escape]") {
  CHECK(escape(std::string("a\x01""b")) == "a\\u0001b");
  CHECK(escape(std::string("\x1f")) == "\\u001f");
}

// ---------------------------------------------------------------------------
// One record
// ---------------------------------------------------------------------------

TEST_CASE("a normal record serializes field for field", "[json][record]") {
  CHECK(one(plain()) ==
        "{\"index\":7,\"v\":2,\"c\":21.35,\"min\":20.90,\"max\":22.10,"
        "\"high\":false,\"low\":false,\"fault\":false,"
        "\"epoch\":1756000000,\"up\":3600}");
}

TEST_CASE("alarm flags become named booleans", "[json][record]") {
  RecordJson r = plain();
  r.flags = bits::kFlagAlarmHigh;
  CHECK(one(r).find("\"high\":true,\"low\":false,\"fault\":false") != std::string::npos);

  r.flags = bits::kFlagAlarmLow;
  CHECK(one(r).find("\"high\":false,\"low\":true,\"fault\":false") != std::string::npos);

  r.flags = bits::kFlagSensorFault;
  CHECK(one(r).find("\"high\":false,\"low\":false,\"fault\":true") != std::string::npos);
}

TEST_CASE("an unread interval serializes as null, not as a sentinel", "[json][record]") {
  // The sentinel is INT16_MIN, which would print as -327.68 — a plausible-looking
  // temperature that a chart would happily plot off the bottom of the axis.
  RecordJson r = plain();
  r.flags = bits::kFlagSensorFault;
  r.temp_c100 = kInvalidTempC100;
  r.min_c100  = kInvalidTempC100;
  r.max_c100  = kInvalidTempC100;
  const std::string s = one(r);
  CHECK(s.find("\"c\":null,\"min\":null,\"max\":null") != std::string::npos);
  CHECK(s.find("-327") == std::string::npos);
}

TEST_CASE("an unset clock omits epoch rather than sending zero", "[json][record]") {
  RecordJson r = plain();
  r.epoch_s = 0;
  const std::string s = one(r);
  CHECK(s.find("epoch") == std::string::npos);
  // The uptime is still there — it is what the sheet orders by in that case.
  CHECK(s.find("\"up\":3600}") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The batch envelope
// ---------------------------------------------------------------------------

TEST_CASE("the envelope carries token, device and name around the records",
          "[json][payload]") {
  const Envelope env{"tok", "a1b2c3d4e5f6", "North Tank", 3812, 62, false, 900};
  const RecordJson recs[] = {plain(), plain()};
  const std::string s = build_payload(env, recs, 2);

  CHECK(s.rfind("{\"token\":\"tok\",\"device\":\"a1b2c3d4e5f6\","
                "\"name\":\"North Tank\",\"batt_mv\":3812,\"batt_pct\":62,"
                "\"ext\":false,\"gap_s\":900,\"fw\":null,\"fw_rejected\":null,"
                "\"records\":[{", 0) == 0);
  CHECK(s.substr(s.size() - 3) == "}]}");
  // Two records, one comma between them.
  CHECK(s.find("},{") != std::string::npos);
}

TEST_CASE("an empty batch still produces valid JSON", "[json][payload]") {
  const Envelope env{"tok", "dev", "post", 4102, 91, true};
  CHECK(build_payload(env, nullptr, 0) ==
        "{\"token\":\"tok\",\"device\":\"dev\",\"name\":\"post\","
        "\"batt_mv\":4102,\"batt_pct\":91,\"ext\":true,\"gap_s\":null,"
        "\"fw\":null,\"fw_rejected\":null,\"records\":[]}");
}

TEST_CASE("a post with no usable battery reading sends null, not a sentinel",
          "[json][payload]") {
  // The default-constructed envelope is the "nothing measured" case: no divider, an
  // ADC that did not answer, or a batch built before power::init() ran. A -1 on the
  // wire is a number some dashboard eventually charts as a real voltage.
  const Envelope env{"tok", "dev", "post"};
  const std::string s = build_payload(env, nullptr, 0);
  CHECK(s.find("\"batt_mv\":null") != std::string::npos);
  CHECK(s.find("\"batt_pct\":null") != std::string::npos);
  // Unknown supply reads as external, which is the safe way round for anything
  // that would raise an alert about a post running down.
  CHECK(s.find("\"ext\":true") != std::string::npos);
}

TEST_CASE("a flat pack still reports its percentage as zero, not as absent",
          "[json][payload]") {
  // 0 % is a measurement and null is the absence of one; a reader that cannot tell
  // them apart would treat every dead post as an unmonitored one.
  const Envelope env{"tok", "dev", "post", 3300, 0, false};
  const std::string s = build_payload(env, nullptr, 0);
  CHECK(s.find("\"batt_mv\":3300,\"batt_pct\":0,\"ext\":false") != std::string::npos);
}

TEST_CASE("an unknown upload gap is null, so the Sheet falls back to its default",
          "[json][payload]") {
  // Zero would read as "never goes quiet", and a post that says so is one whose
  // silence nothing would ever flag.
  const Envelope env{"tok", "dev", "post"};
  CHECK(build_payload(env, nullptr, 0).find("\"gap_s\":null") != std::string::npos);
}

TEST_CASE("a name with a quote in it cannot break out of the envelope",
          "[json][payload]") {
  // settings::set_name() strips non-ASCII but a double quote is printable ASCII
  // and survives it, so the escaping here is the only thing standing between a
  // clumsy name and an unparseable batch.
  const Envelope env{"tok", "dev", "Bob\"s Tank"};
  const std::string s = build_payload(env, nullptr, 0);
  CHECK(s.find("\"name\":\"Bob\\\"s Tank\"") != std::string::npos);
}

TEST_CASE("the envelope names the firmware, and a rolled-back release when there is one",
          "[json][payload]") {
  // The Sheet decides from `fw` whether this post needs an update; null keeps a post
  // on firmware that cannot update from being offered one.
  Envelope env{"tok", "dev", "post"};
  env.firmware = "0.2.0";
  CHECK(build_payload(env, nullptr, 0).find("\"fw\":\"0.2.0\",\"fw_rejected\":null") !=
        std::string::npos);
  env.rejected = "0.3.0";
  CHECK(build_payload(env, nullptr, 0).find("\"fw_rejected\":\"0.3.0\"") != std::string::npos);
}
