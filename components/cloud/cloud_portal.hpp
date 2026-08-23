#pragma once

#include "cloud.hpp"  // kSsidMax, kPasswordMax, kUrlMax, kTokenMax

#include "esp_err.h"

// The web config surface: one HTML form served over HTTP, and — while the post
// is unprovisioned — the SoftAP and DNS hijack that put a phone in front of it
// without a network to reach it on.
//
// **The two halves have different lifetimes, and that is the whole point of this
// file.** espressopost's portal was one-shot: the AP, the DNS responder and the
// httpd came up together for provisioning and went down together on the first
// GOT_IP. Here the form is also where thresholds, the interval and the post's
// name are edited, so tying it to provisioning would mean rejoining a setup AP
// to adjust a number. So:
//
//   start_server()  the form, for the device's whole life, on every interface
//   start_ap()      SoftAP + DNS hijack, only until credentials land
//   stop_ap()       drops those two; the form keeps serving over the LAN
//
// One httpd serves both, because it binds INADDR_ANY — the AP is a way to reach
// the server, not a second server.
//
// Cost is an always-listening HTTP server that writes settings, unauthenticated,
// over plain HTTP. On a home or brewery LAN that is the same exposure the setup
// AP already accepts, and the blast radius is a temperature threshold. It should
// not grow anything reachable from outside the LAN without revisiting that.
namespace thermopost::cloud::portal {

// The submitted fields this component does *not* own. Everything else on the
// form — interval, band, hysteresis, graph window, unit, sound, volume — is
// applied here directly through settings::set_*, because those setters are the
// clamp and the single source of truth and there is nothing to add to them.
//
// These five are different: four are cloud's NVS and the radio's lifecycle, and
// the name has to reach mDNS as well as NVS. They go back to cloud.cpp, which
// owns both.
//
// An empty field means "unchanged" for url and token, "keep the current network"
// for ssid, and "restore the MAC-derived default" for name (set_name's own
// reading of a blank).
struct Submission {
  char name[32];  // sanitised by settings::set_name, not here
  char ssid[kSsidMax];
  char password[kPasswordMax];
  char url[kUrlMax];
  char token[kTokenMax];
};

// Invoked from the httpd task once a valid form arrives, before the response is
// rendered — so a STA connect is already under way as the browser paints the
// confirmation. It must not call stop(): that would tear down the task it is
// running on. cloud.cpp defers the AP teardown to the GOT_IP event instead.
using SubmitCb = void (*)(const Submission&);

// Start the HTTP server and register the form. Idempotent-safe.
esp_err_t start_server(SubmitCb cb);

// Bring up the open SoftAP named `ap_ssid` and the DNS responder that answers
// every lookup with the AP's own address, which is what makes a phone's
// captive-portal probe pop the form.
//
// The radio must already be in a mode that includes AP; it need not be started
// yet — the AP netif is created here if it does not exist, and its address is
// the fixed 192.168.4.1 the default AP netif carries from creation.
esp_err_t start_ap(const char* ap_ssid);

// Drop the DNS responder and mark the setup session finished. The HTTP server
// keeps serving. The AP itself goes down with the Wi-Fi mode, which is cloud.cpp's
// to set — the radio's lifecycle belongs to one file. Safe when the AP is not up.
void stop_ap();

// True while the SoftAP is up. The form reads it to decide whether a network has
// to be chosen (provisioning) or may be left alone (an edit over the LAN), and
// whether a blocking scan is safe to run.
bool ap_active();

// Everything down, server included. For completeness; nothing calls it today.
void stop();

}  // namespace thermopost::cloud::portal
