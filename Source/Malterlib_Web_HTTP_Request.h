// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once
#include <Mib/Core/Core>
#include <Mib/Container/PagedByteVector>
#include "Malterlib_Web_HTTP_Fields.h"

namespace NMib::NWeb::NHTTP
{
	enum ERequestStatus
	{
		ERequestStatus_Empty			// The request has been created but not parsed.
		, ERequestStatus_InProgress 	// The request has parsing in progress,
		, ERequestStatus_Invalid
		, ERequestStatus_Complete
	};

	class CConnection;

	// Parses HTTP request messages.
	class CRequest
	{
	private:
		class CDetails;
		NStorage::TCUniquePointer<CDetails> mp_pD;

		friend class CConnection;

	public:
		CRequest();
		CRequest(CRequest &&_ToMove);
		~CRequest();

		CRequest &operator=(CRequest &&_ToMove);

		void f_Clear();
		ERequestStatus f_GetStatus() const;
		NStr::CStr f_GetErrors() const;

		/*
			f_Parse parses data as it comes from the network.
			As soon as request data is received this is called with that
			data.

			f_Parse returns:
				ERequestStatus_Complete
					- The request is complete, all relevant data has been extracted from _Data

				ERequestStatus_Invalid
					- The request was invalid, use f_GetErrors() for more info. The connection should be dropped.

				ERequestStatus_InProgress
					- The request is not complete yet and f_Parse should be called again as new data arrives.
						_Data may or may not have been altered in this case.
		*/
		ERequestStatus f_Parse(NContainer::CPagedByteVector& _Data);
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

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb::NHTTP;
#endif
