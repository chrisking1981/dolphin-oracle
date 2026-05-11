// Copyright 2026 Dolphin Oracle Project (chrisking1981)
// Based on TwitchPlaysPokemon/dolphinWatch (5.0DW-RC1) protocol.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// External TCP control server for Dolphin. Lets a Python (or any TCP)
// client drive savestate, memory R/W, controller input, screenshots, and
// memory-change subscriptions over a simple newline-delimited protocol.
//
// Hello-world stage: just logs "Oracle alive" at Core::Init() to confirm
// the integration plumbing works. Real TCP server is added in subsequent
// commits.

#pragma once

namespace Core
{
class System;
}

namespace DolphinOracle
{
// Called once after Core::Init() succeeds.
// Starts the TCP server (once the full implementation is in place).
void Init(Core::System& system);

// Called from Core::Stop() / shutdown to release resources.
void Shutdown();
}  // namespace DolphinOracle
