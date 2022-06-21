// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTP_Response.h"
#include "Malterlib_Web_HTTP_HTTP.h"

namespace NMib::NWeb::NHTTP
{
	//
	// The implementation of a response
	//

	class CResponseDetails
	{
	public:
		CResponseDetails()
			: m_Stage(EStage_Header)
			, mp_Status(EResponseStatus_Empty)
			, mp_ParseState(EParseState_Header)
		{
		}
		CResponseDetails(COutputMethod &&_OutputMethod)
			: m_Stage(EStage_Header)
			, mp_Status(EResponseStatus_Empty)
			, mp_ParseState(EParseState_Header)
			, m_OutputMethod(fg_Move(_OutputMethod))
		{
		}

	private:
		enum EStage
		{
				EStage_Header
			,	EStage_Content
			,	EStage_Trailer
		};

		enum EParseState
		{
				EParseState_Header
			, 	EParseState_Complete
			, 	EParseState_Invalid
		};

		EResponseStatus mp_Status;

		EParseState mp_ParseState;
		NStr::CStr mp_Errors;

		EParse fp_ParseHeader(NContainer::CPagedByteVector &_Data);
		EParse fp_ParseHeaderText(NStr::CStr _Text);
		EParse fp_ParseField(NStr::CStr const& _Name, NStr::CStr const& _Value);

	public:
		EStage m_Stage;
		COutputMethod m_OutputMethod;
		CResponseFields m_ResponseFields;
		CGeneralFields m_GeneralFields;
		CEntityFields m_EntityFields;
		CStatusLine m_Status;

		EResponseStatus f_Parse(NContainer::CPagedByteVector& _Data);
	};

	//
	// CResponseStage Public Methods
	//

	CResponseStage::CResponseStage(NStorage::TCUniquePointer<CResponseDetails> _pD)
		: mp_pD(fg_Move(_pD))
	{
	}

	CResponseStage::CResponseStage(CResponseStage&& _ToMove)
		: mp_pD(fg_Move(_ToMove.mp_pD))
	{
	}

	CResponseStage::CResponseStage()
	{
	}

	CResponseStage::~CResponseStage()
	{
	}

	bool CResponseStage::f_IsValid() const
	{
		return mp_pD ? true : false;
	}

	void CResponseStage::f_Abort()
	{

	}

	//
	// CResponseHeader Public Methods
	//

	CResponseHeader::CResponseHeader(CResponseHeader&& _ToMove)
		: CResponseStage(fg_Move(_ToMove))
	{

	}

	CResponseHeader::CResponseHeader(COutputMethod &&_OutputMethod)
		: CResponseStage(fg_Construct(fg_Move(_OutputMethod)))
	{

	}

	CResponseHeader::CResponseHeader()
		: CResponseStage(fg_Construct())
	{
	}

	CResponseHeader::~CResponseHeader()
	{

	}

	void CResponseHeader::f_SetOutputMethod(COutputMethod &&_OutputMethod)
	{
		mp_pD->m_OutputMethod = fg_Move(_OutputMethod);
	}

	void CResponseHeader::f_SetStatus(EStatus _Status, NStr::CStr const &_CustomReason, EVersion _Version)
	{
		mp_pD->m_Status.f_Set(_Version, _Status, _CustomReason);
		mp_pD->m_ResponseFields.f_SetHTTPVersion(_Version);
		mp_pD->m_GeneralFields.f_SetHTTPVersion(_Version);
		mp_pD->m_EntityFields.f_SetHTTPVersion(_Version);
	}

	CResponseFields &CResponseHeader::f_GetResponseFields()
	{
		DMibRequire(mp_pD->m_Status.f_GetStatus() != EStatus_Unknown);
		return mp_pD->m_ResponseFields;
	}

	CGeneralFields &CResponseHeader::f_GetGeneralFields()
	{
		DMibRequire(mp_pD->m_Status.f_GetStatus() != EStatus_Unknown);
		return mp_pD->m_GeneralFields;
	}

	CEntityFields &CResponseHeader::f_GetEntityFields()
	{
		DMibRequire(mp_pD->m_Status.f_GetStatus() != EStatus_Unknown);
		return mp_pD->m_EntityFields;
	}

