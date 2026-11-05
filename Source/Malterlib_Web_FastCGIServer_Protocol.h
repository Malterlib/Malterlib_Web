// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NWeb::NFastCGI
{
	//
	// Values for type component of FCGI_Header
	//

	enum ERequestType : uint8
	{
		ERequestType_Invalid           = 0
		, ERequestType_BeginRequest    = 1
		, ERequestType_AbortRequest    = 2
		, ERequestType_EndRequest      = 3
		, ERequestType_Params          = 4
		, ERequestType_StdIn           = 5
		, ERequestType_StdOut          = 6
		, ERequestType_StdErr          = 7
		, ERequestType_Data            = 8
		, ERequestType_GetValues       = 9
		, ERequestType_GetValuesResult = 10
		, ERequestType_UnknownType     = 11
		, ERequestType_Max             = ERequestType_UnknownType
	};

	enum ERequestVersion : uint8
	{
		ERequestVersion_1 = 1
	};

	struct CHeader
	{
		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream);

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const;

		ERequestVersion m_Version = ERequestVersion_1;
		ERequestType m_Type = ERequestType_Invalid;
		uint16 m_RequestID = 0;
		uint16 m_ContentLength = 0;
		uint8 m_PaddingLength = 0;
		uint8 m_Reserved = 0;
	};

	constexpr aint gc_HeaderLen = 8;
	constexpr aint gc_NullRequestID = 0;

	enum ERequestBodyFlag : uint8
	{
		ERequestBodyFlag_KeepConnection = DMibBit(0)
	};

	enum ERequestRole : uint16
	{
		ERequestRole_Invalid      = 0
		, ERequestRole_Responder  = 1
		, ERequestRole_Authorizer = 2
		, ERequestRole_Filter     = 3
	};

	struct CBeginRequestBody
	{
		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream);

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const;

		ERequestRole m_Role;
		ERequestBodyFlag m_Flags;
		uint8 m_Reserved[5];
	};

	enum EEndRequestStatus : uint8
	{
		EEndRequestStatus_RequestComplete           = 0
		, EEndRequestStatus_CantMultiplexConnection = 1
		, EEndRequestStatus_Overloaded              = 2
		, EEndRequestStatus_UnknownRole             = 3
	};

	struct CEndRequestBody
	{
		CEndRequestBody();

		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream);

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const;

		uint32 m_AppStatus;
		EEndRequestStatus m_ProtocolStatus;
		uint8 m_Reserved[3];
	};

	/*
	 * Variable names for FCGI_GET_VALUES / FCGI_GET_VALUES_RESULT records
	 */
#define DFcgiMaxConns  "FCGI_MAX_CONNS"
#define DFcgiMaxReqs   "FCGI_MAX_REQS"
#define DFcgiMpxsConns "FCGI_MPXS_CONNS"

	struct CUnknownTypeBody
	{
		uint8 m_Type;
		uint8 m_Reserved[7];
	};
}

#include "Malterlib_Web_FastCGIServer_Protocol.hpp"
