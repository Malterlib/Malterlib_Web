// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTP_Utilities.h"

namespace NMib::NWeb::NHTTP
{
	//
	// Utility Methods
	//

	NContainer::TCVector<NStr::CStr> fg_SplitStringOn(NStr::CStr const& _Source, NStr::CStr const& _Sep)
	{
		NContainer::TCVector<NStr::CStr> lResult;

		aint iCurPos = 0;
		aint iLineEnd = 0;
		aint iEnd = _Source.f_GetLen();
		aint SepLen = _Sep.f_GetLen();

		while (iCurPos < iEnd)
		{
			iLineEnd = _Source.f_Find(iCurPos, _Sep);
			if (iLineEnd == -1)
				iLineEnd = iEnd;

			lResult.f_Insert(_Source.f_Extract(iCurPos, iLineEnd - iCurPos));

			iCurPos = iLineEnd + SepLen;
		}

		return fg_Move(lResult);
	}

	// Peeks an ASCII line terminated by CRLF from _Data, starting at _iPos.
	// On success returns true and sets _iPos to the first byte after the CRLF
	// On failure returns false and does not alter _iPos
	bool fg_PeekLine(NContainer::CPagedByteVector const& _Data, umint& _iPos, NStr::CStr& _oLine)
	{
		NStr::CStr Result;
		static uint8 const CRLFCRLF[] = { '\r', '\n', '\r', '\n'};

		umint Pos;
		NStr::CStr::CChar* pDest = nullptr;
		if (_Data.f_ReadFrontUntil(
						CRLFCRLF
					,	sizeof(CRLFCRLF)
					,	Pos
					,	[&](umint _iStart, uint8 const* _pPtr, umint _nBytes, umint _nTotal)
						{
							if (Result.f_IsEmpty())
								pDest = Result.f_GetStr(_nTotal);

							NMemory::fg_MemCopy(pDest + _iStart, _pPtr, _nBytes);

							return true;
						}
					)
			)
		{
//			*(Result.f_GetStr() + Result.f_GetLen()) = 0;
			_iPos = Result.f_GetLen() + sizeof(CRLFCRLF);
			_oLine = fg_Move(Result);
			return true;
		}
		else
		{
			return false;
		}
	}
}
