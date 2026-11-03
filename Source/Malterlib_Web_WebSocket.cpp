// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_WebSocket.h"

namespace NMib::NWeb
{
	///
	/// Shared
	///	======

	// Configured send ceiling, or eight frames when unconfigured.
	umint CWebsocketSettings::f_GetSendWindowBytes() const
	{
		if (m_SendWindowBytes)
			return fg_Min(m_SendWindowBytes, NNetwork::gc_SocketMaxSendWindowBytes);

		// Saturating: eight frames of a fragmentation near a 32 bit umint's limit would wrap
		return 8 * fg_Min(f_GetSendWindowStartBytes(), TCLimitsInt<umint>::mc_Max / 8);
	}

	// Initial one-frame window; growth depends on measured path capacity.
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

	CWebSocketListenSocketFactory::CWebSocketListenSocketFactory(NNetwork::FVirtualSocketFactory &&_fFactory)
		: m_fFactory(fg_Move(_fFactory))
	{
	}

	CWebSocketListenSocketFactory::CWebSocketListenSocketFactory(NNetwork::FVirtualSocketFactory const &_fFactory)
		: m_fFactory(_fFactory)
	{
	}

	auto CWebSocketListenSocketFactory::fs_PerAddress(NFunction::TCFunction<CWebSocketListenAddressConfig (umint _iAddress, NMib::NNetwork::CNetAddress const &_Address)> &&_fSelector)
		-> CWebSocketListenSocketFactory
	{
		CWebSocketListenSocketFactory Return;
		Return.m_fSelector = fg_Move(_fSelector);

		return Return;
	}

	bool CWebSocketListenSocketFactory::f_HasSelector() const
	{
		return bool(m_fSelector);
	}

	// For plain factories, invoke the retained callable directly; copying TCFunction would reset captured state per address.
	CWebSocketListenAddressConfig CWebSocketListenSocketFactory::f_GetConfig(umint _iAddress, NMib::NNetwork::CNetAddress const &_Address) const
	{
		if (m_fSelector)
			return m_fSelector(_iAddress, _Address);

		return CWebSocketListenAddressConfig{.m_fFactory = m_fFactory};
	}

	///
	/// Server connection
	/// =================

	void CWebSocketNewServerConnection::f_Reject(NStr::CStr const &_Error, NHTTP::CResponseHeader &&_ResponseHeader) const
	{
		if (!mp_pHelper->m_bRepliedToConnection.f_Exchange(true))
			mp_Connection.f_Bind<&CWebSocketActor::fp_RejectServerConnection>(_Error, fg_Move(_ResponseHeader), NStr::CStr()).f_DiscardResult();
	}

	CWebSocketNewServerConnection::CWebSocketNewServerConnection
		(
			CWebSocketActor::CConnectionInfo &&_ConnectionInfo
			, NContainer::TCVector<NStr::CStr> &&_Protocols
			, NConcurrency::TCActor<CWebSocketActor> const &_Connection
		)
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

	CWebSocketNewServerConnection::CWebSocketNewServerConnection(CWebSocketNewServerConnection &&_Other)
		: CWebSocketNewConnection(fg_Move(_Other))
		, m_Info(fg_Move(_Other.m_Info))
		, m_Protocols(fg_Move(_Other.m_Protocols))
		, mp_pHelper(fg_Move(_Other.mp_pHelper))
	{
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

	CWebSocketActor::CConnectionInfo::CConnectionInfo()
		: m_pRequest(fg_Construct())
	{
	}

	CWebSocketActor::CClientConnectionInfo::CClientConnectionInfo()
		: m_pResponse(fg_Construct())
	{
	}

	CWebSocketActor::CMaybeSecureByteVector::CMaybeSecureByteVector(NContainer::CByteVector &&_Vector)
		: NStorage::TCVariant<NContainer::CByteVector, NContainer::CSecureByteVector>{fg_Move(_Vector)}
	{
	}

	CWebSocketActor::CMaybeSecureByteVector::CMaybeSecureByteVector(NContainer::CSecureByteVector &&_Vector)
		: NStorage::TCVariant<NContainer::CByteVector, NContainer::CSecureByteVector>{fg_Move(_Vector)}
	{
	}

	CWebSocketActor::CMaybeSecureByteVector::CMaybeSecureByteVector(NContainer::CSecureByteVector const &_Vector)
		: NStorage::TCVariant<NContainer::CByteVector, NContainer::CSecureByteVector>{_Vector}
	{
	}

	CWebSocketActor::CMaybeSecureByteVector::CMaybeSecureByteVector(NContainer::CByteVector const &_Vector)
		: NStorage::TCVariant<NContainer::CByteVector, NContainer::CSecureByteVector>{_Vector}
	{
	}

	umint CWebSocketActor::CMaybeSecureByteVector::f_GetLen() const
	{
		if (this->f_GetTypeID() == 0)
			return this->f_Get<0>().f_GetLen();
		else
			return this->f_Get<1>().f_GetLen();
	}

	uint8 const *CWebSocketActor::CMaybeSecureByteVector::f_GetArray() const
	{
		if (this->f_GetTypeID() == 0)
			return this->f_Get<0>().f_GetArray();
		else
			return this->f_Get<1>().f_GetArray();
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
