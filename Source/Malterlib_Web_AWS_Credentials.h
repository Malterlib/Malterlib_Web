// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

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
