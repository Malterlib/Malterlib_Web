// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Concurrency/ConcurrencyDefines>
#include <Mib/Concurrency/WeakActor>

#include "Malterlib_Web_WebSocket.h"

namespace NMib::NWeb::NWebSocket
{
	class CListenActor : public NConcurrency::CActor
	{
	public:
		CListenActor(NConcurrency::TCActor<CWebSocketServerActor> const& _Server, CWebsocketSettings const &_Settings);
		~CListenActor();

		void f_SetSocket(NStorage::TCUniquePointer<NNetwork::ICSocket> _pSocket);
		void f_StateAdded(NNetwork::ENetTCPState _StateAdded);

	private:
		NConcurrency::TCFuture<void> fp_Destroy();
		void fp_ProcessState();

	private:
		NStorage::TCUniquePointer<NNetwork::ICSocket> mp_pSocket;
		CWebsocketSettings mp_Settings;
		NConcurrency::TCWeakActor<CWebSocketServerActor> mp_Server;
	};
}
