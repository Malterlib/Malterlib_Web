// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

/*
This file contains structures for parsing and storing the different kind of field sets that make
up a HTTP message header.

	CGeneralFields	- Can be present in any message
	CRequestFields	- Can be present in any request message (GET, POST, HEAD)
	CEntityFields 	- Can be present in any message containing or describing content
	CResponseFields - Can be present in any response message


*/
#pragma once
#include <Mib/Core/Core>
#include "Malterlib_Web_HTTP_HTTP.h"
#include "Malterlib_Web_HTTP_URL.h"
#include "Malterlib_Web_HTTP_PagedByteVector.h"

namespace NMib
{

	namespace NHTTP
	{

		class CPagedByteVector;
		
		typedef NFunction::TCFunction<void (uint8 const *_pBytes, mint _nBytes)> COutputMethod;

		// Request line fields
		struct CRequestLine
		{
		private:
			EMethod mp_Method;
			CURL mp_URI;
			EVersion mp_Version;

		public:
			CRequestLine();
			~CRequestLine();

			void f_Clear();

			EParse f_Parse(NStr::CStr const &_RequestLine, NStr::CStr &_oErrors);
			void f_Write(COutputMethod const &_fOutput);

			void f_Set(EVersion _Version, EMethod _Method, CURL const &_URI);
			
			EMethod f_GetMethod() const;
			CURL const& f_GetURI() const;
			EVersion f_GetVersion() const;
		};

		// Status line fields
		struct CStatusLine
		{
		private:
			EVersion mp_Version;
			EStatus mp_Status;
			NStr::CStr mp_ReasonPhrase;

		public:
			CStatusLine();
			~CStatusLine();

			void f_Clear();

			EParse f_Parse(NStr::CStr const& _StatusLine, NStr::CStr& _oErrors);

			EVersion f_GetVersion() const;
			EStatus f_GetStatus() const;
			NStr::CStr const& f_GetReasonPhrase() const;

			void f_Set(EVersion _Version, EStatus _Status);

			void f_Write(COutputMethod const &_fOutput);
		};
		//
		// Base information used in all field objects
		//

		enum EFieldType
		{
				EFieldType_String				// CStr
			,	EFieldType_Mint					// mint
			,	EFieldType_TransferEncoding		// ETransferEncoding
			,	EFieldType_ConnectionToken		// EConnectionToken
		};

		typedef NContainer::TCStreamableVariant<EFieldType
									,	NStr::CStr, EFieldType_String
									,	mint, EFieldType_Mint
									,	ETransferEncoding, EFieldType_TransferEncoding
									, 	EConnectionToken, EFieldType_ConnectionToken
								> CFieldValue;

		// This is used to wrap a string value that may be static or dynamic.
		// Static strings are stored as pointers, without allocation.
		struct CStringValue
		{
		private:
			NStr::CStr mp_Storage;
			char const* mp_pStr;

			inline CStringValue(NStr::CStr const& _Str);
			inline CStringValue(char const* _pStr);

		public:

			inline CStringValue(CStringValue&& _ToMove);
			inline CStringValue();
	
			inline void f_Clear();

			inline CStringValue& operator=(CStringValue&& _ToMove);

			inline char const* f_Get() const;

			static inline CStringValue fs_CreateStaticString(char const* _pStr);
			static inline CStringValue fs_CreateDynamicString(char const* _pStr);
			static inline CStringValue fs_CreateDynamicString(NStr::CStr const& _Str);
		};

		CStringValue fg_GetFieldValueAsString(CFieldValue const &_Value);

		template <typename t_CEnum>
		inline NStr::CStr fg_WriteFieldsToString(NContainer::TCMap<t_CEnum, CFieldValue> const& _Fields);

		template<typename t_CSub, typename t_CEnum>
		class TCFieldsBase
		{
		public:
			typedef t_CEnum CEnum;
			
		protected:
			EVersion mp_HTTPVersion;

			NContainer::TCMap<CEnum, CFieldValue> mp_Fields;

			template<EFieldType t_FieldType>
			auto fp_GetField(CEnum _Field) const -> typename CFieldValue::TCTypeFromMember<t_FieldType>::CType;

