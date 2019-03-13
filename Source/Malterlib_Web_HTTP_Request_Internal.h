// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once
#include "Malterlib_Web_HTTP_Request.h"

namespace NMib::NWeb::NHTTP
{
	class CRequest::CDetails
	{
		enum EParseState
		{
				EParseState_Header
			,	EParseState_Content
			,	EParseState_Chunked_Plain
			,	EParseState_Content_Chunked
			,	EParseState_Content_Chunked_Trailers
			,	EParseState_DecodeContent

			, 	EParseState_Complete
			, 	EParseState_Invalid
		};

	private:
		ERequestStatus mp_Status;
		NStr::CStr mp_Errors;

		EParseState mp_ParseState;

		CRequestLine mp_RequestLine;
		CGeneralFields mp_GeneralFields;
		CRequestFields mp_RequestFields;
		CEntityFields mp_EntityFields;

		NContainer::CByteVector mp_Content;

		EParse fp_ParseHeader(NContainer::CPagedByteVector &_Data);
		EParse fp_ParseHeaderText(NStr::CStr _Text);

		// If content can be parsed returns true and sets _oNextParseState to the parse state to use
		// to parse it.
		bool fp_ParseContent(NContainer::CPagedByteVector const &_Data, EParseState &_oNextParseState);

		EParse fp_ParseChunkedData(NContainer::CPagedByteVector &_Data);

		EParse fp_ParseContent_Plain(NContainer::CPagedByteVector &_Data);
		EParse fp_ParseContent_Chunked(NContainer::CPagedByteVector &_Data);
		EParse fp_ParseContent_Chunked_Trailers(NContainer::CPagedByteVector &_Data);
		EParse fp_ParseField(NStr::CStr const &_Name, NStr::CStr const &_Value);

	public:
		CDetails();
		~CDetails();

		void f_Clear();

		ERequestStatus f_GetStatus() const;

		NStr::CStr f_GetErrors() const;

		// See the comment for f_Parse in CRequest in HTTP_Request.h
		ERequestStatus f_Parse(NContainer::CPagedByteVector &_Data);
		void f_WriteHeaders(COutputMethod const &_fOutput);

		CRequestLine const &f_GetRequestLine() const;
		CGeneralFields const &f_GetGeneralFields() const;
		CRequestFields const &f_GetRequestFields() const;
		CEntityFields const &f_GetEntityFields() const;

		CRequestLine &f_GetRequestLine();
		CGeneralFields &f_GetGeneralFields();
		CRequestFields &f_GetRequestFields();
		CEntityFields &f_GetEntityFields();
	};
}
