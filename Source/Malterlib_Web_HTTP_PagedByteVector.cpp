// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTP_PagedByteVector.h"

namespace NMib
{

	namespace NHTTP
	{

		void CPagedByteVector::fp_InsertWhenEmpty(uint8 const* _pPtr, mint _nBytes)
		{
			mint nBytesLeft = _nBytes;
			mint nBlockBytes = 0;

			while (nBytesLeft)
			{
				uint8 *pNewPage = fp_InsertPage(false);

				nBlockBytes = fg_Min(nBytesLeft, mp_PageSize);

				NMem::fg_MemCopy(pNewPage, _pPtr, nBlockBytes);

				nBytesLeft -= nBlockBytes;
				_pPtr += nBlockBytes;
			}

			mp_iFirstPageStart = 0;
			mp_iLastPageEnd = nBlockBytes;
			mp_nBytes = _nBytes;
		}

		void CPagedByteVector::f_InsertFront(uint8 const* _pPtr, mint _nBytes)
		{
			if (f_IsEmpty())
			{
				fp_InsertWhenEmpty(_pPtr, _nBytes);
				return;
			}

			mint nBytesLeft = _nBytes;

			uint8 const* pEndPtr = _pPtr + _nBytes;

			if (mp_iFirstPageStart > 0)
			{
				// There is space in the first page.
				mint nBytesFreeInFirstPage = mp_iFirstPageStart;

				mint nBlockBytes = fg_Min(nBytesLeft, nBytesFreeInFirstPage);

				mp_iFirstPageStart -= nBlockBytes;
				pEndPtr -= nBlockBytes;

				NMem::fg_MemCopy(mp_lPages[0].f_Get() + mp_iFirstPageStart, pEndPtr, nBlockBytes);

				nBytesLeft -= nBlockBytes;
			}

			while(nBytesLeft)
			{
				mint nBlockBytes = fg_Min(nBytesLeft, mp_PageSize);

				uint8 *pNewPage = fp_InsertPage(true);

				mp_iFirstPageStart = mp_PageSize - nBytesLeft;
				pEndPtr -= nBlockBytes;

				NMem::fg_MemCopy(pNewPage + mp_iFirstPageStart, pEndPtr, nBlockBytes);

				nBytesLeft -= nBlockBytes;
			}

			mp_nBytes += _nBytes;

		}

		void CPagedByteVector::f_InsertBack(uint8 const* _pPtr, mint _nBytes)
		{
			if (f_IsEmpty())
			{
				fp_InsertWhenEmpty(_pPtr, _nBytes);
				return;
			}

			mint nBytesLeft = _nBytes;

			if (mp_iLastPageEnd < mp_PageSize)
			{ // There is space in the last block
				mint nBlockBytes = fg_Min(nBytesLeft, mp_PageSize - mp_iLastPageEnd);

				NMem::fg_MemCopy(
							mp_lPages.f_GetLast().f_Get() + mp_iLastPageEnd
						,	_pPtr
						,	nBlockBytes);

				_pPtr += nBlockBytes;
				nBytesLeft -= nBlockBytes;
				mp_iLastPageEnd += nBlockBytes;
			}

			while(nBytesLeft)
			{
				mint nBlockBytes = fg_Min(nBytesLeft, mp_PageSize);

				uint8 *pNewPage = fp_InsertPage(false);

				NMem::fg_MemCopy(pNewPage, _pPtr, nBlockBytes);

				_pPtr += nBlockBytes;
				nBytesLeft -= nBlockBytes;
				
				mp_iLastPageEnd = nBlockBytes;
			}

			mp_nBytes += _nBytes;
		}

		void CPagedByteVector::f_RemoveFront(mint _nBytes)
		{
			mint iCurPage = 0;

			mint iLastPage = mp_lPages.f_GetLen() - 1;
			mint nBytes = _nBytes;

			while (nBytes)
			{
				mint nPageBytes = mp_PageSize;
				if (iCurPage == 0)
					nPageBytes -= mp_iFirstPageStart;
				if (iCurPage == iLastPage)
					nPageBytes -= (mp_PageSize - mp_iLastPageEnd);

				mint nBlockBytes = fg_Min(nPageBytes, nBytes);

				if (nBlockBytes == nPageBytes)
				{
					++iCurPage;
					mp_iFirstPageStart = 0;
				}
				else
				{
					mp_iFirstPageStart += nBlockBytes;
				}

				nBytes -= nBlockBytes;
			}
			
			mp_nBytes -= _nBytes;
			mp_lPages.f_Remove(0, iCurPage);
		}