		public:
			TCFieldsBase();
			TCFieldsBase(TCFieldsBase&& _ToMove);
			~TCFieldsBase();

			inline void f_SetHTTPVersion(EVersion _Version);
			inline EVersion f_GetHTTPVersion() const;

			inline void f_Clear();

			inline bint f_HasField(CEnum _Field) const;

			// Returns:
			//	EParse_OK 			- The field was known and parsed OK
			//	EParse_Invalid 		- The field was known but the data was invalud
			//	EParse_Unknown	 	- The field was not known
			inline EParse f_ParseField(NStr::CStr const& _Name, NStr::CStr const& _Value);

			inline void f_ClearField(CEnum _Field);

			inline NStr::CStr f_WriteToString() const;
			// Writes the header to the end of the paged vector
			inline void f_WriteToData(COutputMethod const &_fOutput) const;
		};

		struct CGeneralFields : public TCFieldsBase<CGeneralFields, EGeneralField>
		{
		public:
			CGeneralFields();
			~CGeneralFields();

			EParse f_ParseKnownField(EGeneralField _Field, NStr::CStr const& _Value);
			EParse f_ParseUnknownField(NStr::CStr const& _Name, NStr::CStr const& _Value);

			NStr::CStr f_GetCacheControl() const;
			EConnectionToken f_GetConnection() const;
			NStr::CStr f_GetDate() const;
			NStr::CStr f_GetPragma() const;
			NStr::CStr f_GetTrailer() const;
			ETransferEncoding f_GetTransferEncoding() const;
			NStr::CStr f_GetUpgrade() const;
			NStr::CStr f_GetVia() const;
			NStr::CStr f_GetWarning() const;

			void f_SetCacheControl(NStr::CStr const& _Value);
			void f_SetConnection(EConnectionToken _Value);
			void f_SetDate(NStr::CStr const& _Value);
			void f_SetPragma(NStr::CStr const& _Value);
			void f_SetTrailer(NStr::CStr const& _Value);
			void f_SetTransferEncoding(ETransferEncoding _Value);
			void f_SetUpgrade(NStr::CStr const& _Value);
			void f_SetVia(NStr::CStr const& _Value);
			void f_SetWarning(NStr::CStr const& _Value);
		};

		struct CRequestFields : public TCFieldsBase<CRequestFields, ERequestField>
		{
		public:
			CRequestFields();
			~CRequestFields();

			EParse f_ParseKnownField(ERequestField _Field, NStr::CStr const& _Value);
			EParse f_ParseUnknownField(NStr::CStr const& _Name, NStr::CStr const& _Value);

			NStr::CStr f_GetAccept() const;
			NStr::CStr f_GetAcceptCharset() const;
			NStr::CStr f_GetAcceptEncoding() const;
			NStr::CStr f_GetAcceptLanguage() const;
			NStr::CStr f_GetAuthorization() const;
			NStr::CStr f_GetExpect() const;
			NStr::CStr f_GetFrom() const;
			NStr::CStr f_GetHost() const;
			NStr::CStr f_GetIfMatch() const;
			NStr::CStr f_GetIfModifiedSince() const;
			NStr::CStr f_GetIfNoneMatch() const;
			NStr::CStr f_GetIfRange() const;
			NStr::CStr f_GetIfUnmodifiedSince() const;
			NStr::CStr f_GetMaxForwards() const;
			NStr::CStr f_GetProxyAuthorization() const;
			NStr::CStr f_GetRange() const;
			NStr::CStr f_GetReferer() const;
			NStr::CStr f_GetTE() const;
			NStr::CStr f_GetUserAgent() const;			

			void f_SetAccept(NStr::CStr const& _Value);
			void f_SetAcceptCharset(NStr::CStr const& _Value);
			void f_SetAcceptEncoding(NStr::CStr const& _Value);
			void f_SetAcceptLanguage(NStr::CStr const& _Value);
			void f_SetAuthorization(NStr::CStr const& _Value);
			void f_SetExpect(NStr::CStr const& _Value);
			void f_SetFrom(NStr::CStr const& _Value);
			void f_SetHost(NStr::CStr const& _Value);
			void f_SetIfMatch(NStr::CStr const& _Value);
			void f_SetIfModifiedSince(NStr::CStr const& _Value);
			void f_SetIfNoneMatch(NStr::CStr const& _Value);
			void f_SetIfRange(NStr::CStr const& _Value);
			void f_SetIfUnmodifiedSince(NStr::CStr const& _Value);
			void f_SetMaxForwards(NStr::CStr const& _Value);
			void f_SetProxyAuthorization(NStr::CStr const& _Value);
			void f_SetRange(NStr::CStr const& _Value);
			void f_SetReferer(NStr::CStr const& _Value);
			void f_SetTE(NStr::CStr const& _Value);
			void f_SetUserAgent(NStr::CStr const& _Value);;			

		};

