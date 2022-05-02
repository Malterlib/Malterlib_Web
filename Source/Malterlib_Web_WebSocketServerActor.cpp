// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorSubscription>

#include "Malterlib_Web_WebSocket.h"
#include "Malterlib_Web_WebSocketServerActor_Internal.h"
#include "Malterlib_Web_WebSocketServerActor_Internal_Listen.h"

#include <Mib/Network/Sockets/TCP>

namespace NMib::NWeb
{
	using namespace NWebSocket;
	CWebSocketServerActor::CWebSocketServerActor()
		: mp_pInternal(fg_Construct(this))
	{
	}

	CWebSocketServerActor::~CWebSocketServerActor()
	{
	}

	void CWebSocketServerActor::f_SetDefaultMaxMessageSize(mint _MaxMessageSize)
	{
		DMibRequire(mp_pInternal->m_ListenSockets.f_IsEmpty());
		mp_pInternal->m_MaxMessageSize = _MaxMessageSize;
	}

	void CWebSocketServerActor::f_SetDefaultFragmentationSize(mint _FragmentationSize)
	{
		DMibRequire(mp_pInternal->m_ListenSockets.f_IsEmpty());
		mp_pInternal->m_FragmentationSize = _FragmentationSize;
	}

	void CWebSocketServerActor::f_SetDefaultTimeout(fp64 _Timeout)
	{
		DMibRequire(mp_pInternal->m_ListenSockets.f_IsEmpty());
		mp_pInternal->m_Timeout = _Timeout;
	}

	void CWebSocketServerActor::f_Debug_SetBroken(bool _bBroken)
	{
		mp_pInternal->m_bBroken = _bBroken;
	}

	void CWebSocketServerActor::CInternal::f_Clear()
	{
		m_ListenSockets.f_Clear();
		m_fOnNewConnection.f_Clear();
		m_fOnFailedConnection.f_Clear();
	}

	auto CWebSocketServerActor::f_StartListenAddress
		(
			NContainer::TCVector<NNetwork::CNetAddress> &&_AddressesToListenTo
			, NMib::NNetwork::ENetFlag _ListenFlags
			, NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (CWebSocketNewServerConnection && _Connection)> &&_fNewConnection
			, NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (CWebSocketActor::CConnectionInfo && _ConnectionInfo)> &&_fFailedConnection
			, NNetwork::FVirtualSocketFactory &&_SocketFactory
		)
		-> NConcurrency::TCFuture<CListenResult>
	{
		NNetwork::FVirtualSocketFactory SocketFactory = fg_Move(_SocketFactory);
		if (!SocketFactory)
			SocketFactory = NNetwork::CSocket_TCP::fs_GetFactory();

		try
		{
			if (!mp_pInternal->m_ListenSockets.f_IsEmpty())
				DMibError("Socket server is already listening");

			mp_pInternal->f_Clear();
			mp_pInternal->m_fOnNewConnection = fg_Move(_fNewConnection);
			mp_pInternal->m_fOnFailedConnection = fg_Move(_fFailedConnection);

			CListenResult ListenResults;

			ListenResults.m_Subscription = NConcurrency::g_ActorSubscription / [this]() -> NConcurrency::TCFuture<void>
				{
					auto &Internal = *mp_pInternal;

					NConcurrency::TCActorResultVector<void> DestroyResults;
					fg_Move(Internal.m_fOnNewConnection).f_Destroy() > DestroyResults.f_AddResult();
					fg_Move(Internal.m_fOnFailedConnection).f_Destroy() > DestroyResults.f_AddResult();

					co_await DestroyResults.f_GetResults();

					co_return {};
				}
			;
			
			NConcurrency::TCActorResultVector<void> SetSocketResults;
			{

				mint nListenTo = _AddressesToListenTo.f_GetLen();

				mp_pInternal->m_ListenSockets.f_SetLen(nListenTo);

				for (mint i = 0; i < nListenTo; ++i)
				{
					NNetwork::CNetAddress &Address = _AddressesToListenTo[i];

					NConcurrency::TCActor<CListenActor> &ListenActor = mp_pInternal->m_ListenSockets[i];
					ListenActor = NConcurrency::fg_ConstructActor<CListenActor>(fg_ThisActor(this), mp_pInternal->m_MaxMessageSize, mp_pInternal->m_FragmentationSize, mp_pInternal->m_Timeout);

					NConcurrency::TCWeakActor<CListenActor> WeakListenActor = ListenActor;

					NStorage::TCUniquePointer<NNetwork::ICSocket> pListenSocket = SocketFactory("");
					NException::CDisableExceptionTraceScope DisableExceptionTrace;
					pListenSocket->f_Listen
						(
							Address
							, [WeakListenActor](NNetwork::ENetTCPState _StateAdded)
							{
								auto ListenActor = WeakListenActor.f_Lock();
								if (!ListenActor)
									return;
								ListenActor(&CListenActor::f_StateAdded, _StateAdded) > NConcurrency::fg_DiscardResult();
							}
							, _ListenFlags
						)
					;

					ListenResults.m_ListenPorts.f_Insert(pListenSocket->f_GetListenPort());

					ListenActor(&CListenActor::f_SetSocket, fg_Move(pListenSocket)) > SetSocketResults.f_AddResult();
				}
			}

			co_await SetSocketResults.f_GetResults() | NConcurrency::g_Unwrap;

			co_return fg_Move(ListenResults);
		}
		catch (NException::CException const &_Exception)
		{
			mp_pInternal->f_Clear();
			co_return _Exception.f_ExceptionPointer();
		}

		DMibNeverGetHere;
		co_return {};
	}

