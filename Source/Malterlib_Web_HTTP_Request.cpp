// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTP_Request.h"
#include "Malterlib_Web_HTTP_Request_Internal.h"
#include "Malterlib_Web_HTTP_Utilities.h"

namespace NMib::NWeb::NHTTP
{
	// For debug code
	static char const* gc_RequestParseStates[]
		=
		{
			"EParseState_Header"
			, "EParseState_Content"
			, "EParseState_Chunked_Plain"
			, "EParseState_Content_Chunked"
			, "EParseState_Content_Chunked_Trailers"
			, "EParseState_DecodeContent"
			, "EParseState_Complete"
			, "EParseState_Invalid"
		}
	;


	//
	// CRequest Public Methods
	//

	CRequest::CRequest()
		: mp_pD(fg_Construct())
	{
	}

	CRequest::CRequest(CRequest&& _ToMove)
		: mp_pD(fg_Move(_ToMove.mp_pD))
	{
		_ToMove.mp_pD = fg_Construct();
	}

	CRequest::~CRequest()
	{
		mp_pD = nullptr;
	}

	CRequest& CRequest::operator=(CRequest&& _ToMove)
	{
		mp_pD = fg_Move(_ToMove.mp_pD);
		_ToMove.mp_pD = fg_Construct();

		return *this;
	}

	void CRequest::f_Clear()
	{
		return mp_pD->f_Clear();
	}

	ERequestStatus CRequest::f_GetStatus() const
	{
		return mp_pD->f_GetStatus();
	}

	NStr::CStr CRequest::f_GetErrors() const
	{
		return mp_pD->f_GetErrors();
	}

	/*
		f_Parse parses data as it comes from the network.
		As soon as request data is received this is called with that
		data.

		f_Parse returns:
			ERequestStatus_Complete
				- The request is complete, all relevant data has been extracted from _Data

			ERequestStatus_Invalid
				- The request was invalid, use f_GetErrors() for more info. The connection should be dropped.

			ERequestStatus_InProgress
				- The request is not complete yet and f_Parse should be called again as new data arrives.
					_Data may or may not have been altered in this case.
	*/
	ERequestStatus CRequest::f_Parse(NContainer::CPagedByteVector& _Data)
	{
		return mp_pD->f_Parse(_Data);
	}

	void CRequest::f_WriteHeaders(COutputMethod const &_fOutput)
	{
		mp_pD->f_WriteHeaders(_fOutput);
	}

	CRequestLine const& CRequest::f_GetRequestLine() const
	{
		return mp_pD->f_GetRequestLine();
	}
	CGeneralFields const& CRequest::f_GetGeneralFields() const
	{
		return mp_pD->f_GetGeneralFields();
	}
	CRequestFields const& CRequest::f_GetRequestFields() const
	{
		return mp_pD->f_GetRequestFields();
	}
	CEntityFields const& CRequest::f_GetEntityFields() const
	{
		return mp_pD->f_GetEntityFields();
	}


	CRequestLine &CRequest::f_GetRequestLine()
	{
		return mp_pD->f_GetRequestLine();
	}

	CGeneralFields &CRequest::f_GetGeneralFields()
	{
		return mp_pD->f_GetGeneralFields();
	}

	CRequestFields &CRequest::f_GetRequestFields()
	{
		return mp_pD->f_GetRequestFields();
	}

	CEntityFields &CRequest::f_GetEntityFields()
	{
		return mp_pD->f_GetEntityFields();
	}



	//
	// CRequest::CDetails Public Methods
	//

	CRequest::CDetails::CDetails()
		: mp_Status(ERequestStatus_Empty)
		, mp_ParseState(EParseState_Header)
	{
	}

	CRequest::CDetails::~CDetails()
	{
		f_Clear();
	}


	void CRequest::CDetails::f_Clear()
	{
		mp_Status = ERequestStatus_Empty;
		mp_Errors.f_Clear();
		mp_ParseState = EParseState_Header;
		mp_RequestLine.f_Clear();
		mp_GeneralFields.f_Clear();
		mp_RequestFields.f_Clear();
		mp_EntityFields.f_Clear();
	}

	ERequestStatus CRequest::CDetails::f_GetStatus() const
	{
		return mp_Status;
	}

	NStr::CStr CRequest::CDetails::f_GetErrors() const
	{
		return mp_Errors;
	}