		// Header fields
		struct CEntityFields : public TCFieldsBase<CEntityFields, EEntityField>
		{
		private:
			NContainer::TCMap<NStr::CStr, NStr::CStr, NStr::CCompareNoCase> mp_UnknownFields;

		public:
			CEntityFields();
			~CEntityFields();

			EParse f_ParseKnownField(EEntityField _Field, NStr::CStr const& _Value);
			EParse f_ParseUnknownField(NStr::CStr const& _Name, NStr::CStr const& _Value);

			NStr::CStr f_GetContentEncoding() const;
			NStr::CStr f_GetContentLanguage() const;
			mint f_GetContentLength() const;
			NStr::CStr f_GetContentLocation() const;
			NStr::CStr f_GetContentMD() const;
			NStr::CStr f_GetContentRange() const;
			NStr::CStr f_GetContentType() const;
			NStr::CStr f_GetExpires() const;
			NStr::CStr f_GetLastModified() const;
			NStr::CStr f_GetExtensionHeader() const;
			
			NStr::CStr const *f_GetUnknownField(NStr::CStr const &_Field) const;

			void f_SetContentEncoding(NStr::CStr const& _Value);
			void f_SetContentLanguage(NStr::CStr const& _Value);
			void f_SetContentLength(mint _Value);
			void f_SetContentLocation(NStr::CStr const& _Value);
			void f_SetContentMD(NStr::CStr const& _Value);
			void f_SetContentRange(NStr::CStr const& _Value);
			void f_SetContentType(NStr::CStr const& _Value);
			void f_SetExpires(NStr::CStr const& _Value);
			void f_SetLastModified(NStr::CStr const& _Value);
			void f_SetExtensionHeader(NStr::CStr const& _Value);
			
			void f_SetUnknownField(NStr::CStr const &_Field, NStr::CStr const &_Value);
			
			NStr::CStr f_WriteToString() const;
			void f_WriteToData(COutputMethod const &_fOutput) const;
		};

		struct CResponseFields : public TCFieldsBase<CResponseFields, EResponseField>
		{
		public:
			CResponseFields();
			~CResponseFields();

			EParse f_ParseKnownField(EResponseField _Field, NStr::CStr const& _Value);
			EParse f_ParseUnknownField(NStr::CStr const& _Name, NStr::CStr const& _Value);

			NStr::CStr f_GetAcceptRanges() const;
			NStr::CStr f_GetAge() const;
			NStr::CStr f_GetETag() const;
			NStr::CStr f_GetLocation() const;
			NStr::CStr f_GetProxyAuthenticate() const;
			NStr::CStr f_GetRetryAfter() const;
			NStr::CStr f_GetServer() const;
			NStr::CStr f_GetVary() const;
			NStr::CStr f_GetWWWAuthenticate() const;

			void f_SetAcceptRanges(NStr::CStr const& _Value);
			void f_SetAge(NStr::CStr const& _Value);
			void f_SetETag(NStr::CStr const& _Value);
			void f_SetLocation(NStr::CStr const& _Value);
			void f_SetProxyAuthenticate(NStr::CStr const& _Value);
			void f_SetRetryAfter(NStr::CStr const& _Value);
			void f_SetServer(NStr::CStr const& _Value);
			void f_SetVary(NStr::CStr const& _Value);
			void f_SetWWWAuthenticate(NStr::CStr const& _Value);
		};
		
	} // Namespace NHTTP

} // Namespace NMib

#include "Malterlib_Web_HTTP_Fields_Imp.h"

#ifndef DMibPNoShortCuts
using namespace NMib::NHTTP;
#endif
