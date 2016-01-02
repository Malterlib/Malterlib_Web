// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once
#include <Mib/Core/Core>
#include "Malterlib_Web_HTTP_HTTP.h"

namespace NMib
{

	namespace NHTTP
	{

		//
		// Utility Methods
		//

		class CPagedByteVector;

		NContainer::TCVector<NStr::CStr> fg_SplitStringOn(NStr::CStr const& _Source, NStr::CStr const& _Sep);

		bint fg_PeekLine(CPagedByteVector const& _Data, mint& _iPos, NStr::CStr& _oLine);

	} // Namespace NHTTP

} // Namespace NMib

#ifndef DMibPNoShortCuts
using namespace NMib::NHTTP;
#endif
