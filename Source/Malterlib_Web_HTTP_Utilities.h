// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once
#include <Mib/Core/Core>
#include <Mib/Container/PagedByteVector>

#include "Malterlib_Web_HTTP_HTTP.h"

namespace NMib::NWeb::NHTTP
{
	//
	// Utility Methods
	//

	NContainer::TCVector<NStr::CStr> fg_SplitStringOn(NStr::CStr const& _Source, NStr::CStr const& _Sep);

	bool fg_PeekLine(NContainer::CPagedByteVector const& _Data, umint& _iPos, NStr::CStr& _oLine);
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb::NHTTP;
#endif
