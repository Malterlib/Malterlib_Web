// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once
#include <Mib/Core/Core>

namespace NMib::NWeb::NHTTP
{
	class CPagedByteVector
	{
	private:

		struct CPageSizeScope
		{
			CPageSizeScope(mint _PageSize);
			~CPageSizeScope();

			DMibThreadLocalScopeDebugMember;
		};

		struct CPageAllocator : public NMemory::CAllocator_Heap
		{
			enum
			{
				mc_bIsDefault = false
			};
			only_parameters_aliased static void f_Free(void *_pBlock, mint _Size);
		};

		mint mp_PageSize;
		mint mp_nBytes;
		NContainer::TCVector<NStorage::TCUniquePointer<uint8, CPageAllocator>> mp_lPages;

		mint mp_iFirstPageStart;
		mint mp_iLastPageEnd;


		uint8 *fp_InsertPage(bint _bFront);
		void fp_InsertWhenEmpty(uint8 const* _pPtr, mint _nBytes);
		inline void fp_GetPageInfo(mint _iPage, mint& _oPageSize, mint& _oPageStart);

	public:

		inline CPagedByteVector(mint _PageSize = 1024);
		~CPagedByteVector();

		CPagedByteVector(CPagedByteVector &&) = delete;
		CPagedByteVector(CPagedByteVector const &) = delete;
		CPagedByteVector &operator = (CPagedByteVector &&) = delete;
		CPagedByteVector &operator = (CPagedByteVector const &) = delete;

		inline bint f_IsEmpty() const;
		inline mint f_GetLen() const;

		inline mint f_GetPageSize() const;

		inline mint f_GetFirstPageSpace() const;

		void f_InsertFront(uint8 const* _pPtr, mint _nBytes);
		void f_InsertBack(uint8 const* _pPtr, mint _nBytes);

		void f_RemoveFront(mint _nBytes);
		void f_RemoveBack(mint _nBytes);

		// If _pMatch is found at the front it is removed and this returns true.
		// else this returns false and the data is untouched.
		bint f_ExpectAndRemoveFront(uint8 const* _pMatch, mint _nMatchBytes);

		// tf_FReader is of the format: void (mint _iStart, uint8 const* _pPtr, mint _nBytes)
		template <typename tf_FReader>
		inline bint f_Read(mint _iStart, mint _nBytes, tf_FReader &&_fReader) const;

		// tf_FReader is of the format: void (mint _iStart, uint8 * _pPtr, mint _nBytes)
		template <typename tf_FReader>
		inline bint f_Mutate(mint _iStart, mint _nBytes, tf_FReader &&_fReader) const;

		// tf_FReader is of the format: void (mint _iStart, uint8 const* _pPtr, mint _nBytes)
		template <typename tf_FReader>
		inline bint f_ReadFront(mint _nBytes, tf_FReader &&_fReader) const;

		// tf_FReader is of the format: void (mint _iStart, uint8 const* _pPtr, mint _nBytes, mint _nTotalBytes)
		template <typename tf_FReader>
		inline bint f_ReadFront(tf_FReader &&_fReader) const;

		// tf_FReader is of the format: void (mint _iStart, uint8 const* _pPtr, mint _nBytes)
		template <typename tf_FReader>
		inline bint f_ReadBack(mint _nBytes, tf_FReader &&_fReader) const;

		// tf_FReader is of the format: void (mint _iStart, uint8 const* _pPtr, mint _nBytes, mint _nTotalBytes)
		template <typename tf_FReader>
		inline bint f_ReadFrontUntil(uint8 const* _pMatch, mint _nMatchBytes, mint& _oPos, tf_FReader &&_fReader) const;

		bint f_FindFront(uint8 const* _pMatch, mint _nMatchBytes, mint& _oPos) const;

	};

	template <typename t_CInherit = NStream::CBinaryStreamLittleEndian>
	class TCBinaryStreamPagedByteVector : public t_CInherit
	{
	private:
		TCBinaryStreamPagedByteVector(TCBinaryStreamPagedByteVector const &) = delete;
		TCBinaryStreamPagedByteVector &operator = (TCBinaryStreamPagedByteVector const &) = delete;

	protected:
		mint m_Position;
		mint m_Length;
		NHTTP::CPagedByteVector &m_Buffer;

		void fp_SetPositionInternal(NStream::CFilePos _Pos);
		DMibStreamImplementProtected(TCBinaryStreamPagedByteVector);
	public:
		DMibStreamImplementOperators(TCBinaryStreamPagedByteVector);


		TCBinaryStreamPagedByteVector(NHTTP::CPagedByteVector &_Buffer);
		~TCBinaryStreamPagedByteVector();
		void f_ResetStream();
		void f_Clear();
		void f_FeedBytes(const void *_pMem, mint _nBytes);
		void f_ConsumeBytes(void *_pMem, mint _nBytes);
		bint f_IsValid() const;
		bint f_IsAtEndOfStream() const;
		NStream::CFilePos f_GetPosition() const;
		void f_SetPosition(NStream::CFilePos _Pos);
		void f_SetPositionFromEnd(NStream::CFilePos _Pos);
		void f_AddPosition(NStream::CFilePos _Pos);
		bint f_IsValidReadPosition(NStream::CFilePos _Pos) const;
		void f_Flush(bint _bLocalCacheOnly);
		NStream::CFilePos f_GetLength() const;
		mint f_ContainerLengthLimit() const;
		void f_SetLength(NStream::CFilePos _Length);

	};
}

#include "Malterlib_Web_HTTP_PagedByteVector_Imp.h"
