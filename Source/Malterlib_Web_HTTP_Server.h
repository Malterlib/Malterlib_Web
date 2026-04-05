// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>

namespace NMib::NWeb::NHTTP
{
	class CServer
	{
		private:
			class CDetails;
			NStorage::TCUniquePointer<CDetails> mp_pD;

		public:
			struct CConfig
			{
				uint16 m_Port;
			};

		public:
			CServer(CConfig const& _Config);
			~CServer();

			bool f_Start(NStr::CStr& _oError);
			void f_Stop();
	};
}
