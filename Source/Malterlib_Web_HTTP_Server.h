// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>

namespace NMib::NWeb::NHTTP
{
	struct CServer
	{
		struct CConfig
		{
			uint16 m_Port;
		};

	public:
		CServer(CConfig const &_Config);
		~CServer();

		bool f_Start(NStr::CStr &o_Error);
		void f_Stop();

	private:
		struct CDetails;
		NStorage::TCUniquePointer<CDetails> mp_pD;
	};
}
