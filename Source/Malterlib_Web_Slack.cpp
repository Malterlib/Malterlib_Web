// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_Slack.h"
#include <Mib/Encoding/JSON>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Web/Curl>

namespace NMib::NWeb
{
	using namespace NEncoding;
	using namespace NStorage;
	using namespace NContainer;
	using namespace NStr;

	struct CSlackActor::CInternal : public NConcurrency::CActorInternal
	{
		CInternal(NConcurrency::TCActor<CCurlActor> const &_CurlActor)
			: m_CurlActor{_CurlActor}
		{
		}

		NConcurrency::TCActor<CCurlActor> m_CurlActor;
	};

	CSlackActor::CSlackActor(NConcurrency::TCActor<CCurlActor> const &_CurlActor)
		: mp_pInternal{fg_Construct(_CurlActor)}
	{
	}

	CSlackActor::~CSlackActor() = default;


	CStr CSlackActor::CMessage::fs_EscapeString(CStr const &_String)
	{
		CUStr UnicodeString = _String;

		CUStr OutString;

		for (auto pParse = UnicodeString.f_GetStr(); *pParse; ++pParse)
		{
			switch (*pParse)
			{
			case '`':
			case '<':
			case '>':
			case '*':
			case '~':
				OutString.f_AddChar(0x0C);
				OutString.f_AddChar(*pParse);
				OutString.f_AddChar(0x0C);
				break;
			default:
				OutString.f_AddChar(*pParse);
				break;
			}
		}

		return OutString;
	}

	namespace
	{
		CEJSONSorted fg_MessageToJson(CSlackActor::CMessage const &_Message)
		{
			CEJSONSorted SlackMessage(EJSONType_Object);

			if (_Message.m_Text)
				SlackMessage["text"] = *_Message.m_Text;
			if (_Message.m_Channel)
				SlackMessage["channel"] = *_Message.m_Channel;
			if (_Message.m_UserName)
				SlackMessage["username"] = *_Message.m_UserName;
			if (_Message.m_IconEmoji)
				SlackMessage["icon_emoji"] = *_Message.m_IconEmoji;
			if (_Message.m_IconURL)
				SlackMessage["icon_url"] = *_Message.m_IconURL->f_Encode();

			if (_Message.m_bMarkdown)
				SlackMessage["mrkdwn"] = *_Message.m_bMarkdown;
			if (_Message.m_bLinkNames)
				SlackMessage["link_names"] = *_Message.m_bLinkNames;
			if (_Message.m_bReplyBroadcast)
				SlackMessage["reply_broadcast"] = *_Message.m_bReplyBroadcast;
			if (_Message.m_bUnfurlLinks)
				SlackMessage["unfurl_links"] = *_Message.m_bUnfurlLinks;
			if (_Message.m_bUnfurlMedia)
				SlackMessage["unfurl_media"] = *_Message.m_bUnfurlMedia;
			if (_Message.m_bFullParse)
				SlackMessage["unfurl_media"] = *_Message.m_bFullParse ? "full" : "none";

			if (_Message.m_ThreadTimestamp)
				SlackMessage["thread_ts"] = *_Message.m_ThreadTimestamp;

			for (auto &Attachment : _Message.m_Attachments)
			{
				auto &OutputAttachment = SlackMessage["attachments"].f_Array().f_Insert() =
					{
						"fallback"_= Attachment.m_Fallback
						, "title"_= Attachment.m_Title
						, "text"_= Attachment.m_Text
					}
				;

				if (Attachment.m_bPretextMarkdown && *Attachment.m_bPretextMarkdown)
					OutputAttachment["mrkdwn_in"].f_Array().f_Insert("pretext");
				if (Attachment.m_bTextMarkdown && *Attachment.m_bTextMarkdown)
					OutputAttachment["mrkdwn_in"].f_Array().f_Insert("text");
				if (Attachment.m_bFieldsMarkdown && *Attachment.m_bFieldsMarkdown)
					OutputAttachment["mrkdwn_in"].f_Array().f_Insert("fields");

				if (Attachment.m_PreText)
					OutputAttachment["pretext"] = *Attachment.m_PreText;
				if (Attachment.m_TitleLink)
					OutputAttachment["title_link"] = *Attachment.m_TitleLink;

				if (Attachment.m_Color)
				{
					if (Attachment.m_Color->f_GetTypeID() == 0)
					{
						auto ColorEnum = Attachment.m_Color->f_Get<0>();
						switch (ColorEnum.m_Color)
						{
						case CSlackActor::EPredefinedColor_Good: OutputAttachment["color"] = "good"; break;
						case CSlackActor::EPredefinedColor_Warning: OutputAttachment["color"] = "warning"; break;
						case CSlackActor::EPredefinedColor_Danger: OutputAttachment["color"] = "danger"; break;
						}
					}
					else
					{
						auto &Color = Attachment.m_Color->f_Get<1>();
						OutputAttachment["color"] = "#{nfh,sj2,sf0}{nfh,sj2,sf0}{nfh,sj2,sf0}"_f << Color.m_Red << Color.m_Green << Color.m_Blue;
					}
				}

				if (Attachment.m_AuthorName)
					OutputAttachment["author_name"] = *Attachment.m_AuthorName;
				if (Attachment.m_AuthorLink)
					OutputAttachment["author_link"] = Attachment.m_AuthorLink->f_Encode();
				if (Attachment.m_AuthorIcon)
					OutputAttachment["author_icon"] = Attachment.m_AuthorIcon->f_Encode();

				if (Attachment.m_ImageURL)
					OutputAttachment["image_url"] = Attachment.m_ImageURL->f_Encode();
				if (Attachment.m_ThumbURL)
					OutputAttachment["thumb_url"] = Attachment.m_ThumbURL->f_Encode();

				if (Attachment.m_Footer)
					OutputAttachment["footer"] = *Attachment.m_Footer;
				if (Attachment.m_FooterIconURL)
					OutputAttachment["thumb_url"] = Attachment.m_FooterIconURL->f_Encode();
				if (Attachment.m_FooterTimestamp)
					OutputAttachment["ts"] = NTime::CTimeConvert(*Attachment.m_FooterTimestamp).f_UnixSeconds();

				for (auto &Field : Attachment.m_Fields)
				{
					auto &OutputField = OutputAttachment["fields"].f_Array().f_Insert() =
						{
							"title"_= Field.m_Title
							, "value"_= Field.m_Value
						}
					;

					if (Field.m_bShort)
						OutputField["short"] = *Field.m_bShort;
				}
			}

			return SlackMessage;
		}
	}