		void CPagedByteVector::f_RemoveBack(mint _nBytes)
		{
			mint iLastPage = mp_lPages.f_GetLen() - 1;
			mint iCurPage = iLastPage;
			
			mint nBytes = _nBytes;

			while (nBytes)
			{
				mint nPageBytes = mp_PageSize;
				if (iCurPage == 0)
					nPageBytes -= mp_iFirstPageStart;
				if (iCurPage == iLastPage)
					nPageBytes -= (mp_PageSize - mp_iLastPageEnd);

				mint nBlockBytes = fg_Min(nPageBytes, nBytes);

				if (nBlockBytes == nPageBytes)
				{
					--iCurPage;
					mp_iLastPageEnd = mp_PageSize;
				}
				else
				{
					mp_iLastPageEnd -= nBlockBytes;
				}

				nBytes -= nBlockBytes;
			}

			mp_nBytes -= _nBytes;
			mp_lPages.f_Remove(iCurPage + 1, iLastPage - iCurPage);
		}

		// If _pMatch is found at the front it is removed and this returns true.
		// else this returns false and the data is untouched.
		bint CPagedByteVector::f_ExpectAndRemoveFront(uint8 const* _pMatch, mint _nMatchBytes)
		{
			bint bFound = false;
			mint iMatchPos = 0;
			f_ReadFront
				(
					_nMatchBytes
					, [&](mint _iStart, uint8 const* _pPtr, mint _nBytes) -> bint
					{
						if (NMem::fg_MemCmp(_pMatch + iMatchPos, _pPtr, _nBytes) == 0)
						{
							iMatchPos += _nBytes;
							if (iMatchPos == _nMatchBytes)
							{
								bFound = true;
								return false;
							}
							else
							{
								return true;
							}
						}
						else
						{
							bFound = false;
							return false;
						}
					}
				)
			;
			if (bFound)
				f_RemoveFront(_nMatchBytes);

			return bFound;
		}

		uint8 *CPagedByteVector::fp_InsertPage(bint _bFront)
		{
			mint PageSize = mp_PageSize;
			NPtr::TCUniquePointer<uint8> pNewPage = fg_Explicit((uint8 *)NMem::fg_Alloc(PageSize));

			if (_bFront)
				return mp_lPages.f_InsertFirst(fg_Move(pNewPage)).f_Get();
			else
				return mp_lPages.f_Insert(fg_Move(pNewPage)).f_Get();
		}

		bint CPagedByteVector::f_FindFront(uint8 const* _pMatch, mint _nMatchBytes, mint& _oPos) const
		{
			bint bFound = false;
			mint iFoundPos = 0;

			f_ReadFront
				(
					mp_nBytes
					, [&](mint _iStart, uint8 const* _pPtr, mint _nBytes) -> bint
					{
						uint8 const* pEnd = _pPtr + _nBytes;
						for (uint8 const* pCurPos = _pPtr; pCurPos != pEnd; ++pCurPos)
						{
							if (*pCurPos == *_pMatch)
							{
								mint iMatchPos = 0;
								mint iCurPos = _iStart + (pCurPos - _pPtr);
								f_Read
									(
										iCurPos
										, _nBytes - iCurPos
										, [&](mint _iStart, uint8 const* _pPtr, mint _nBytes) -> bint
										{
											mint CmpLen = fg_Min(_nBytes, _nMatchBytes - iMatchPos);
											if (NMem::fg_MemCmp(_pPtr, _pMatch + iMatchPos, CmpLen) == 0)
											{
												iMatchPos += CmpLen;

												if (iMatchPos == _nMatchBytes)
												{
													bFound = true;
													return false;
												}
												else
												{
													return true;
												}
											}
											else
											{
												bFound = false;
												return false;
											}
										}
									)
								;
								if (bFound)
								{
									iFoundPos = iCurPos;
									break;
								}
							}
						}
						
						return !bFound;
					}
				)
			;

			if (bFound)
				_oPos = iFoundPos;

			return bFound;
		}

	} // Namespace NHTTP

} // Namespace NMib
