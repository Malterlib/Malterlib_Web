// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NWeb::NFastCGI
{
	template <typename tf_CStream>
	void CHeader::f_Consume(tf_CStream &_Stream)
	{
		uint8 Version;
		_Stream >> Version;
		m_Version = (ERequestVersion)Version;
		uint8 Type;
		_Stream >> Type;
		m_Type = (ERequestType)Type;
		_Stream >> m_RequestID;
		_Stream >> m_ContentLength;
		_Stream >> m_PaddingLength;
		_Stream >> m_Reserved;
	}

	template <typename tf_CStream>
	void CHeader::f_Feed(tf_CStream &_Stream) const
	{
		uint8 Version = m_Version;
		_Stream << Version;
		uint8 Type = m_Type;
		_Stream << Type;
		_Stream << m_RequestID;
		_Stream << m_ContentLength;
		_Stream << m_PaddingLength;
		_Stream << m_Reserved;
	}

	template <typename tf_CStream>
	void CBeginRequestBody::f_Consume(tf_CStream &_Stream)
	{
		uint16 Role;
		_Stream >> Role;
		m_Role = (ERequestRole)Role;
		uint8 Flags;
		_Stream >> Flags;
		m_Flags = (ERequestBodyFlag)Flags;
		_Stream.f_ConsumeBytes(m_Reserved, sizeof(m_Reserved));
	}

	template <typename tf_CStream>
	void CBeginRequestBody::f_Feed(tf_CStream &_Stream) const
	{
		uint16 Role = m_Role;
		_Stream << Role;
		uint8 Flags = m_Flags;
		_Stream << Flags;
		_Stream.f_FeedBytes(m_Reserved, sizeof(m_Reserved));
	}

	template <typename tf_CStream>
	void CEndRequestBody::f_Consume(tf_CStream &_Stream)
	{
		_Stream >> m_AppStatus;
		uint8 ProtocolStatus;
		_Stream >> ProtocolStatus;
		m_ProtocolStatus = (EEndRequestStatus)ProtocolStatus;
		_Stream.f_ConsumeBytes(m_Reserved, sizeof(m_Reserved));
	}

	template <typename tf_CStream>
	void CEndRequestBody::f_Feed(tf_CStream &_Stream) const
	{
		_Stream << m_AppStatus;
		uint8 ProtocolStatus = m_ProtocolStatus;
		_Stream << ProtocolStatus;
		_Stream.f_FeedBytes(m_Reserved, sizeof(m_Reserved));
	}
}