	CStatusLine const &CResponseHeader::f_GetStatusLine() const
	{
		return mp_pD->m_Status;
	}

	// Fields
	CResponseContent CResponseHeader::f_Complete()
	{
		DMibRequire(mp_pD->m_Status.f_GetStatus() != EStatus_Unknown);
		mp_pD->m_Status.f_Write(mp_pD->m_OutputMethod);
		mp_pD->m_ResponseFields.f_WriteToData(mp_pD->m_OutputMethod);
		mp_pD->m_GeneralFields.f_WriteToData(mp_pD->m_OutputMethod);
		mp_pD->m_EntityFields.f_WriteToData(mp_pD->m_OutputMethod);
		mp_pD->m_OutputMethod((uint8 const *)"\r\n", 2);
		return CResponseContent(fg_Move(mp_pD));
	}

	//
	// CRequest::CDetails Private Methods
	//
	EParse CResponseDetails::fp_ParseHeader(NContainer::CPagedByteVector& _Data)
	{
		// A header will always end with a CRLFCRLF sequence
		// So we use that to detect if we have a complete header available.
		static uint8 const s_CrLfCrLf[] = { '\r', '\n', '\r', '\n'};
		NStr::CStr RequestText;
		mint iEnd;
		if
		(
			_Data.f_ReadFrontUntil
			(
				 s_CrLfCrLf
				, sizeof(s_CrLfCrLf)
				, iEnd
				, [&](mint _iStart, uint8 const* _pPtr, mint _nBytes, mint _nTotalBytes)
				{
					RequestText.f_AddStr((ch8 const *)_pPtr, _nBytes);
					return true;
				}
			)
		)
		{
			_Data.f_RemoveFront(iEnd + sizeof(s_CrLfCrLf));

			// We have a complete header so lets parse that.
			return fp_ParseHeaderText(RequestText);
		}
		else
		{
			return EParse_Incomplete;
		}
	}

	NContainer::TCVector<NStr::CStr> fg_SplitStringOn(NStr::CStr const& _Source, NStr::CStr const& _Sep);

	// RFC 2616 Section 4 HTTP Message
	EParse CResponseDetails::fp_ParseHeaderText(NStr::CStr _Text)
	{
		NContainer::TCVector<NStr::CStr> lLines = fg_SplitStringOn(_Text, "\r\n");

		if (lLines.f_IsEmpty())
		{
			mp_Errors = "Request text contained nothing!";
			return EParse_Invalid;
		}

		// Parse the status line
		EParse StatusResult = m_Status.f_Parse(lLines[0], mp_Errors);

		if (StatusResult != EParse_OK)
			return StatusResult;

		// Parse the remaining header lines

		for (auto LIter = ++lLines.f_GetIterator(); LIter; ++LIter)
		{
			NStr::CStr const& CurLine = *LIter;

			if (CurLine == "")
				break;

			aint iColonSpace = CurLine.f_Find(": ");
			if (iColonSpace == -1)
			{
				mp_Errors = "Request was malformed. Invalid header line.";
				return EParse_Invalid;
			}

			NStr::CStr Name = CurLine.f_Extract(0, iColonSpace);
			NStr::CStr Value = CurLine.f_Extract(iColonSpace + 2);

			EParse FieldParse = fp_ParseField(Name, Value);

			if (FieldParse == EParse_Invalid)
				return EParse_Invalid;
		}

		return EParse_OK;
	}

	EParse CResponseDetails::fp_ParseField(NStr::CStr const& _Name, NStr::CStr const& _Value)
	{
		// First see if the fields is a known general field.
		EParse ParseStatus = m_GeneralFields.f_ParseField(_Name, _Value);
		if (ParseStatus == EParse_Invalid)
		{
			mp_Errors = NStr::fg_Format("Invalid format for general field \"{}\": \"{}\"", _Name, _Value);
		}

		// Second see if the field is a known request field.
		if (ParseStatus == EParse_Unknown)
		{
			ParseStatus = m_ResponseFields.f_ParseField(_Name, _Value);
			if (ParseStatus == EParse_Invalid)
			{
				mp_Errors = NStr::fg_Format("Invalid format for request field \"{}\": \"{}\"", _Name, _Value);
			}
		}

		// Third see if the field is a known entity field.
		if (ParseStatus == EParse_Unknown)
		{
			ParseStatus = m_EntityFields.f_ParseField(_Name, _Value);
			if (ParseStatus == EParse_Invalid)
			{
				mp_Errors = NStr::fg_Format("Invalid format for entity field \"{}\": \"{}\"", _Name, _Value);
			}
			// Unknown fields are stored in the entity fields object (as per standard)
		}

		return ParseStatus;
	}