	ERequestStatus CRequest::CDetails::f_Parse(NContainer::CPagedByteVector& _Data)
	{
		if (mp_Status == ERequestStatus_Empty)
			mp_ParseState = EParseState_Header;
		else if (mp_Status != ERequestStatus_InProgress)
			return ERequestStatus_Invalid;

		bint bContinueParsing = true;

		// EParse is used as a result from the various sub-parsing methods
		// This lambda takes that result and the parse state to move to
		// and sets the real parse state along with a flag on whether or not
		// to continue parsing.
		auto fl_HandleParseResult =
			[&](EParse _Result, EParseState _NextParseState, bint& _bContinueParsing)
			{
				switch( _Result )
				{
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

			(void)gc_RequestParseStates;
			//DMibTrace("CRequest::f_Parse() State: {}\n", gc_RequestParseStates[(int)mp_ParseState]);

			switch(mp_ParseState)
			{
				case EParseState_Header:
					{
						fl_HandleParseResult( fp_ParseHeader(_Data), EParseState_Content, bContinueParsing );
						break;
					}

				case EParseState_Content:
					{
						if ( fp_ParseContent(_Data, mp_ParseState) )
						{
							bContinueParsing = true;
						}
						else
						{
							// Did not know what type of content to parse.
							mp_ParseState = EParseState_Invalid;
						}
						break;
					}

				case EParseState_Chunked_Plain:
					{
						fl_HandleParseResult( fp_ParseContent_Plain(_Data), EParseState_DecodeContent, bContinueParsing );
						break;
					}

				case EParseState_Content_Chunked:
					{
						fl_HandleParseResult( fp_ParseContent_Chunked(_Data), EParseState_Content_Chunked_Trailers, bContinueParsing );
						break;
					}

				case EParseState_Content_Chunked_Trailers:
					{
						fl_HandleParseResult( fp_ParseContent_Chunked_Trailers(_Data), EParseState_DecodeContent, bContinueParsing);
						break;
					}

				case EParseState_DecodeContent:
					break;

				case EParseState_Complete:
					break;

				case EParseState_Invalid:
					break;

				default:
					break;
			}

		}

		if (mp_ParseState == EParseState_Complete)
			mp_Status = ERequestStatus_Complete;
		else if (mp_ParseState == EParseState_Invalid)
			mp_Status = ERequestStatus_Invalid;
		else
			mp_Status = ERequestStatus_InProgress;

		return mp_Status;
	}

	void CRequest::CDetails::f_WriteHeaders(COutputMethod const &_fOutput)
	{
		mp_RequestLine.f_Write(_fOutput);

		mp_GeneralFields.f_WriteToData(_fOutput);
		mp_RequestFields.f_WriteToData(_fOutput);
		mp_EntityFields.f_WriteToData(_fOutput);

		_fOutput((uint8 const *)"\r\n", 2);
	}

	CRequestLine const &CRequest::CDetails::f_GetRequestLine() const
	{
		return mp_RequestLine;
	}

	CGeneralFields const &CRequest::CDetails::f_GetGeneralFields() const
	{
		return mp_GeneralFields;
	}

	CRequestFields const &CRequest::CDetails::f_GetRequestFields() const
	{
		return mp_RequestFields;
	}

	CEntityFields const &CRequest::CDetails::f_GetEntityFields() const
	{
		return mp_EntityFields;
	}


	CRequestLine &CRequest::CDetails::f_GetRequestLine()
	{
		return mp_RequestLine;
	}

	CGeneralFields &CRequest::CDetails::f_GetGeneralFields()
	{
		return mp_GeneralFields;
	}

	CRequestFields &CRequest::CDetails::f_GetRequestFields()
	{
		return mp_RequestFields;
	}

	CEntityFields &CRequest::CDetails::f_GetEntityFields()
	{
		return mp_EntityFields;
	}


/*
	NStr::CStr CRequest::f_GetFieldAsList(int _iField) const
	{
		int nFields = (int)mp_Fields.f_GetLen();
		NStr::CStr Result;

		NStr::CStr FieldName = mp_Fields[_iField].first;
		Result = mp_Fields[_iField].second;
		++_iField;

		for (;_iField < nFields;++_iField)
		{
			if (mp_Fields[_iField].first.f_CmpNoCase(FieldName) == 0 )
			{
				if (!Result.f_IsEmpty())
					Result += ",";

				Result += mp_Fields[_iField].second;
			}
		}

		return Result;
	}
*/

	//
	// CRequest::CDetails Private Methods
	//
	EParse CRequest::CDetails::fp_ParseHeader(NContainer::CPagedByteVector& _Data)
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
					RequestText.f_AddStr(_pPtr, _nBytes);
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

	// RFC 2616 Section 4 HTTP Message
	EParse CRequest::CDetails::fp_ParseHeaderText(NStr::CStr _Text)
	{
		NContainer::TCVector<NStr::CStr> lLines = fg_SplitStringOn(_Text, "\r\n");

		if (lLines.f_IsEmpty())
		{
			mp_Errors = "Request text contained nothing!";
			return EParse_Invalid;
		}

		// Parse the status line
		EParse StatusResult = mp_RequestLine.f_Parse(lLines[0], mp_Errors);

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

	bool CRequest::CDetails::fp_ParseContent(NContainer::CPagedByteVector const& _Data, EParseState& _oNextParseState)
	{
		bint bContentExpected = false;

		switch(mp_RequestLine.f_GetMethod())
		{
		case EMethod_Options:
			{
				// RFC 2616 Section 9.2 OPTIONS
				if (mp_EntityFields.f_HasField(EEntityField_ContentLength))
				{
					bContentExpected = mp_EntityFields.f_GetContentLength() != 0;
				}
				else
				{
					_oNextParseState = EParseState_Invalid;
					return false;
				}
				break;
			}
		case EMethod_Get:
			{
				// RFC 2616 Section 9.3 GET
				break;
			}
		case EMethod_Head:
			{
				// RFC 2616 Section 9.4 HEAD
				break;
			}
		case EMethod_Post:
			{
				// RFC 2616 Section 9.5 POST
				bContentExpected = true;
				break;
			}
		case EMethod_Put:
			{
				// RFC 2616 Section 9.6 PUT
				bContentExpected = true;
				break;
			}
		case EMethod_Delete:
			{
				// RFC 2616 Section 9.7 DELETE
				break;
			}
		case EMethod_Trace:
			{
				// RFC 2616 Section 9.3 TRACE
				break;
			}
		case EMethod_Connect:
			{
				// RFC 2616 Section 9.3 CONNECT
				break;
			}
		}

		if (!bContentExpected)
		{
			_oNextParseState = EParseState_Complete;
			return true;
		}

		ETransferEncoding TransferEncoding = mp_GeneralFields.f_GetTransferEncoding();

		switch(TransferEncoding)
		{
		case ETransferEncoding_Identity:
		case ETransferEncoding_Unknown: // Default is Identity
			{
				_oNextParseState = EParseState_Chunked_Plain;
				return true;
			}

		case ETransferEncoding_Chunked:
		case ETransferEncoding_GZip:
		case ETransferEncoding_Deflate:
		case ETransferEncoding_Compress:
			{
				_oNextParseState = EParseState_Content_Chunked;
				return true;
			}

		default:
			{ // Unsupported
				return false;
			}
		}
	}

	EParse CRequest::CDetails::fp_ParseContent_Plain(NContainer::CPagedByteVector& _Data)
	{
		if (!mp_EntityFields.f_HasField(EEntityField_ContentLength))
			return EParse_Invalid;

		mint ContentLength = mp_EntityFields.f_GetContentLength();

		if (_Data.f_GetLen() >= ContentLength)
		{
			mp_Content.f_SetLen(ContentLength);
			mint iCurPos = 0;

			_Data.f_ReadFront
				(
					ContentLength
					, [&](mint _iStart, uint8 const* _pPtr, mint _nBytes) -> bint
					{
						NMemory::fg_MemCopy(mp_Content.f_GetArray() + iCurPos, _pPtr, _nBytes);
						iCurPos += _nBytes;
						return true;
					}
				)
			;

			return EParse_OK;
		}
		else
		{
			return EParse_Incomplete;
		}
	}

	// RFC 2616 Section 3.6.1 HTTP Message
	EParse CRequest::CDetails::fp_ParseContent_Chunked(NContainer::CPagedByteVector& _Data)
	{
		// Read chunks

		static uint8 const CRLF[] = { '\r', '\n' };
		NStr::CStr ChunkHeaderLine;
		mint ChunkSize;
		mint iPos;

		while(1)
		{
			// Read chunk header line and parse chunk size.
			if (!fg_PeekLine(_Data, iPos, ChunkHeaderLine))
			{
				mp_Errors = "Expected a chunk header line.";
				return EParse_Incomplete;
			}

			NContainer::TCVector<NStr::CStr> HeaderSplit = fg_SplitStringOn(ChunkHeaderLine, " ");
			if (HeaderSplit.f_GetLen() == 0)
			{
				mp_Errors = "Invalid chunk header line.";
				return EParse_Invalid;
			}

			{
				aint nParsed = 0;
				(NStr::CStr::CParse("{nfh}") >> ChunkSize).f_Parse(HeaderSplit[0], nParsed);
				if (nParsed != 1)
				{
					mp_Errors = "Could not parse chunk header line size.";
					return EParse_Invalid;
				}
			}

			// Check to see if we are at the last chunk.
			if( ChunkSize == 0)
			{
				// This is the last chunk
				break;
			}

			// Check the required chunk data is present (plus two chars for CRLF)
			if ( (iPos + ChunkSize + 2) > _Data.f_GetLen() )
			{
				mp_Errors = "Incomplete chunk.";
				return EParse_Incomplete;
			}

			// Remove status line
			_Data.f_RemoveFront(iPos);

			_Data.f_ReadFront
				(
					ChunkSize
					, [&](mint _iStart, uint8 const* _pPtr, mint _nBytes) -> bint
					{
						mp_Content.f_Insert(_pPtr, _nBytes);
						return true;
					}
				)
			;

			_Data.f_RemoveFront(ChunkSize);

			// Expect CRLF after chunk data.
			if (!_Data.f_ExpectAndRemoveFront(CRLF, sizeof(CRLF)))
			{
				mp_Errors = "No terminating CRLF after chunk.";
				return EParse_Invalid;
			}
		}

		return EParse_OK;
	}

	// RFC 2616 Section 3.6.1 HTTP Message
	EParse CRequest::CDetails::fp_ParseContent_Chunked_Trailers(NContainer::CPagedByteVector& _Data)
	{
		// Read trailers

		// The standard is a bit vague regarding how to detect if trailers definitely follow a chunk list.
		// For now we will rely on the "Trailers" header field.

		if (mp_GeneralFields.f_HasField(EGeneralField_Trailer))
		{

			NStr::CStr TrailerLine;
			mint iPos;

			while (fg_PeekLine(_Data, iPos, TrailerLine))
			{
				if (TrailerLine.f_IsEmpty())
				{
					// We have reached the end
					return EParse_OK;
				}

				aint iColonSpace = TrailerLine.f_Find(": ");
				if (iColonSpace == -1)
				{
					mp_Errors = "Trailer was malformed. Invalid header line.";
					return EParse_Invalid;
				}

				NStr::CStr Name = TrailerLine.f_Extract(0, iColonSpace);
				NStr::CStr Value = TrailerLine.f_Extract(iColonSpace + 2);

				if (fp_ParseField(Name, Value) == EParse_Invalid)
				{
					return EParse_Invalid;
				}
			}

			// We are still parsing trailers.
			return EParse_Incomplete;
		}
		else
		{
			// Expect CRLF at the end.
			static uint8 const CRLF[] = { '\r', '\n' };
			if (!_Data.f_ExpectAndRemoveFront(CRLF, sizeof(CRLF)))
			{
				mp_Errors = "No terminating CRLF after last chunk.";
				return EParse_Invalid;
			}

			return EParse_OK;
		}
	}

	EParse CRequest::CDetails::fp_ParseField(NStr::CStr const& _Name, NStr::CStr const& _Value)
	{
		// First see if the fields is a known general field.
		EParse ParseStatus = mp_GeneralFields.f_ParseField(_Name, _Value);
		if (ParseStatus == EParse_Invalid)
		{
			mp_Errors = NStr::fg_Format("Invalid format for general field \"{}\": \"{}\"", _Name, _Value);
		}

		// Second see if the field is a known request field.
		if (ParseStatus == EParse_Unknown)
		{
			ParseStatus = mp_RequestFields.f_ParseField(_Name, _Value);
			if (ParseStatus == EParse_Invalid)
			{
				mp_Errors = NStr::fg_Format("Invalid format for request field \"{}\": \"{}\"", _Name, _Value);
			}
		}

		// Third see if the field is a known entity field.
		if (ParseStatus == EParse_Unknown)
		{
			ParseStatus = mp_EntityFields.f_ParseField(_Name, _Value);
			if (ParseStatus == EParse_Invalid)
			{
				mp_Errors = NStr::fg_Format("Invalid format for entity field \"{}\": \"{}\"", _Name, _Value);
			}
			// Unknown fields are stored in the entity fields object (as per standard)
		}

		return ParseStatus;
	}
}
