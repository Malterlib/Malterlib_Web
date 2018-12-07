// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NWeb
{
	template <typename tf_CResultCall>
	NConcurrency::TCActor<CWebSocketActor> CWebSocketNewClientConnection::f_Accept(tf_CResultCall &&_CallbackResultCall)
	{
		mp_Connection
			(
				&CWebSocketActor::fp_SetCallbacks
				, _CallbackResultCall.mp_Actor
				, fg_Move(m_fOnReceiveBinaryMessage)
				, fg_Move(m_fOnReceiveTextMessage)
				, fg_Move(m_fOnReceivePing)
				, fg_Move(m_fOnReceivePong)
				, fg_Move(m_fOnClose)
			)
			> fg_Forward<tf_CResultCall>(_CallbackResultCall)
		;
		if (!mp_pHelper->m_bRepliedToConnection.f_Exchange(true))
		{
			this->mp_Connection(&CWebSocketActor::fp_AcceptClientConnection)
				> NConcurrency::fg_DiscardResult()
			;
		}
		return fg_Move(mp_Connection);
	}

	template <typename tf_CResultCall>
	NConcurrency::TCActor<CWebSocketActor> CWebSocketNewServerConnection::f_Accept(NStr::CStr const &_Protocol, tf_CResultCall &&_CallbackResultCall, NHTTP::CResponseHeader &&_ResponseHeader)
	{
		mp_Connection
			(
				&CWebSocketActor::fp_SetCallbacks
				, _CallbackResultCall.mp_Actor
				, fg_Move(m_fOnReceiveBinaryMessage)
				, fg_Move(m_fOnReceiveTextMessage)
				, fg_Move(m_fOnReceivePing)
				, fg_Move(m_fOnReceivePong)
				, fg_Move(m_fOnClose)
			)
			> fg_Forward<tf_CResultCall>(_CallbackResultCall)
		;
		if (!mp_pHelper->m_bRepliedToConnection.f_Exchange(true))
		{
			mp_Connection(&CWebSocketActor::fp_AcceptServerConnection, _Protocol, fg_Move(_ResponseHeader))
				> NConcurrency::fg_DiscardResult()
			;
		}
		return fg_Move(mp_Connection);
	}
}
