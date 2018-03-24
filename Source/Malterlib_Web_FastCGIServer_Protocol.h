// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib
{
	namespace NWeb
	{
		namespace NFastCGI
		{
			//
			// Values for type component of FCGI_Header
			//
			
			enum ERequestType : uint8
			{
				ERequestType_Invalid = 0
				, ERequestType_BeginRequest = 1
				, ERequestType_AbortRequest = 2
				, ERequestType_EndRequest = 3
				, ERequestType_Params = 4
				, ERequestType_StdIn = 5
				, ERequestType_StdOut = 6
				, ERequestType_StdErr = 7
				, ERequestType_Data = 8
				, ERequestType_GetValues = 9
				, ERequestType_GetValuesResult = 10
				, ERequestType_UnknownType = 11
				, ERequestType_Max = ERequestType_UnknownType
			};

			enum ERequestVersion : uint8
			{
				ERequestVersion_1 = 1
			};
			
			struct CHeader
			{
				ERequestVersion m_Version = ERequestVersion_1;
				ERequestType m_Type = ERequestType_Invalid;
				uint16 m_RequestID = 0;
				uint16 m_ContentLength = 0;
				uint8 m_PaddingLength = 0;
				uint8 m_Reserved = 0;
				
				template <typename tf_CStream>
				void f_Consume(tf_CStream& _Stream)
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
				void f_Feed(tf_CStream& _Stream) const
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
			};

			enum
			{
				//
				// Number of bytes in a FCGI_Header.  Future versions of the protocol
				// will not reduce this number.
				// 
				EHeaderLen = 8
				
				//
				// Value for requestId component of FCGI_Header
				//
				, ENullRequestID = 0
			};


			enum ERequestBodyFlag : uint8
			{
				ERequestBodyFlag_KeepConnection = DMibBit(0)
			};

			enum ERequestRole : uint16
			{
				ERequestRole_Invalid = 0
				, ERequestRole_Responder = 1
				, ERequestRole_Authorizer = 2
				, ERequestRole_Filter = 3
			};
			

			struct CBeginRequestBody
			{
				ERequestRole m_Role;
				ERequestBodyFlag m_Flags;
				uint8 m_Reserved[5];

				template <typename tf_CStream>
				void f_Consume(tf_CStream& _Stream)
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
				void f_Feed(tf_CStream& _Stream) const
				{
					uint16 Role = m_Role;
					_Stream << Role;
					uint8 Flags = m_Flags;
					_Stream << Flags;
					_Stream.f_FeedBytes(m_Reserved, sizeof(m_Reserved));
				}
			};

			enum EEndRequestStatus : uint8
			{
				EEndRequestStatus_RequestComplete = 0
				, EEndRequestStatus_CantMultiplexConnection = 1
				, EEndRequestStatus_Overloaded = 2
				, EEndRequestStatus_UnknownRole = 3
			};

			
			struct CEndRequestBody
			{
				CEndRequestBody()
					: m_AppStatus(0)
					, m_ProtocolStatus(EEndRequestStatus_RequestComplete)
				{
					NMem::fg_MemClear(m_Reserved);
				}
				
				uint32 m_AppStatus;
				EEndRequestStatus m_ProtocolStatus;
				uint8 m_Reserved[3];
				
				template <typename tf_CStream>
				void f_Consume(tf_CStream& _Stream)
				{
					_Stream >> m_AppStatus;
					uint8 ProtocolStatus;
					_Stream >> ProtocolStatus;
					m_ProtocolStatus = (EEndRequestStatus)ProtocolStatus;
					_Stream.f_ConsumeBytes(m_Reserved, sizeof(m_Reserved));
				}

				template <typename tf_CStream>
				void f_Feed(tf_CStream& _Stream) const
				{
					_Stream << m_AppStatus;
					uint8 ProtocolStatus = m_ProtocolStatus;
					_Stream << ProtocolStatus;
					_Stream.f_FeedBytes(m_Reserved, sizeof(m_Reserved));
				}
				
			};


			/*
			 * Variable names for FCGI_GET_VALUES / FCGI_GET_VALUES_RESULT records
			 */
			#define FCGI_MAX_CONNS  "FCGI_MAX_CONNS"
			#define FCGI_MAX_REQS   "FCGI_MAX_REQS"
			#define FCGI_MPXS_CONNS "FCGI_MPXS_CONNS"

			struct CUnknownTypeBody
			{
				uint8 m_Type;
				uint8 m_Reserved[7];
			} ;
		}	
	}
}

