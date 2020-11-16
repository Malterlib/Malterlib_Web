// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

namespace NMib::NWeb::NHTTP
{
	enum EConnectionState
	{
		EConnectionState_Connected
		, EConnectionState_Disconnected
	};

	class CConnectionWorker;

	class CConnection
	{
		private:
			class CDetails;
			NStorage::TCUniquePointer<CDetails> mp_pD;

			DMibListLinkD_Link(CConnection, mp_Link);

			friend CConnectionWorker;

		public:
			CConnection(NStorage::TCUniquePointer<NNetwork::CSocket> _pSock);
			~CConnection();

			bool f_IsConnected() const;

			void f_SetReportTo(NMib::NThread::CSemaphoreAggregate *_pReportTo);

			void f_Process();
	};
}
