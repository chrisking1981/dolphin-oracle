// Copyright 2026 Dolphin Oracle Project (chrisking1981)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// See DolphinOracle.h for the design rationale.
//
// Hello-world build: confirm that DolphinLib picks up our new translation
// unit and that the Init/Shutdown hooks fire from Core::Init/Stop.

#include "Core/DolphinOracle.h"

#include "Common/Logging/Log.h"
#include "Core/System.h"

namespace DolphinOracle
{

void Init([[maybe_unused]] Core::System& system)
{
  NOTICE_LOG_FMT(CORE,
                 "[DolphinOracle] Oracle alive -- hello-world build, "
                 "TCP server stub not yet started. Version v0.1");
}

void Shutdown()
{
  NOTICE_LOG_FMT(CORE, "[DolphinOracle] Oracle shutdown");
}

}  // namespace DolphinOracle
