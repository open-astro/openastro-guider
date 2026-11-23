/*
 *  alpaca_request_limits.cpp
 *  PHD Guiding
 *
 *  See alpaca_request_limits.h.
 */

#include "alpaca_request_limits.h"

AlpacaRequestLimitsScope::AlpacaRequestLimitsScope(CURL *handle, const AlpacaRequestLimits *limits, long clientTimeoutMs)
    : m_handle(handle), m_limits(limits), m_clientTimeoutMs(clientTimeoutMs)
{
    if (!m_handle || !m_limits)
        return;
    if (m_limits->timeoutMs > 0)
        curl_easy_setopt(m_handle, CURLOPT_TIMEOUT_MS, m_limits->timeoutMs);
    if (m_limits->abortRequested)
    {
        curl_easy_setopt(m_handle, CURLOPT_XFERINFOFUNCTION, &AlpacaRequestLimitsScope::OnProgress);
        curl_easy_setopt(m_handle, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(m_handle, CURLOPT_NOPROGRESS, 0L);
    }
}

AlpacaRequestLimitsScope::~AlpacaRequestLimitsScope()
{
    if (!m_handle || !m_limits)
        return;
    // the callback's data is this scope: take both off so no later request on the handle can call
    // into a scope that is gone
    if (m_limits->abortRequested)
    {
        curl_easy_setopt(m_handle, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(m_handle, CURLOPT_XFERINFOFUNCTION, nullptr);
        curl_easy_setopt(m_handle, CURLOPT_XFERINFODATA, nullptr);
    }
    if (m_limits->timeoutMs > 0)
        curl_easy_setopt(m_handle, CURLOPT_TIMEOUT_MS, m_clientTimeoutMs);
}

int AlpacaRequestLimitsScope::OnProgress(void *self, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    // runs inside curl_easy_perform, on the thread that called it and owns the scope
    AlpacaRequestLimitsScope *scope = static_cast<AlpacaRequestLimitsScope *>(self);
    if (scope->m_limits->abortRequested())
    {
        scope->m_aborted = true;
        return 1; // CURLE_ABORTED_BY_CALLBACK
    }
    return 0;
}
