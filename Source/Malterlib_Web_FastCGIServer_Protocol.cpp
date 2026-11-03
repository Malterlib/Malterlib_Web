// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>

#include "Malterlib_Web_FastCGIServer_Protocol.h"

namespace NMib::NWeb::NFastCGI
{
	CEndRequestBody::CEndRequestBody()
		: m_AppStatus(0)
		, m_ProtocolStatus(EEndRequestStatus_RequestComplete)
	{
		NMemory::fg_MemClear(m_Reserved);
	}
}
