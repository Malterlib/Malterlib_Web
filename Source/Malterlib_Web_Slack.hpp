// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/String/Appender>
#include <Mib/String/FormatUtils>

namespace NMib::NWeb
{
	template <typename tf_CStr>
	void CSlackActor::CAttachment::f_Format(tf_CStr &o_Str) const
	{
		typename tf_CStr::CAppender Appender(o_Str);
		NStr::TCFormatUtilities<tf_CStr> Utils(Appender);

		Utils.f_TitledValue("Fallback", m_Fallback);
		Utils.f_TitledValue("PreText", m_PreText);
		Utils.f_TitledValue("PretextMarkdown", m_bPretextMarkdown);
		Utils.f_TitledValue("Title", m_Title);
		Utils.f_TitledValue("TitleLink", m_TitleLink);
		Utils.f_TitledValue("Text", m_Text);
		Utils.f_TitledValue("TextMarkdown", m_bTextMarkdown);
		Utils.f_TitledValue("Color", m_Color);
		Utils.f_TitledValue("AuthorName", m_AuthorName);
		Utils.f_TitledValue("AuthorLink", m_AuthorLink);
		Utils.f_TitledValue("AuthorIcon", m_AuthorIcon);
		Utils.f_TitledValue("Fields", m_Fields);
		Utils.f_TitledValue("FieldsMarkdown", m_bFieldsMarkdown);
		Utils.f_TitledValue("ImageURL", m_ImageURL);
		Utils.f_TitledValue("ThumbURL", m_ThumbURL);
		Utils.f_TitledValue("Footer", m_Footer);
		Utils.f_TitledValue("FooterIconURL", m_FooterIconURL);
		Utils.f_TitledValue("FooterTimestamp", m_FooterTimestamp);
	}

	template <typename tf_CStr>
	void CSlackActor::CMessage::f_Format(tf_CStr &o_Str) const
	{
		typename tf_CStr::CAppender Appender(o_Str);
		NStr::TCFormatUtilities<tf_CStr> Utils(Appender);

		Utils.f_TitledValue("Text", m_Text);
		Utils.f_TitledValue("Channel", m_Channel);
		Utils.f_TitledValue("UserName", m_UserName);
		Utils.f_TitledValue("IconEmoji", m_IconEmoji);
		Utils.f_TitledValue("IconURL", m_IconURL);
		Utils.f_TitledValue("Markdown", m_bMarkdown);
		Utils.f_TitledValue("LinkNames", m_bLinkNames);
		Utils.f_TitledValue("ReplyBroadcast", m_bReplyBroadcast);
		Utils.f_TitledValue("UnfurlLinks", m_bUnfurlLinks);
		Utils.f_TitledValue("UnfurlMedia", m_bUnfurlMedia);
		Utils.f_TitledValue("FullParse", m_bFullParse);
		Utils.f_TitledValue("ThreadTimestamp", m_ThreadTimestamp);
		Utils.f_TitledValue("Attachments", m_Attachments);
	}

	template <typename tf_CStr>
	void CSlackActor::CField::f_Format(tf_CStr &o_Str) const
	{
		typename tf_CStr::CAppender Appender(o_Str);
		NStr::TCFormatUtilities<tf_CStr> Utils(Appender);

		Utils.f_TitledValue("Title", m_Title);
		Utils.f_TitledValue("Value", m_Value);
		Utils.f_TitledValue("Short", m_bShort);
	}

	constexpr CSlackActor::CPredefinedColor::CPredefinedColor(EPredefinedColor _Color)
		: m_Color(_Color)
	{
	}

	template <typename tf_CStr>
	void CSlackActor::CPredefinedColor::f_Format(tf_CStr &o_Str) const
	{
		switch (m_Color)
		{
		case EPredefinedColor_Good: o_Str += "Predef: Good"; break;
		case EPredefinedColor_Warning: o_Str += "Predef: Warning"; break;
		case EPredefinedColor_Danger: o_Str += "Predef: Danger"; break;
		}
	}

	template <typename tf_CStr>
	void CSlackActor::CRgbColor::f_Format(tf_CStr &o_Str) const
	{
		o_Str += typename tf_CStr::CFormat("#{nfh,sf0,sj2}{nfh,sf0,sj2}{nfh,sf0,sj2}") << m_Red << m_Green << m_Blue;
	}
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