	auto CWebSocketServerActor::f_StartListen
		(
			uint16 _StartListen
			, uint16 _nListen
			, NMib::NNetwork::ENetFlag _ListenFlags
			, NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (CWebSocketNewServerConnection && _Connection)> &&_fNewConnection
			, NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (CWebSocketActor::CConnectionInfo && _ConnectionInfo)> &&_fFailedConnection
			, NNetwork::FVirtualSocketFactory &&_SocketFactory
		)
		-> NConcurrency::TCFuture<CListenResult>
	{
		NContainer::TCVector<NNetwork::CNetAddress> AddressesToListenTo;
		for (mint i = 0; i < _nListen; ++i)
		{
			NNetwork::CNetAddressTCPv4 AnyAddress;
			AnyAddress.m_Port = _StartListen + i;
			NNetwork::CNetAddress Address;
			Address.f_Set(AnyAddress);
			AddressesToListenTo.f_Insert(fg_Move(Address));
		}
		return f_StartListenAddress(fg_Move(AddressesToListenTo), _ListenFlags, fg_Move(_fNewConnection), fg_Move(_fFailedConnection), fg_Move(_SocketFactory));
	}

	NConcurrency::TCFuture<void> CWebSocketServerActor::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;
		NConcurrency::TCActorResultVector<void> Results;
		for (auto &ListenSocket : Internal.m_ListenSockets)
			ListenSocket.f_Destroy() > Results.f_AddResult();

		for (auto &Connection : Internal.m_Subscriptions)
		{
			if (Connection)
				Connection->f_Destroy() > Results.f_AddResult();
		}

		Internal.m_Subscriptions.f_Clear();

		fg_Move(Internal.m_fOnNewConnection).f_Destroy() > Results.f_AddResult();
		fg_Move(Internal.m_fOnFailedConnection).f_Destroy() > Results.f_AddResult();

		co_await Results.f_GetResults() | NConcurrency::g_Unwrap;

		co_return {};
	}

	void CWebSocketServerActor::fp_AddConnection(NConcurrency::TCActor<CWebSocketActor> &&_Connection)
	{
		if (f_IsDestroyed() || mp_pInternal->m_bBroken)
			return;

		auto pSubscription = &mp_pInternal->m_Subscriptions.f_Insert();
		NStorage::TCSharedPointer<NAtomic::TCAtomic<bool>> pHandled = fg_Construct(false);
		_Connection
			(
				&CWebSocketActor::fp_OnFinishServerConnection
				, NConcurrency::g_ActorFunctorWeak / [this, _Connection, pHandled, pSubscription, AllowDestroy = NConcurrency::g_AllowWrongThreadDestroy]
				(CWebSocketActor::EFinishConnectionResult _Result, CWebSocketActor::CConnectionInfo &&_ConnectionInfo) mutable -> NConcurrency::TCFuture<void>
				{
					if (*pHandled || f_IsDestroyed())
						co_return {};

					switch (_Result)
					{
					case CWebSocketActor::EFinishConnectionResult_Error:
						{
							if (mp_pInternal->m_fOnFailedConnection)
								mp_pInternal->m_fOnFailedConnection(fg_Move(_ConnectionInfo)) > NConcurrency::fg_DiscardResult();
							pSubscription->f_Clear();
						}
						break;
					case CWebSocketActor::EFinishConnectionResult_Success:
						{
							auto Protocols = _ConnectionInfo.m_Protocols;
							CWebSocketNewServerConnection Connection(fg_Move(_ConnectionInfo), fg_Move(Protocols), _Connection);
							if (mp_pInternal->m_fOnNewConnection)
								mp_pInternal->m_fOnNewConnection(fg_Move(Connection)) > NConcurrency::fg_DiscardResult();
							pSubscription->f_Clear();
						}
						break;
					default:
						DMibPDebugBreak;
						break;
					}

					*pHandled = true;
					mp_pInternal->m_Subscriptions.f_Remove(*pSubscription);

					co_return {};
				}
			)
			> [this, pSubscription, pHandled](NConcurrency::TCAsyncResult<NConcurrency::CActorSubscription> &&_Result)
			{
				if (!*pHandled && !f_IsDestroyed())
					*pSubscription = fg_Move(*_Result);
			}
		;
	}
}
