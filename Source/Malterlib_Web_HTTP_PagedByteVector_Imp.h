// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NWeb::NHTTP
{
	CPagedByteVector::CPagedByteVector(mint _PageSize)
		: mp_PageSize(_PageSize)
		, mp_nBytes(0)
		, mp_iFirstPageStart(0)
		, mp_iLastPageEnd(0)
	{

	}

	bint CPagedByteVector::f_IsEmpty() const
	{
		if (mp_lPages.f_IsEmpty())
			return true;
		else
			return false;
	}

	mint CPagedByteVector::f_GetLen() const
	{
		return mp_nBytes;
	}

	mint CPagedByteVector::f_GetPageSize() const
	{
		return mp_PageSize;
	}

	mint CPagedByteVector::f_GetFirstPageSpace() const
	{
		if (f_IsEmpty())
			return mp_PageSize;
		else
			return mp_PageSize - mp_iFirstPageStart;
	}

	// tf_FReader is of the format: bint (mint _iStart, uint8 const* _pPtr, mint _nBytes)
	template<typename tf_FReader>
	bint CPagedByteVector::f_Read(mint _iStart, mint _nBytes, tf_FReader &&_fReader) const
	{
		if ((_nBytes + _iStart) > mp_nBytes)
			return false;

		mint iLastPage = mp_lPages.f_GetLen() - 1;

		mint iCurPage = (_iStart + mp_iFirstPageStart) / mp_PageSize;
		mint iCurPagePos = (_iStart + mp_iFirstPageStart) - (iCurPage * mp_PageSize);

		while (_nBytes)
		{
			mint nPageBytes = mp_PageSize - iCurPagePos;
			if (iCurPage == iLastPage)
				nPageBytes -= (mp_PageSize - mp_iLastPageEnd);

			mint nBlockBytes = fg_Min(_nBytes, nPageBytes);

			if (!_fReader(_iStart, (uint8 const *)mp_lPages[iCurPage].f_Get() + iCurPagePos, nBlockBytes))
				break;

			_iStart += nBlockBytes;
			_nBytes -= nBlockBytes;
			++iCurPage;
			iCurPagePos = 0;
		}

		return true;
	}

	// tf_FReader is of the format: bint (mint _iStart, uint8 const* _pPtr, mint _nBytes)
	template<typename tf_FMutate>
	bint CPagedByteVector::f_Mutate(mint _iStart, mint _nBytes, tf_FMutate &&_fMutate) const
	{
		if ((_nBytes + _iStart) > mp_nBytes)
			return false;

		mint iLastPage = mp_lPages.f_GetLen() - 1;

		mint iCurPage = (_iStart + mp_iFirstPageStart) / mp_PageSize;
		mint iCurPagePos = (_iStart + mp_iFirstPageStart) - (iCurPage * mp_PageSize);

		while (_nBytes)
		{
			mint nPageBytes = mp_PageSize - iCurPagePos;
			if (iCurPage == iLastPage)
				nPageBytes -= (mp_PageSize - mp_iLastPageEnd);

			mint nBlockBytes = fg_Min(_nBytes, nPageBytes);

			if (!_fMutate(_iStart, mp_lPages[iCurPage].f_Get() + iCurPagePos, nBlockBytes))
				break;

			_iStart += nBlockBytes;
			_nBytes -= nBlockBytes;
			++iCurPage;
			iCurPagePos = 0;
		}

		return true;
	}

	// tf_FReader is of the format: bint (mint _iStart, uint8 const* _pPtr, mint _nBytes)
	template <typename tf_FReader>
	bint CPagedByteVector::f_ReadFront(mint _nBytes, tf_FReader &&_fReader) const
	{
		return f_Read(0, _nBytes, fg_Forward<tf_FReader>(_fReader));
	}

	// tf_FReader is of the format: bint (mint _iStart, uint8 const* _pPtr, mint _nBytes)
	template <typename tf_FReader>
	bint CPagedByteVector::f_ReadFront(tf_FReader &&_fReader) const
	{
		return f_Read(0, mp_nBytes, fg_Forward<tf_FReader>(_fReader));
	}

	// tf_FReader is of the format: bint (mint _iStart, uint8 const* _pPtr, mint _nBytes)
	template <typename tf_FReader>
	bint CPagedByteVector::f_ReadBack(mint _nBytes, tf_FReader &&_fReader) const
	{
		return f_Read(mp_nBytes - _nBytes, _nBytes, fg_Forward<tf_FReader>(_fReader));
	}

	// tf_FReader is of the format: void (mint _iStart, uint8 const* _pPtr, mint _nBytes, mint _nTotalBytes)
	template <typename tf_FReader>
	bint CPagedByteVector::f_ReadFrontUntil(uint8 const* _pMatch, mint _nMatchBytes, mint& _oPos, tf_FReader &&_fReader) const
	{
		mint iEnd;

		if (f_FindFront(_pMatch, _nMatchBytes, iEnd))
		{
			f_ReadFront(	iEnd
						, 	[&](mint _iStart, uint8 const* _pPtr, mint _nBytes) -> bint
							{
								return fg_Forward<tf_FReader>(_fReader)(_iStart, _pPtr, _nBytes, iEnd);
							}
						);
			_oPos = iEnd;
			return true;
		}
		else
		{
			return false;
		}
	}

	void CPagedByteVector::fp_GetPageInfo(mint _iPage, mint& _oPageSize, mint& _oPageStart)
	{
		_oPageSize = mp_PageSize;
		_oPageStart = 0;

		if (_iPage == 0)
		{
			_oPageSize -= mp_iFirstPageStart;
			_oPageStart = mp_iFirstPageStart;
		}

		if (_iPage == mp_lPages.f_GetLen())
		{
			_oPageSize -= (mp_PageSize - mp_iLastPageEnd);
		}
	}


	///
	/// Stream
	///	======

	template <typename t_CInherit>
	void TCBinaryStreamPagedByteVector<t_CInherit>::fp_SetPositionInternal(NStream::CFilePos _Pos)
	{
		if ((_Pos < 0) || fg_SafeLargerThan(_Pos, TCLimitsInt<mint>::mc_Max))
			DMibError("Memory stream positions are limited to 0 -> TCLimitsInt<mint>::mc_Max");

		m_Position = _Pos;
	}
	template <typename t_CInherit>
	TCBinaryStreamPagedByteVector<t_CInherit>::TCBinaryStreamPagedByteVector(NHTTP::CPagedByteVector &_Buffer)
		: m_Buffer(_Buffer)
	{
		m_Position = 0;
		m_Length = _Buffer.f_GetLen();
	}

	template <typename t_CInherit>
	TCBinaryStreamPagedByteVector<t_CInherit>::~TCBinaryStreamPagedByteVector()
	{
	}

	template <typename t_CInherit>
	void TCBinaryStreamPagedByteVector<t_CInherit>::f_ResetStream()
	{
		m_Position = 0;
		m_Length = 0;
	}

	template <typename t_CInherit>
	void TCBinaryStreamPagedByteVector<t_CInherit>::f_Clear()
	{
		m_Position = 0;
		m_Length = 0;
	}

	template <typename t_CInherit>
	void TCBinaryStreamPagedByteVector<t_CInherit>::f_FeedBytes(const void *_pMem, mint _nBytes)
	{
		m_Buffer.f_InsertBack((uint8 const *)_pMem, _nBytes);
		m_Position += _nBytes;
		if (m_Position > m_Length)
			m_Length = m_Position;

	}

	template <typename t_CInherit>
	void TCBinaryStreamPagedByteVector<t_CInherit>::f_ConsumeBytes(void *_pMem, mint _nBytes)
	{
		DMibPDebugBreak; // Not supported
	}

	template <typename t_CInherit>
	bint TCBinaryStreamPagedByteVector<t_CInherit>::f_IsValid() const
	{
		return true;
	}

	template <typename t_CInherit>
	bint TCBinaryStreamPagedByteVector<t_CInherit>::f_IsAtEndOfStream() const
	{
		DMibPDebugBreak; // Not supported
		return m_Position == m_Length;
	}

	template <typename t_CInherit>
	NStream::CFilePos TCBinaryStreamPagedByteVector<t_CInherit>::f_GetPosition() const
	{
		return m_Position;
	}

	template <typename t_CInherit>
	void TCBinaryStreamPagedByteVector<t_CInherit>::f_SetPosition(NStream::CFilePos _Pos)
	{
		fp_SetPositionInternal(_Pos);
	}

	template <typename t_CInherit>
	void TCBinaryStreamPagedByteVector<t_CInherit>::f_SetPositionFromEnd(NStream::CFilePos _Pos)
	{
		fp_SetPositionInternal(m_Length + _Pos);
	}

	template <typename t_CInherit>
	void TCBinaryStreamPagedByteVector<t_CInherit>::f_AddPosition(NStream::CFilePos _Pos)
	{
		fp_SetPositionInternal(m_Position + _Pos);
	}

	template <typename t_CInherit>
	bint TCBinaryStreamPagedByteVector<t_CInherit>::f_IsValidReadPosition(NStream::CFilePos _Pos) const
	{
		return _Pos >= 0 && _Pos < NStream::CFilePos(m_Length);
	}

	template <typename t_CInherit>
	void TCBinaryStreamPagedByteVector<t_CInherit>::f_Flush(bint _bLocalCacheOnly)
	{
	}

	template <typename t_CInherit>
	NStream::CFilePos TCBinaryStreamPagedByteVector<t_CInherit>::f_GetLength() const
	{
		return m_Length;
	}

	template <typename t_CInherit>
	mint TCBinaryStreamPagedByteVector<t_CInherit>::f_ContainerLengthLimit() const
	{
		return NStream::fg_CapLengthLimit(f_GetLength() - f_GetPosition());
	}

	template <typename t_CInherit>
	void TCBinaryStreamPagedByteVector<t_CInherit>::f_SetLength(NStream::CFilePos _Length)
	{
		DMibPDebugBreak; // Not supported
	}
}
