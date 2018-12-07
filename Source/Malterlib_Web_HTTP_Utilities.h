// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once
#include <Mib/Core/Core>
#include "Malterlib_Web_HTTP_HTTP.h"

namespace NMib::NWeb::NHTTP
{
	//
	// Utility Methods
	//

	class CPagedByteVector;

	NContainer::TCVector<NStr::CStr> fg_SplitStringOn(NStr::CStr const& _Source, NStr::CStr const& _Sep);

	bint fg_PeekLine(CPagedByteVector const& _Data, mint& _iPos, NStr::CStr& _oLine);
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb::NHTTP;
#endif
