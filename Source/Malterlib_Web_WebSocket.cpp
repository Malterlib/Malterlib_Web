// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_WebSocket.h"

namespace NMib::NWeb
{
	///
	/// Shared
	///	======

	umint CWebsocketSettings::f_GetSendWindowBytes() const
	{
		if (m_SendWindowBytes)
			return fg_Min(m_SendWindowBytes, NNetwork::gc_SocketMaxSendWindowBytes);

		// Saturating: eight frames of a fragmentation near a 32 bit umint's limit would wrap
		return 8 * fg_Min(f_GetSendWindowStartBytes(), TCLimitsInt<umint>::mc_Max / 8);
	}

	umint CWebsocketSettings::f_GetSendWindowStartBytes() const
	{
		// Saturating: a fragmentation near a 32 bit umint's limit would wrap with the margin
		return fg_Min(fg_Max(m_FragmentationSize, umint(4096)), TCLimitsInt<umint>::mc_Max - NNetwork::gc_SocketFramingMargin) + NNetwork::gc_SocketFramingMargin;
	}

	CWebSocketNewConnection::CWebSocketNewConnection(NConcurrency::TCActor<CWebSocketActor> const &_Connection, umint _FragmentationSize, umint _MaxFragmentSize)
		: m_FragmentationSize(_FragmentationSize)
		, m_MaxFragmentSize(_MaxFragmentSize)
		, mp_Connection(_Connection)
	{
	}

	///
	/// Listen socket factory
	/// =====================

	CWebSocketListenSocketFactory::CWebSocketListenSocketFactory(NNetwork::FVirtualSocketFactory &&_Factory)
		: m_Factory(fg_Move(_Factory))
	{
	}

	CWebSocketListenSocketFactory::CWebSocketListenSocketFactory(NNetwork::FVirtualSocketFactory const &_Factory)
		: m_Factory(_Factory)
	{
	}

	CWebSocketListenSocketFactory CWebSocketListenSocketFactory::fs_PerAddress(NFunction::TCFunction<CWebSocketListenAddressConfig (umint _iAddress, NMib::NNetwork::CNetAddress const &_Address)> &&_fSelector)
	{
		CWebSocketListenSocketFactory Return;
		Return.m_fSelector = fg_Move(_fSelector);

		return Return;
	}

	bool CWebSocketListenSocketFactory::f_HasSelector() const
	{
		return bool(m_fSelector);
	}

	// Only meaningful for the per-address selector form. For the plain factory form callers must
	// invoke m_Factory itself rather than the copy a config would carry: copying a TCFunction
	// duplicates its captured callable, which would reset a stateful factory for every address
	CWebSocketListenAddressConfig CWebSocketListenSocketFactory::f_GetConfig(umint _iAddress, NMib::NNetwork::CNetAddress const &_Address) const
	{
		if (m_fSelector)
			return m_fSelector(_iAddress, _Address);

		return CWebSocketListenAddressConfig{.m_Factory = m_Factory};
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
		: CWebSocketNewConnection(_Connection, _ConnectionInfo.m_FragmentationSize, _ConnectionInfo.m_MaxFragmentSize)
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
			, umint _FragmentationSize
			, umint _MaxFragmentSize
		)
		: CWebSocketNewConnection(_Connection, _FragmentationSize, _MaxFragmentSize)
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
