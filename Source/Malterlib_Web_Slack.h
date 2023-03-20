// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Encoding/EJSON>
#include <Mib/Storage/Optional>
#include <Mib/Web/HTTP/URL>

namespace NMib::NWeb
{
	struct CCurlActor;

	struct CSlackActor : public NConcurrency::CActor
	{
		enum EPredefinedColor
		{
			EPredefinedColor_Good
			, EPredefinedColor_Warning
			, EPredefinedColor_Danger
		};

		struct CPredefinedColor
		{
			constexpr CPredefinedColor(EPredefinedColor _Color);
			CPredefinedColor(CPredefinedColor const &) = default;
			CPredefinedColor& operator = (CPredefinedColor const &) = default;

			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			EPredefinedColor m_Color;
		};

		struct CRgbColor
		{
			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;
			
			uint8 m_Red = 0;
			uint8 m_Green = 0;
			uint8 m_Blue = 0;
		};

		using CColor = NStorage::TCVariant<CPredefinedColor, CRgbColor>;

		struct CField
		{
			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			NStr::CStr m_Title;
			NStr::CStr m_Value;
			NStorage::TCOptional<bool> m_bShort;
		};

		struct CAttachment
		{
			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			NStr::CStr m_Fallback;

			NStorage::TCOptional<NStr::CStr> m_PreText;
			NStorage::TCOptional<bool> m_bPretextMarkdown;

			NStr::CStr m_Title;
			NStorage::TCOptional<NStr::CStr> m_TitleLink;

			NStr::CStr m_Text;
			NStorage::TCOptional<bool> m_bTextMarkdown;

			NStorage::TCOptional<CColor> m_Color;

			NStorage::TCOptional<NStr::CStr> m_AuthorName;
			NStorage::TCOptional<NHTTP::CURL> m_AuthorLink;
			NStorage::TCOptional<NHTTP::CURL> m_AuthorIcon;

			NContainer::TCVector<CField> m_Fields;
			NStorage::TCOptional<bool> m_bFieldsMarkdown;

			NStorage::TCOptional<NHTTP::CURL> m_ImageURL;
			NStorage::TCOptional<NHTTP::CURL> m_ThumbURL;

			NStorage::TCOptional<NStr::CStr> m_Footer;
			NStorage::TCOptional<NHTTP::CURL> m_FooterIconURL;
			NStorage::TCOptional<NTime::CTime> m_FooterTimestamp;
		};

		struct CMessage
		{
			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			static NStr::CStr fs_EscapeString(NStr::CStr const &_String); // Escapes string so formatting is not applied

			NStorage::TCOptional<NStr::CStr> m_Text;
			NStorage::TCOptional<NStr::CStr> m_Channel;
			NStorage::TCOptional<NStr::CStr> m_ThreadTimestamp;
			NStorage::TCOptional<NStr::CStr> m_UserName;
			NStorage::TCOptional<NStr::CStr> m_IconEmoji;
			NStorage::TCOptional<NHTTP::CURL> m_IconURL;

			NStorage::TCOptional<bool> m_bMarkdown;
			NStorage::TCOptional<bool> m_bLinkNames;
			NStorage::TCOptional<bool> m_bReplyBroadcast;
			NStorage::TCOptional<bool> m_bUnfurlLinks;
			NStorage::TCOptional<bool> m_bUnfurlMedia;
			NStorage::TCOptional<bool> m_bFullParse;

			NContainer::TCVector<CAttachment> m_Attachments;
		};

		NConcurrency::TCFuture<NStr::CStr> f_PostMessage(NStr::CStr const &_Token, CMessage const &_Message);
		NConcurrency::TCFuture<NStr::CStr> f_UpdateMessage(NStr::CStr const &_Token, NStr::CStr const &_Timestamp, CMessage const &_Message);
		NConcurrency::TCFuture<void> f_SendMessage(NHTTP::CURL const &_IncomingWebhook, CMessage const &_Message);

		CSlackActor(NConcurrency::TCActor<CCurlActor> const &_CurlActor);
		~CSlackActor();

	private:
		struct CInternal;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif

#include "Malterlib_Web_Slack.hpp"
