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
		NStr::CStrSecure m_SessionToken; // Only for temporary credentials, for example from an assumed role or instance metadata
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
