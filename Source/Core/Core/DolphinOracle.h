// Copyright 2026 Dolphin Oracle Project (chrisking1981)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// External TCP control server for Dolphin -- inspired by
// TwitchPlaysPokemon/dolphinWatch but rebased on modern Dolphin master.
//
// Listens on a configurable TCP port (default 6000) for newline-delimited
// commands from a Python (or any TCP) client:
//
//   SAVE <abs_path>              -- save savestate to file
//   LOAD <abs_path>              -- load savestate from file
//   READ <8|16|32> <addr_hex>    -- read PowerPC memory
//   WRITE <8|16|32> <addr_hex> <val_hex>
//   SCREENSHOT <abs_path>        -- save current framebuffer (Dolphin Oracle
//                                   extension; original DolphinWatch lacked
//                                   this).
//
// Each command replies with "SUCCESS" or "FAIL" + optional payload on a
// single line.

#pragma once

#include <cstdint>

struct GCPadStatus;

namespace Core
{
class System;
}

namespace DolphinOracle
{
// Default TCP port if [General] OraclePort is unset.
constexpr unsigned short DEFAULT_PORT = 6000;

// Called once after Core::Init() so we can start the server thread.
// Reads the desired port from Dolphin.ini (key [General] OraclePort);
// falls back to DEFAULT_PORT.
void Init(Core::System& system);

// Stop the server thread, close clients. Idempotent.
void Shutdown();

// BUTTONSTATES_GC hook: called from Pad::GetStatus() in the CPU thread.
// If a TCP client has recently sent BUTTONSTATES_GC for this pad, this
// returns true and fills *out_status with the forced state. Otherwise
// returns false and Pad continues with normal input.
//
// Auto-expires after HIJACK_TIMEOUT_MS (500ms) so a missed unhijack
// doesn't permanently steal control from the human player.
bool GetForcedGCPadStatus(int pad_num, GCPadStatus* out_status);

}  // namespace DolphinOracle