	NConcurrency::TCFuture<NStr::CStr> CSlackActor::f_PostMessage(CStr _Token, CMessage _Message)
	{
		auto &Internal = *mp_pInternal;

		CEJSONSorted SlackMessage = fg_MessageToJson(_Message);
		auto SlackMessageString = SlackMessage.f_ToString();

		TCMap<CStr, CStr> Headers;
		Headers["Authorization"] = "Bearer {}"_f << _Token;
		Headers["Content-Type"] = "application/json";

		auto Result = co_await Internal.m_CurlActor
			(
				&CCurlActor::f_Request
				,CCurlActor::EMethod_POST
				, "https://slack.com/api/chat.postMessage"
				, fg_Move(Headers)
				, CByteVector((uint8 const *)SlackMessageString.f_GetStr(), SlackMessageString.f_GetLen())
				, TCMap<CStr, CStr>{}
			)
		;

		if (Result.m_StatusCode != 200)
			co_return DMibErrorInstance("Slack request failed with status {}: {}"_f << Result.m_StatusCode << Result.m_Body);

		{
			auto CaptureScope = co_await NConcurrency::g_CaptureExceptions;

			auto JsonResult = fg_Const(Result.f_ToJson());

			if (!JsonResult["ok"].f_Boolean())
				co_return DMibErrorInstance("Slack request failed with error: {}"_f << JsonResult["error"].f_String());

			co_return JsonResult["ts"].f_String();
		}

		co_return {};
	}

	NConcurrency::TCFuture<NStr::CStr> CSlackActor::f_UpdateMessage(CStr _Token, CStr _Timestamp, CMessage _Message)
	{
		auto &Internal = *mp_pInternal;

		CEJSONSorted SlackMessage = fg_MessageToJson(_Message);
		SlackMessage["ts"] = _Timestamp;
		SlackMessage["as_user"] = true;

		auto SlackMessageString = SlackMessage.f_ToString();

		TCMap<CStr, CStr> Headers;
		Headers["Authorization"] = "Bearer {}"_f << _Token;
		Headers["Content-Type"] = "application/json";

		auto Result = co_await Internal.m_CurlActor
			(
				&CCurlActor::f_Request
				,CCurlActor::EMethod_POST
				, "https://slack.com/api/chat.update"
				, fg_Move(Headers)
				, CByteVector((uint8 const *)SlackMessageString.f_GetStr(), SlackMessageString.f_GetLen())
				, TCMap<CStr, CStr>{}
			)
		;

		if (Result.m_StatusCode != 200)
			co_return DMibErrorInstance("Slack request failed with status {}: {}"_f << Result.m_StatusCode << Result.m_Body);

		{
			auto CaptureScope = co_await NConcurrency::g_CaptureExceptions;

			auto JsonResult = fg_Const(Result.f_ToJson());

			if (!JsonResult["ok"].f_Boolean())
				co_return DMibErrorInstance("Slack request failed with error: {}"_f << JsonResult["error"].f_String());

			co_return JsonResult["ts"].f_String();
		}

		co_return {};
	}

	NConcurrency::TCFuture<void> CSlackActor::f_SendMessage(NHTTP::CURL _IncomingWebhook, CMessage _Message)
	{
		auto &Internal = *mp_pInternal;

		CEJSONSorted SlackMessage = fg_MessageToJson(_Message);

		auto SlackMessageString = SlackMessage.f_ToString();
		auto Result = co_await Internal.m_CurlActor
			(
				&CCurlActor::f_Request
				,CCurlActor::EMethod_POST
				, _IncomingWebhook.f_Encode()
				, TCMap<CStr, CStr>{}
				, CByteVector((uint8 const *)SlackMessageString.f_GetStr(), SlackMessageString.f_GetLen())
				, TCMap<CStr, CStr>{}
			)
		;

		if (Result.m_StatusCode != 200)
			co_return DMibErrorInstance("Slack request failed with status {}: {}"_f << Result.m_StatusCode << Result.m_Body);

		co_return {};
	}
}
