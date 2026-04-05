// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Concurrency/DistributedApp>

namespace NMib::NWeb
{
	NConcurrency::TCActor<NConcurrency::CDistributedAppActor> fg_ConstructApp_WebSocketEcho();
}
