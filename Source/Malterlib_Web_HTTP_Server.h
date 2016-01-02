// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

namespace NMib
{

	namespace NHTTP
	{

		class CServer
		{
			private:
				class CDetails;
				NPtr::TCUniquePointer<CDetails> mp_pD;

			public:
				struct CConfig
				{
					uint16 m_Port;
				};

			public:
				CServer(CConfig const& _Config);
				~CServer();

				bint f_Start(NStr::CStr& _oError);
				void f_Stop();
		};

	} // Namespace NHTTP

} // Namespace NMib
