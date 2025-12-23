// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedDaemon>
#include <Mib/Web/WebSocket>

namespace NMib::NWeb::NWebSocketEcho
{
	struct CWebSocketEchoActor : public CDistributedAppActor
	{
		CWebSocketEchoActor();
		~CWebSocketEchoActor();

	private:
		using EStatusSeverity = CDistributedAppSensorReporter::EStatusSeverity;

		struct CClientConnection
		{
			TCActor<CWebSocketActor> m_WebSocket;
			CActorSubscription m_Subscription;
		};

		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;

		TCFuture<void> fp_StartApp(CEJsonSorted const _Params) override;
		TCFuture<void> fp_StopApp() override;

		TCActor<CWebSocketServerActor> m_WebsocketServer;
		CActorSubscription m_ListenSubscription;
		mint m_iSocketId = 0;
		TCMap<mint, CClientConnection> m_Clients;
	};
}
