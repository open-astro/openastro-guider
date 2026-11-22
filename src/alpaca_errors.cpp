/*
 *  alpaca_errors.cpp
 *  PHD Guiding
 *
 *  See alpaca_errors.h.
 */

#include "alpaca_errors.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "json_parser.h"

namespace AlpacaErrors
{
bool CaptureNeedsReconnect(long errorNumber)
{
    return errorNumber == kNotConnected;
}

CaptureFailureAction CaptureFailureTracker::OnFailure(long errorNumber)
{
    if (CaptureNeedsReconnect(errorNumber))
    {
        m_consecutive.store(0);
        return CaptureFailureAction::Reconnect;
    }
    if (errorNumber == kUnspecifiedError && m_consecutive.fetch_add(1) + 1 >= kAcquisitionFailuresBeforeReconnect)
    {
        m_consecutive.store(0);
        return CaptureFailureAction::Reconnect;
    }
    return CaptureFailureAction::Alert;
}

CaptureFailureAction CaptureFailureTracker::OnFailure(const CallError& error)
{
    if (error.kind == CallError::Kind::Transport && error.curlCode == kCurlCouldntConnect)
    {
        m_consecutive.store(0);
        return CaptureFailureAction::Reconnect;
    }
    return OnFailure(error.kind == CallError::Kind::Alpaca ? error.alpacaError : 0);
}

bool ReadError(const json_value *root, long *errorNumber, std::string *errorMessage)
{
    if (!root || root->type != JSON_OBJECT)
        return false;

    int number = 0;
    std::string message;
    json_for_each(n, root)
    {
        if (!n->name)
            continue;

        if (strcmp(n->name, "ErrorNumber") == 0)
        {
            if (n->type == JSON_INT)
                number = n->int_value;
            else if (n->type == JSON_FLOAT)
                number = static_cast<int>(n->float_value);
        }
        else if (strcmp(n->name, "ErrorMessage") == 0 && n->type == JSON_STRING)
        {
            message = n->string_value;
        }
    }

    if (errorNumber)
        *errorNumber = number;
    if (errorMessage)
        *errorMessage = message;

    return number != 0;
}
}

namespace
{
const char kReplacement[] = "\xEF\xBF\xBD"; // U+FFFD
const char kEllipsis[] = "...";

// Length of the valid UTF-8 sequence at s[i], or 0 when the bytes there are not one
// (bad lead byte, missing or bad continuation, overlong form, surrogate, > U+10FFFF).
std::size_t ValidSequenceLength(const std::string& s, std::size_t i)
{
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80)
        return 1;
    std::size_t len;
    unsigned char lo = 0x80, hi = 0xBF; // allowed range of the second byte
    if (c >= 0xC2 && c <= 0xDF)
        len = 2;
    else if (c >= 0xE0 && c <= 0xEF)
    {
        len = 3;
        if (c == 0xE0)
            lo = 0xA0; // overlong
        else if (c == 0xED)
            hi = 0x9F; // surrogates
    }
    else if (c >= 0xF0 && c <= 0xF4)
    {
        len = 4;
        if (c == 0xF0)
            lo = 0x90; // overlong
        else if (c == 0xF4)
            hi = 0x8F; // > U+10FFFF
    }
    else
        return 0;
    if (i + len > s.size())
        return 0;
    unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
    if (c1 < lo || c1 > hi)
        return 0;
    for (std::size_t k = 2; k < len; k++)
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
            return 0;
    return len;
}

// The longest prefix of valid UTF-8 s that is at most maxBytes long and ends on a sequence boundary.
std::string Utf8Prefix(const std::string& s, std::size_t maxBytes)
{
    if (s.size() <= maxBytes)
        return s;
    std::size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80)
        end--;
    return s.substr(0, end);
}

// s (already cleaned) cut to maxBytes with "...", trailing space before the ellipsis dropped.
std::string Cap(const std::string& s, std::size_t maxBytes)
{
    if (s.size() <= maxBytes)
        return s;
    std::size_t ellipsis = sizeof(kEllipsis) - 1;
    if (maxBytes <= ellipsis)
        return Utf8Prefix(s, maxBytes);
    std::string cut = Utf8Prefix(s, maxBytes - ellipsis);
    while (!cut.empty() && cut.back() == ' ')
        cut.pop_back();
    return cut + kEllipsis;
}
} // namespace

namespace AlpacaErrors
{
std::string OneLine(const std::string& text, std::size_t maxBytes)
{
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    std::size_t i = 0;
    while (i < text.size())
    {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7F || c == ' ')
        {
            pendingSpace = !out.empty();
            i++;
            continue;
        }
        if (pendingSpace)
        {
            out += ' ';
            pendingSpace = false;
        }
        std::size_t len = ValidSequenceLength(text, i);
        if (len == 0)
        {
            out += kReplacement;
            i++;
        }
        else
        {
            out.append(text, i, len);
            i += len;
        }
    }
    return Cap(out, maxBytes);
}

std::string CallErrorReason(const CallError& error)
{
    char buf[64];
    switch (error.kind)
    {
    case CallError::Kind::None:
        return std::string();

    case CallError::Kind::Alpaca:
    {
        // one pass, capped to leave room for the suffix (at most 30 bytes, so the cap stays above the "...")
        snprintf(buf, sizeof(buf), " (Alpaca %ld)", error.alpacaError);
        std::string suffix(buf);
        std::string message = OneLine(error.message, kReasonMaxBytes - suffix.size());
        if (message.empty())
        {
            snprintf(buf, sizeof(buf), "Alpaca error %ld", error.alpacaError);
            return buf;
        }
        return message + suffix;
    }

    case CallError::Kind::Http:
    {
        snprintf(buf, sizeof(buf), "HTTP %ld", error.httpStatus);
        std::string reason(buf);
        // judge the whole cleaned body: a capped one can come out short (trailing space or a UTF-8
        // sequence dropped before the "...") and would hide a '<' past the cut
        std::string body = OneLine(error.message, std::numeric_limits<std::size_t>::max());
        if (!body.empty() && body.size() <= kHttpBodyMaxBytes && body.find('<') == std::string::npos)
            reason += ": " + body;
        return reason;
    }

    case CallError::Kind::Transport:
    {
        std::string reason = "no response from the Alpaca server";
        std::string curlText = OneLine(error.message, kReasonMaxBytes);
        if (curlText.empty())
            return reason;
        return OneLine(reason + " (" + curlText + ")", kReasonMaxBytes);
    }

    case CallError::Kind::Parse:
        return "invalid response from the Alpaca server";
    }
    return std::string();
}

CallErrorScope::CallErrorScope(CallError *out) : m_out(out)
{
    if (m_out)
        *m_out = CallError();
}

CallErrorScope::~CallErrorScope()
{
    if (!m_out)
        return;
    if (m_succeeded)
        *m_out = CallError();
    else
    {
        if (m_error.kind == CallError::Kind::None)
            m_error.kind = CallError::Kind::Parse;
        *m_out = m_error;
    }
}

void CallErrorScope::Set(CallError::Kind kind, long httpStatus, long alpacaError, const std::string& message, long curlCode)
{
    m_error.kind = kind;
    m_error.httpStatus = httpStatus;
    m_error.alpacaError = alpacaError;
    m_error.message = message;
    m_error.curlCode = curlCode;
}
}
