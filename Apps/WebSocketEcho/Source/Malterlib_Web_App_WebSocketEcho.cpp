// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>
#include <Mib/Network/Sockets/TCP>

#include "Malterlib_Web_App_WebSocketEcho.h"

namespace NMib::NWeb::NWebSocketEcho
{
	CWebSocketEchoActor::CWebSocketEchoActor()
		: CDistributedAppActor(CDistributedAppActor_Settings("WebSocketEcho").f_AuditCategory("Malterlib/Web/WebSocketEcho"))
	{
	}

	CWebSocketEchoActor::~CWebSocketEchoActor() = default;

	TCFuture<void> CWebSocketEchoActor::fp_StartApp(CEJsonSorted const &_Params)
	{
		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					if (mp_State.m_bStoppingApp || f_IsDestroyed())
						return DMibErrorInstance("Startup aborted");
					return {};
				}
			)
		;

		m_WebsocketServer = fg_Construct();
		auto ListenResult = co_await m_WebsocketServer
			(
				&CWebSocketServerActor::f_StartListen
				, 9001
				, 1
				, ENetFlag_None
				, g_ActorFunctorWeak / [this](CWebSocketNewServerConnection &&_Connection) -> TCFuture<void>
				{
					DMibLog(Info, "New connection '{}': {vs}", _Connection.m_Info.m_PeerAddress, _Connection.m_Protocols);

					auto SocketID = m_iSocketId++;
					auto Address = _Connection.m_Info.m_PeerAddress;

					_Connection.m_fOnReceiveBinaryMessage = g_ActorFunctorWeak / [this, SocketID, Address](TCSharedPointer<CSecureByteVector> const &_pMessage) -> TCFuture<void>
						{
							DMibLog(Info, "{} Binary '{}': {}", SocketID, Address, _pMessage->f_GetLen());
							auto *pClient = m_Clients.f_FindEqual(SocketID);
							if (pClient)
								co_await pClient->m_WebSocket(&CWebSocketActor::f_SendBinary, _pMessage, 0);

							co_return {};
						}
					;
					_Connection.m_fOnReceiveTextMessage = g_ActorFunctorWeak / [this, SocketID, Address](CStr const &_Message) -> TCFuture<void>
						{
							DMibLog(Info, "{} Text '{}': {}", SocketID, Address, _Message.f_GetLen());
							auto *pClient = m_Clients.f_FindEqual(SocketID);
							if (pClient)
								co_await pClient->m_WebSocket(&CWebSocketActor::f_SendText, _Message, 0);

							co_return {};
						}
					;
					_Connection.m_fOnReceivePing = g_ActorFunctorWeak / [this, SocketID, Address](TCSharedPointer<CSecureByteVector> const &_ApplicationData) -> TCFuture<void>
						{
							DMibLog(Info, "{} Ping '{}': {}", SocketID, Address, _ApplicationData->f_GetLen());
							auto *pClient = m_Clients.f_FindEqual(SocketID);
							if (pClient)
								co_await pClient->m_WebSocket(&CWebSocketActor::f_SendPong, _ApplicationData);

							co_return {};
						}
					;
					_Connection.m_fOnReceivePong = g_ActorFunctorWeak / [SocketID, Address](TCSharedPointer<CSecureByteVector> const &_ApplicationData) -> TCFuture<void>
						{
							DMibLog(Info, "{} Pong '{}': {}", SocketID, Address, _ApplicationData->f_GetLen());
							co_return {};
						}
					;
					_Connection.m_fOnClose = g_ActorFunctorWeak / [this, SocketID, Address](EWebSocketStatus _Reason, CStr const &_Message, EWebSocketCloseOrigin _Origin) -> TCFuture<void>
						{
							DMibLog(Info, "{} Close '{}': {}", SocketID, Address, _Message);
							auto *pClient = m_Clients.f_FindEqual(SocketID);
							if (pClient)
							{
								TCActorResultVector<void> Destroys;

								if (pClient->m_Subscription)
									pClient->m_Subscription->f_Destroy() > Destroys.f_AddResult();

								fg_Move(pClient->m_WebSocket).f_Destroy() > Destroys.f_AddResult();

								m_Clients.f_Remove(pClient);

								co_await Destroys.f_GetResults();
							}

							co_return {};
						}
					;

					NHTTP::CResponseHeader Header;
					Header.f_GetEntityFields().f_SetUnknownField("User-Agent", "MalterlibWebSocketEcho");

					m_Clients[SocketID].m_WebSocket = _Connection.f_Accept
						(
							_Connection.m_Protocols.f_IsEmpty() ? CStr() : _Connection.m_Protocols[0]
							, fg_ThisActor(this) / [this, SocketID, Address](TCAsyncResult<CActorSubscription> &&_Subscription)
							{
								if (!_Subscription)
								{
									DMibLog(Info, "{} Failed to accept connection '{}': {}", SocketID, Address, _Subscription.f_GetExceptionStr());
									m_Clients.f_Remove(SocketID);
									return;
								}
								auto *pClient = m_Clients.f_FindEqual(SocketID);
								if (pClient)
								{
									DMibLog(Info, "{} Accepted connection '{}'", SocketID, Address);
									pClient->m_Subscription = fg_Move(*_Subscription);
								}
							}
							, fg_Move(Header)
						)
					;

					co_return {};
				}
				, g_ActorFunctorWeak / [](CWebSocketActor::CConnectionInfo &&_ConnectionInfo) -> TCFuture<void>
				{
					DMibLog(Info, "Failed connection '{}': {}", _ConnectionInfo.m_PeerAddress, _ConnectionInfo.m_Error);

					co_return {};
				}
				, CSocket_TCP::fs_GetFactory()
			)
		;
		m_ListenSubscription = fg_Move(ListenResult.m_Subscription);

		co_return {};
	}

	TCFuture<void> CWebSocketEchoActor::fp_StopApp()
	{
		TCActorResultVector<void> Destroys;

		co_await Destroys.f_GetResults();

		co_return {};
	}
}

namespace NMib::NWeb
{
	TCActor<CDistributedAppActor> fg_ConstructApp_WebSocketEcho()
	{
		return fg_Construct<NWebSocketEcho::CWebSocketEchoActor>();
	}
}
