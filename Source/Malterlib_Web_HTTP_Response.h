// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <Mib/Core/Core>
#include <utility>
#include "Malterlib_Web_HTTP_HTTP.h"
#include "Malterlib_Web_HTTP_Fields.h"

namespace NMib::NWeb::NHTTP
{
	enum EResponseStatus
	{
		EResponseStatus_Empty        // The request has been created but not parsed.
		, EResponseStatus_InProgress // The request has parsing in progress,
		, EResponseStatus_Invalid
		, EResponseStatus_Complete
	};

	struct CResponseDetails;

	// Utility class common to all response stages.

	struct CResponseStage
	{
		CResponseStage(NStorage::TCUniquePointer<CResponseDetails> &&_pDetails);
		CResponseStage(CResponseStage &&_ToMove);
		CResponseStage();
		~CResponseStage();

		bool f_IsValid() const;

		void f_Abort();

	protected:
		NStorage::TCUniquePointer<CResponseDetails> mp_pDetails;

	private:
		CResponseStage(CResponseStage const &_Other) = delete;
		CResponseStage &operator = (CResponseStage const &_Other) = delete;
		CResponseStage &operator = (CResponseStage &&_Other) = delete;
	};

	// These are in reverse order due to dependencies.
	// You start with a ResponseHeader then get a ResponseContent then get a ResponseTrailer.

	//
	// CResponseTrailer
	// Represents the optional end of a HTTP response, the trailer fields.
	//

	struct CResponseTrailer : public CResponseStage
	{
		CResponseTrailer(CResponseTrailer &&_ToMove);
		CResponseTrailer(NStorage::TCUniquePointer<CResponseDetails> &&_pDetails);
		~CResponseTrailer();

		void f_AddField(NStr::CStr const &_Name, NStr::CStr const &_Value);

		void f_Complete();
	};

	//
	// CResponseContent
	// Represents the content of a HTTP response.
	//

	struct CResponseContent : public CResponseStage
	{
		CResponseContent(CResponseContent &&_ToMove);
		CResponseContent(NStorage::TCUniquePointer<CResponseDetails> &&_pDetails);
		~CResponseContent();

		void f_SendData(uint8 const *_pData, umint _nBytes);
		void f_SendString(NStr::CStr const &_String);

		CResponseTrailer f_Complete();
	};

	//
	// CResponseHeader
	// Represents the start of a HTTP response, the header block.
	//

	struct CResponseHeader : public CResponseStage
	{
		CResponseHeader(CResponseHeader &&_ToMove);
		CResponseHeader(COutputMethod &&_fOutputMethod);
		CResponseHeader();
		~CResponseHeader();

		void f_SetOutputMethod(COutputMethod &&_fOutputMethod);
		void f_SetStatus(EStatus _Status, NStr::CStr const &_CustomReason = NStr::CStr(), EVersion _Version = EVersion_HTTP_1_1);

		CStatusLine const &f_GetStatusLine() const;

		CGeneralFields &f_GetGeneralFields();
		CResponseFields &f_GetResponseFields();
		CEntityFields &f_GetEntityFields();

		// For fields access used the inherited method from CResponseFields

		CResponseContent f_Complete();

		EResponseStatus f_Parse(NContainer::CPagedByteVector &_Data);

	private:
		CResponseHeader(CResponseHeader const &_Other) = delete;
		CResponseHeader &operator = (CResponseHeader const &_Other) = delete;
		CResponseHeader &operator = (CResponseHeader &&_Other) = delete;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NWeb::NHTTP;
#endif
