/*
 *  alpaca_request_limits.h
 *  PHD Guiding
 *
 *  One AlpacaClient request's own limits (#294): a timeout other than the
 *  client's, and an abort check that curl polls while the request is in
 *  flight, so a stop does not wait for a request the server is holding.
 *  Kept free of wx so it can be unit tested against a real curl handle
 *  (tests/alpaca/test_alpaca_request_limits.cpp).
 */

#ifndef ALPACA_REQUEST_LIMITS_H_INCLUDED
#define ALPACA_REQUEST_LIMITS_H_INCLUDED

#include <curl/curl.h>

struct AlpacaRequestLimits
{
    // the whole request's timeout, ms; 0 keeps the client's
    long timeoutMs = 0;
    // Polled on the thread running the request, by curl's progress callback: often while data
    // moves and about once a second while the request waits on the server. True gives the request
    // up (CURLE_ABORTED_BY_CALLBACK). nullptr: never. A plain function, so there is nothing whose
    // lifetime could end while curl still holds it.
    bool (*abortRequested)() = nullptr;
};

// Puts limits on handle for one curl_easy_perform and takes them off again in the destructor:
// the client's timeout (clientTimeoutMs) back, the progress callback off. Construct it after the
// handle's other options are set and destroy it before the handle is used again; the handle must
// only be used by the thread that owns the scope (AlpacaClient holds its mutex). limits may be
// nullptr: then it changes nothing.
class AlpacaRequestLimitsScope
{
public:
    AlpacaRequestLimitsScope(CURL *handle, const AlpacaRequestLimits *limits, long clientTimeoutMs);
    ~AlpacaRequestLimitsScope();
    AlpacaRequestLimitsScope(const AlpacaRequestLimitsScope&) = delete;
    AlpacaRequestLimitsScope& operator=(const AlpacaRequestLimitsScope&) = delete;

    // whether the abort check gave the request up
    bool Aborted() const { return m_aborted; }

private:
    static int OnProgress(void *self, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

    CURL *m_handle;
    const AlpacaRequestLimits *m_limits;
    long m_clientTimeoutMs;
    bool m_aborted = false;
};

#endif
