// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/XML/XML>

namespace NMib::NWeb
{
	template <typename tf_CReturn>
	void fg_ReportAWSError(TCContinuation<tf_CReturn> const &_Continuation, CCurlActor::CResult &_Result, ch8 const *_pRequestDescription)
	{
		NXML::CXMLDocument ErrorReturn;
		do
		{
			if (!ErrorReturn.f_ParseString(_Result.m_Body))
				break;

			auto pErrorRoot = ErrorReturn.f_GetChildNode(ErrorReturn.f_GetRootNode(), "ErrorResponse");

			if (!pErrorRoot)
				pErrorRoot = ErrorReturn.f_GetRootNode();

			auto pErrorNode = ErrorReturn.f_GetChildNode(pErrorRoot, "Error");

			if (!pErrorNode)
				break;

			auto pCodeNode = ErrorReturn.f_GetChildNode(pErrorNode, "Code");
			auto pMessageNode = ErrorReturn.f_GetChildNode(pErrorNode, "Message");
			if (!pCodeNode || !pMessageNode)
				break;

			auto Code = ErrorReturn.f_GetNodeText(pCodeNode);
			auto Message = ErrorReturn.f_GetNodeText(pMessageNode);
			if (!Code || !Message)
				break;

			_Continuation.f_SetException(DMibErrorInstance("{} request failed with status {}: {} - {}"_f << _pRequestDescription << _Result.m_StatusCode << Code << Message));
			return;
		}
		while (false);

		_Continuation.f_SetException(DMibErrorInstance("{} request failed with status {}: {}"_f << _pRequestDescription << _Result.m_StatusCode << _Result.m_Body));
	}
}
