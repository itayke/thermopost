#pragma once

#include <cstddef>

#include "esp_err.h"

// Firmware updates over the air, and the post's own version.
//
// The Sheet names the release each post should run; the upload reply carries it to a
// post that runs something else, and offer() decides whether now is a good time. A
// download is checked against the sha256 the Sheet gave before it is ever booted.
//
// A new image has to prove itself: it boots pending, and only a successful upload
// confirms it. One that cannot upload before its deadline reboots into the image it
// replaced, so a bad release can never strand a post somewhere it cannot be updated from.
// A deep sleep before that point is a reboot too, and also rolls it back, which is why
// the power ladder stays awake while an image is proving itself.
namespace thermopost::ota {

// esp_app_desc_t's own field width, terminator included.
constexpr size_t kVersionMax = 32;
// A GitHub release asset URL with room to spare; its redirect is followed separately.
constexpr size_t kUrlMax = 256;
constexpr size_t kSha256HexLen = 64;

// A release, as the Sheet names it.
struct Target {
  char version[kVersionMax];
  char url[kUrlMax];
  char sha256[kSha256HexLen + 1];  // lowercase or uppercase hex
};

// Classify the running image, arm the proving deadline when it is new, and register the
// `ota` serial command. Requires settings::init() and shell::init(). A quiet wake skips
// it, so offers then are ignored until the post comes up properly.
//
// Safe to call once; ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// The running firmware's version. A build not made by tools/release.sh ends in "-dev".
const char* version();

// The version of the last image that failed to prove itself and was rolled back, or "".
const char* rejected();

// The running image is new and has not yet confirmed itself.
bool proving();

// An upload just succeeded: the self-test a new image has to pass. Cheap otherwise.
void confirm();

// A download is in progress; the uploader holds back until it is done.
bool busy();

// The Sheet's target for this post. Starts a download when the post runs something
// else, is a release build, has the power for it and has no alarm open. Never blocks.
void offer(const Target& t);

}  // namespace thermopost::ota
