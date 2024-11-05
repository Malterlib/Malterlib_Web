// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_WebSocket.h"

namespace NMib::NWeb
{
	///
	/// Shared
	///	======

	CWebSocketNewConnection::CWebSocketNewConnection(NConcurrency::TCActor<CWebSocketActor> const &_Connection)
		: mp_Connection(_Connection)
	{
	}

	///
	/// Server connection
	/// =================

	void CWebSocketNewServerConnection::f_Reject(NStr::CStr const &_Error, NHTTP::CResponseHeader &&_ResponseHeader) const
	{
		if (!mp_pHelper->m_bRepliedToConnection.f_Exchange(true))
			mp_Connection.f_Bind<&CWebSocketActor::fp_RejectServerConnection>(_Error, fg_Move(_ResponseHeader), NStr::CStr()).f_DiscardResult();
	}

	CWebSocketNewServerConnection::CWebSocketNewServerConnection(CWebSocketActor::CConnectionInfo &&_ConnectionInfo, NContainer::TCVector<NStr::CStr> &&_Protocols, NConcurrency::TCActor<CWebSocketActor> const &_Connection)
		: CWebSocketNewConnection(_Connection)
		, m_Info(fg_Move(_ConnectionInfo))
		, m_Protocols(fg_Move(_Protocols))
		, mp_pHelper(fg_Construct(_Connection))
	{

	}

	CWebSocketNewServerConnection::CRepliedHelper::CRepliedHelper(NConcurrency::TCActor<CWebSocketActor> const &_Connection)
		: m_Connection(_Connection)
	{
	}
	CWebSocketNewServerConnection::CRepliedHelper::~CRepliedHelper()
	{
		if (!m_bRepliedToConnection.f_Exchange(true))
			m_Connection.f_Bind<&CWebSocketActor::fp_RejectServerConnection>("Abandoned", NHTTP::CResponseHeader(), NStr::CStr()).f_DiscardResult();
	}
	CWebSocketNewServerConnection::~CWebSocketNewServerConnection()
	{
		mp_pHelper.f_Clear();
	}

	///
	/// Client connection
	/// =================

	void CWebSocketNewClientConnection::f_Reject(NStr::CStr const &_Error) const
	{
		if (!mp_pHelper->m_bRepliedToConnection.f_Exchange(true))
			mp_Connection.f_Bind<&CWebSocketActor::fp_RejectClientConnection>(_Error).f_DiscardResult();
	}

	CWebSocketNewClientConnection::CWebSocketNewClientConnection
		(
			NHTTP::CResponseHeader &&_Response
			, NStr::CStr &&_Protocol
			, NConcurrency::TCActor<CWebSocketActor> const &_Connection
			, NStorage::TCUniquePointer<NNetwork::ICSocketConnectionInfo> &&_pSocketInfo
			, NMib::NNetwork::CNetAddress const &_PeerAddress
		)
		: CWebSocketNewConnection(_Connection)
		, m_Response(fg_Move(_Response))
		, m_Protocol(fg_Move(_Protocol))
		, m_pSocketInfo(fg_Move(_pSocketInfo))
		, m_PeerAddress(_PeerAddress)
		, mp_pHelper(fg_Construct(_Connection))
	{
	}

	CWebSocketNewClientConnection::CWebSocketNewClientConnection(CWebSocketNewClientConnection &&_Other) = default;

	CWebSocketNewClientConnection::~CWebSocketNewClientConnection()
	{
		mp_pHelper.f_Clear();
	}

	CWebSocketNewClientConnection::CRepliedHelper::CRepliedHelper(NConcurrency::TCActor<CWebSocketActor> const &_Connection)
		: m_Connection(_Connection)
	{
	}

	CWebSocketNewClientConnection::CRepliedHelper::~CRepliedHelper()
	{
		if (!m_bRepliedToConnection.f_Exchange(true))
			m_Connection.f_Bind<&CWebSocketActor::fp_RejectClientConnection>("Abandoned").f_DiscardResult();
	}

	bool CWebSocketActor::fs_IsValidCloseStatus(EWebSocketStatus _Status)
	{
		return (_Status >= EWebSocketStatus_NormalClosure && _Status <= EWebSocketStatus_PrivateEnd)
			&& !(_Status >= EWebSocketStatus_ReservedStart && _Status <= EWebSocketStatus_ReservedEnd)
			&& _Status != EWebSocketStatus_NoStatusReceived
			&& _Status != EWebSocketStatus_AbnormalClosure
			&& _Status != EWebSocketStatus_Reserved0
		;
	}
}
