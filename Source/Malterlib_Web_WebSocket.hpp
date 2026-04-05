// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NWeb
{
	template <typename tf_CResultCall>
	NConcurrency::TCActor<CWebSocketActor> CWebSocketNewClientConnection::f_Accept(tf_CResultCall &&_CallbackResultCall)
	{
		if (!mp_pHelper->m_bRepliedToConnection.f_Exchange(true))
		{
			this->mp_Connection(&CWebSocketActor::fp_AcceptClientConnection, fg_Move(static_cast<CWebSocketActor::CCallbacks &>(*this)))
				> fg_Forward<tf_CResultCall>(_CallbackResultCall)
			;
		}
		return fg_Move(mp_Connection);
	}

	template <typename tf_CResultCall>
	NConcurrency::TCActor<CWebSocketActor> CWebSocketNewServerConnection::f_Accept
		(
			NStr::CStr const &_Protocol
			, tf_CResultCall &&_CallbackResultCall
			, NHTTP::CResponseHeader &&_ResponseHeader
		)
	{
		if (!mp_pHelper->m_bRepliedToConnection.f_Exchange(true))
		{
			mp_Connection(&CWebSocketActor::fp_AcceptServerConnection, _Protocol, fg_Move(_ResponseHeader), fg_Move(static_cast<CWebSocketActor::CCallbacks &>(*this)))
				> fg_Forward<tf_CResultCall>(_CallbackResultCall)
			;
		}
		return fg_Move(mp_Connection);
	}
}
