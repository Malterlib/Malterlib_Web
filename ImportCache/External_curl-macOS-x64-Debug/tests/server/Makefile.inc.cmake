#***************************************************************************
#                                  _   _ ____  _
#  Project                     ___| | | |  _ \| |
#                             / __| | | | |_) | |
#                            | (__| |_| |  _ <| |___
#                             \___|\___/|_| \_\_____|
#
# Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution. The terms
# are also available at https://curl.se/docs/copyright.html.
#
# You may opt to use, copy, modify, merge, publish, distribute and/or sell
# copies of the Software, and permit persons to whom the Software is
# furnished to do so, under the terms of the COPYING file.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
# SPDX-License-Identifier: curl
#
###########################################################################

set(noinst_PROGRAMS getpart resolve rtspd sockfilt sws tftpd    
 socksd disabled mqttd)

set(CURLX_SRCS 
 ../../lib/mprintf.c 
 ../../lib/nonblock.c 
 ../../lib/strtoofft.c 
 ../../lib/warnless.c 
 ../../lib/timediff.c 
 ../../lib/dynbuf.c 
 ../../lib/strdup.c 
 ../../lib/strcase.c 
 ../../lib/curl_multibyte.c)

set(CURLX_HDRS 
 ../../lib/curlx.h 
 ../../lib/nonblock.h 
 ../../lib/strtoofft.h 
 ../../lib/warnless.h 
 ../../lib/timediff.h 
 ../../lib/curl_ctype.h 
 ../../lib/dynbuf.h 
 ../../lib/strdup.h 
 ../../lib/curl_multibyte.h)

set(USEFUL 
 getpart.c 
 getpart.h 
 server_setup.h 
 ../../lib/base64.c 
 ../../lib/curl_base64.h 
 ../../lib/memdebug.c 
 ../../lib/memdebug.h)

set(UTIL 
 util.c 
 util.h)

set(getpart_SOURCES ${CURLX_SRCS} ${CURLX_HDRS} ${USEFUL} 
 testpart.c)
set(getpart_LDADD ${CURL_NETWORK_AND_TIME_LIBS})
set(getpart_CFLAGS ${AM_CFLAGS})

set(resolve_SOURCES ${CURLX_SRCS} ${CURLX_HDRS} ${USEFUL} ${UTIL} 
 resolve.c)
set(resolve_LDADD ${CURL_NETWORK_AND_TIME_LIBS})
set(resolve_CFLAGS ${AM_CFLAGS})

set(rtspd_SOURCES ${CURLX_SRCS} ${CURLX_HDRS} ${USEFUL} ${UTIL} 
 server_sockaddr.h 
 rtspd.c)
set(rtspd_LDADD ${CURL_NETWORK_AND_TIME_LIBS})
set(rtspd_CFLAGS ${AM_CFLAGS})

set(sockfilt_SOURCES ${CURLX_SRCS} ${CURLX_HDRS} ${USEFUL} ${UTIL} 
 server_sockaddr.h 
 sockfilt.c 
 ../../lib/inet_pton.c)
set(sockfilt_LDADD ${CURL_NETWORK_AND_TIME_LIBS})
set(sockfilt_CFLAGS ${AM_CFLAGS})

set(socksd_SOURCES ${CURLX_SRCS} ${CURLX_HDRS} ${USEFUL} ${UTIL} 
 server_sockaddr.h socksd.c 
 ../../lib/inet_pton.c)
set(socksd_LDADD ${CURL_NETWORK_AND_TIME_LIBS})
set(socksd_CFLAGS ${AM_CFLAGS})

set(mqttd_SOURCES ${CURLX_SRCS} ${CURLX_HDRS} ${USEFUL} ${UTIL} 
 server_sockaddr.h mqttd.c 
 ../../lib/inet_pton.c)
set(mqttd_LDADD ${CURL_NETWORK_AND_TIME_LIBS})
set(mqttd_CFLAGS ${AM_CFLAGS})

set(sws_SOURCES ${CURLX_SRCS} ${CURLX_HDRS} ${USEFUL} ${UTIL} 
 server_sockaddr.h 
 sws.c 
 ../../lib/inet_pton.c)
set(sws_LDADD ${CURL_NETWORK_AND_TIME_LIBS})
set(sws_CFLAGS ${AM_CFLAGS})

set(tftpd_SOURCES ${CURLX_SRCS} ${CURLX_HDRS} ${USEFUL} ${UTIL} 
 server_sockaddr.h 
 tftpd.c 
 tftp.h)
set(tftpd_LDADD ${CURL_NETWORK_AND_TIME_LIBS})
set(tftpd_CFLAGS ${AM_CFLAGS})

set(disabled_SOURCES disabled.c)
