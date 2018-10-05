// Copyright (c) Microsoft. All rights reserved.

#pragma once
#include <IHttpClient.hpp>
#include "pal/PAL.hpp"

namespace ARIASDK_NS_BEGIN {


#ifndef _WININET_
typedef void* HINTERNET;
#endif

class WinInetRequestWrapper;

class HttpClient_WinInet : public IHttpClient {
  public:
    HttpClient_WinInet();
    virtual ~HttpClient_WinInet();
    virtual IHttpRequest* CreateRequest() override;
    virtual void SendRequestAsync(IHttpRequest* request, IHttpResponseCallback* callback) override;
    virtual void CancelRequestAsync(std::string const& id) override;
    virtual void CancelAllRequests() override;

  protected:
    void signalDoneAndErase(std::string const& id);

  protected:
    HINTERNET                                                        m_hInternet;
    std::mutex                                                       m_requestsMutex;
    std::map<std::string, WinInetRequestWrapper*>                    m_requests;
    static unsigned                                                  s_nextRequestId;

    friend class WinInetRequestWrapper;
};


} ARIASDK_NS_END
