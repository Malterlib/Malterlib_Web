// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <Mib/Core/Core>
#include "Malterlib_Web_HTTP_HTTP.h"
#include "Malterlib_Web_HTTP_Response.h"

namespace NMib::NWeb::NHTTP
{
	struct CRequest;

	struct ICRequestHandler
	{
		virtual ~ICRequestHandler();

		// Must be able to tell if a request can be handled from just the fields, no content.
		virtual EStatus f_CanHandle(CRequest const &_Req);

		// NOTE: The request must be handled on a separate thread from the one this is called on.
		virtual void f_Handle(NStorage::TCUniquePointer<CRequest> _pReq, CResponseHeader _Response);
	};

	struct CStaticRequestHandler : public ICRequestHandler
	{
		CStaticRequestHandler();
		~CStaticRequestHandler();

		void f_ServeLocation(CStr const &_LocalRoot, NContainer::TCVector<CStr> const &_Prefix);
		void f_ServeLocation(CStr const &_LocalRoot, CStr const &_EncodedPrefix); // Percent encoded prefix

		// From ICRequestHandler

		bool f_CanHandle(CRequest const &_Req) override;

		void f_Handle(NStorage::TCUniquePointer<CRequest> _pReq, CResponseHeader _Response) override;

	private:
		struct CDetails;
		NStorage::TCUniquePointer<CDetails> mp_pDetails;

		struct CLocation
		{
			NContainer::TCVector<CStr> m_Prefix;
			CStr m_LocalRoot;
		};

		NContainer::TCVector<CLocations> mp_ServedLocations;
	};
}

#include "Malterlib_Web_HTTP_RequestHandler.hpp"
