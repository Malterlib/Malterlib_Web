// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NWeb
{
	struct CAwsCredentials
	{
		NStr::CStr m_Region;
		NStr::CStr m_AccessKeyID;
		NStr::CStrSecure m_SecretKey;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
