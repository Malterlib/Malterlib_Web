// Copyright © 2017 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Encoding/EJSON>

namespace NMib::NWeb
{
	struct CCurlActor : public NConcurrency::CActor
	{
		using CActorHolder = NConcurrency::CSeparateThreadActorHolder;

		enum EMethod
		{
			EMethod_GET
			, EMethod_POST
			, EMethod_PUT
			, EMethod_DELETE
		};
		
		struct CState;
		
		struct CResult
		{
			CResult(CState const &_State);
			
			uint32 m_StatusCode;
			NStr::CStr m_StatusMessage;
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_Headers;
			NStr::CStr m_Body;
			
			NEncoding::CEJSON f_ToJSON() const;
		};
		
		NConcurrency::TCContinuation<CResult> f_Request
			(
				EMethod _Method
				, NStr::CStr const &_URL
				, NContainer::TCMap<NStr::CStr, NStr::CStr> const &_Headers
				, NStr::CStr const &_Data
			)
		;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NWeb;
#endif
