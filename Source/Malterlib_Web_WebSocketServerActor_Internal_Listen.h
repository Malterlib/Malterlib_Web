// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Concurrency/ConcurrencyDefines>
#include <Mib/Concurrency/WeakActor>

#include "Malterlib_Web_WebSocket.h"

namespace NMib::NWeb::NWebSocket
{
	class CListenActor : public NConcurrency::CActor
	{
	public:
		// Runs the accept path in the connection actors' priority class, so accepts keep
		// pace with established connections and stay in the pool whose loops carry the sockets
		static constexpr NConcurrency::EPriority mc_Priority = CWebSocketActor::mc_Priority;

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