	EResponseStatus CResponseDetails::f_Parse(NContainer::CPagedByteVector& _Data)
	{

		if (mp_Status == EResponseStatus_Empty)
			mp_ParseState = EParseState_Header;
		else if (mp_Status != EResponseStatus_InProgress)
			return EResponseStatus_Invalid;

		bool bContinueParsing = true;

		// EParse is used as a result from the various sub-parsing methods
		// This lambda takes that result and the parse state to move to
		// and sets the real parse state along with a flag on whether or not
		// to continue parsing.
		auto fl_HandleParseResult =
			[&](EParse _Result, EParseState _NextParseState, bool& _bContinueParsing)
			{
				switch (_Result)
				{
					case EParse_NotPresent:
					case EParse_Unknown:
					case EParse_Incomplete:
						break;
					case EParse_OK:
						// Header complete, continue with trying to parse content
						mp_ParseState = _NextParseState;
						if (mp_ParseState != EParseState_Complete)
							bContinueParsing = true;
						break;
					case EParse_Invalid:
						mp_ParseState = EParseState_Invalid;
						break;
				}
			}
		;

		// Multiple runs of this loop may occur if one parse state completes and work can
		// move on to the next one.

		while (bContinueParsing)
		{
			bContinueParsing = false;

			switch(mp_ParseState)
			{
				case EParseState_Header:
					{
						fl_HandleParseResult( fp_ParseHeader(_Data), EParseState_Complete, bContinueParsing );
						break;
					}

				case EParseState_Complete:
					break;

				case EParseState_Invalid:
					break;

				default:
					break;
			}

		}

		if (mp_ParseState == EParseState_Complete)
			mp_Status = EResponseStatus_Complete;
		else if (mp_ParseState == EParseState_Invalid)
			mp_Status = EResponseStatus_Invalid;
		else
			mp_Status = EResponseStatus_InProgress;

		return mp_Status;
	}

	EResponseStatus CResponseHeader::f_Parse(NContainer::CPagedByteVector& _Data)
	{
		return mp_pD->f_Parse(_Data);
	}


	//
	// CResponseContent
	//

	CResponseContent::CResponseContent(CResponseContent&& _ToMove)
		: CResponseStage(fg_Move(_ToMove))
	{

	}

	CResponseContent::CResponseContent(NStorage::TCUniquePointer<CResponseDetails> _pD)
		: CResponseStage(fg_Move(_pD))
	{

	}

	CResponseContent::~CResponseContent()
	{

	}


	// Content related
	/*
	NStream::CBinaryStreamMemory<>& CResponseContent::f_ContentStream()
	{
	}
	 */

	CResponseTrailer CResponseContent::f_Complete()
	{
		return CResponseTrailer(fg_Move(mp_pD));
	}


	void CResponseContent::f_SendData(uint8 const *_pData, mint _nBytes)
	{
		mp_pD->m_OutputMethod(_pData, _nBytes);
	}

	void CResponseContent::f_SendString(NStr::CStr const &_String)
	{
		NStr::CStr Data = _String;
		mp_pD->m_OutputMethod((uint8 const *)Data.f_GetStr(), Data.f_GetLen());
	}


	//
	// CResponseTrailer Public Methods
	//

	CResponseTrailer::CResponseTrailer(CResponseTrailer&& _ToMove)
		: CResponseStage(fg_Move(_ToMove))
	{

	}
	CResponseTrailer::CResponseTrailer(NStorage::TCUniquePointer<CResponseDetails> _pD)
		: CResponseStage(fg_Move(_pD))
	{

	}
	CResponseTrailer::~CResponseTrailer()
	{

	}

	void CResponseTrailer::f_AddField(NStr::CStr const& _Name, NStr::CStr const& _Value)
	{

	}

	void CResponseTrailer::f_Complete()
	{

	}
}
