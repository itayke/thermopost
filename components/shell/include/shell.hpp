#pragma once

#include "esp_err.h"

// The serial REPL, and nothing else.
//
// espressopost put its REPL inside `cloud`, with a comment admitting why: cloud
// was the only component that wanted commands, so it ended up owning the console
// for everything. That is already untrue here — the log, the settings and the
// alarm all have things worth poking from a terminal, and none of them should
// have to depend on the network stack to get a command registered.
//
// So the REPL is its own component and owns only its lifetime. Commands are
// registered by whoever implements them, with esp_console_cmd_register directly;
// there is no wrapper here, because there is nothing to add to it.
//
// Named `shell` rather than the obvious `console` because IDF already has a
// component by that name — a local one shadows it, and the failure is
// esp_console.h suddenly not existing.
namespace thermopost::shell {

// Start the REPL on the USB-Serial-JTAG port. Non-fatal: a device with no
// console still works, it just cannot be talked to.
//
// **Must run before any component registers a command** — esp_console's command
// list is created here, and a registration before it returns
// ESP_ERR_INVALID_STATE. app_main sequences that; ready() is how a component
// checks rather than assumes.
esp_err_t init();

// True once init() has succeeded, i.e. once esp_console_cmd_register will work.
bool ready();

}  // namespace thermopost::shell
