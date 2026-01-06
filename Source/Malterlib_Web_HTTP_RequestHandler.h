// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once
#include <Mib/Core/Core>
#include "Malterlib_Web_HTTP_HTTP.h"
#include "Malterlib_Web_HTTP_Response.h"

namespace NMib::NWeb::NHTTP
{
	class CRequest;

	class IRequestHandler
	{
	public:
		virtual ~IRequestHandler() {}

		// Must be able to tell if a request can be handled from just the fields, no content.
		virtual EStatus f_CanHandle(CRequest const& _Req);

		// NOTE: The request must be handled on a separate thread from the one this is called on.
		virtual void f_Handle(NStorage::TCUniquePointer<CRequest> _pReq, CResponseHeader _Response);
	};

	class CStaticRequestHandler : public IRequestHandler
	{
	private:

		class CDetails;
		NStorage::TCUniquePointer<CDetails> mp_pD;

		struct CLocation
		{
			NContainer::TCVector<CStr> mp_Prefix;
			CStr mp_LocalRoot;
		};

		NContainer::TCVector< CLocations > mp_lServedLocations;

	public:
		CStaticRequestHandler();
		~CStaticRequestHandler();

		void f_ServeLocation(CStr const& _LocalRoot, NContainer::TCVector<CStr> const& _lPrefix);
		void f_ServeLocation(CStr const& _LocalRoot, CStr const& _EncodedPrefix); // Percent encoded prefix

		// From IRequestHandler

		bool f_CanHandle(CRequest const& _Req) override;

		void f_Handle(NStorage::TCUniquePointer<CRequest> _pReq, CResponseHeader _Response) override;

	};
}
