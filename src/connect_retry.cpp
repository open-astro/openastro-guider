/*
 *  connect_retry.cpp
 *  PHD Guiding
 *
 *  See connect_retry.h.
 */

#include "connect_retry.h"

#include <algorithm>
#include <chrono>

namespace ConnectRetry
{
// these texts are internal to the event server and are not translated
const char *const kNotConfiguredReason = "the Alpaca server is not configured; set its host and port first";

int RetryDelaySeconds(int attempt)
{
    if (attempt <= 1)
        return kFirstRetryDelaySec;
    if (attempt == 2)
        return kSecondRetryDelaySec;
    return kMaxRetryDelaySec;
}

int Tracker::Index(GearDevice d)
{
    switch (d)
    {
    case GearDevice::Mount:
        return 1;
    case GearDevice::Rotator:
        return 2;
    default:
        return 0;
    }
}

void Tracker::OnConnectRequested(GearDevice d)
{
    if (d == GearDevice::All)
    {
        for (State& s : m_state)
            s.intent = Intent::Wanted;
        return;
    }
    At(d).intent = Intent::Wanted;
}

bool Tracker::OnConnectResult(GearDevice d, bool connected, int64_t nowMs, const std::string& reason, bool retryable)
{
    State& s = At(d);
    s.results++;
    s.generation++;
    s.inFlight = false;
    s.intent = Intent::Wanted;
    if (connected)
    {
        bool wasFailed = s.failed;
        s.failed = false;
        s.parked = false;
        s.attempts = 0;
        s.nextDueMs = 0;
        s.reason.clear();
        return wasFailed;
    }
    s.failed = true;
    s.attempts++;
    s.parked = !retryable;
    s.reason = reason;
    s.nextDueMs = nowMs + static_cast<int64_t>(RetryDelaySeconds(s.attempts)) * 1000;
    return false;
}

void Tracker::OnUserDisconnect(GearDevice d)
{
    if (d == GearDevice::All)
    {
        OnUserDisconnect(GearDevice::Camera);
        OnUserDisconnect(GearDevice::Mount);
        OnUserDisconnect(GearDevice::Rotator);
        return;
    }
    State& s = At(d);
    s.intent = Intent::UserDisconnected;
    s.generation++;
    s.inFlight = false;
}

void Tracker::Forget(GearDevice d)
{
    if (d == GearDevice::All)
    {
        Forget(GearDevice::Camera);
        Forget(GearDevice::Mount);
        Forget(GearDevice::Rotator);
        return;
    }
    State& s = At(d);
    State fresh;
    fresh.generation = s.generation + 1;
    fresh.results = s.results;
    s = fresh;
}

Tracker::LeafOutcome Tracker::OnLeafResult(GearDevice d, bool connected, bool autoReconnecting, int64_t nowMs,
                                           const std::string& reason, bool retryable)
{
    LeafOutcome out;
    if (autoReconnecting && !connected)
        return out;
    bool recovered = OnConnectResult(d, connected, nowMs, reason, retryable);
    out.reconnected = recovered && !autoReconnecting;
    out.failed = !connected;
    return out;
}

bool Tracker::OnConnectCallEnded(GearDevice d, uint64_t results, bool connected, int64_t nowMs, const std::string& reason)
{
    if (connected || At(d).results != results)
        return false;
    OnConnectResult(d, false, nowMs, reason);
    return true;
}

void Tracker::OnGuideOutOfTime(GearDevice d, int64_t nowMs, bool dueNow)
{
    State& s = At(d);
    if (s.intent == Intent::UserDisconnected)
        return;
    if (Retrying(d))
    {
        if (dueNow && s.nextDueMs > nowMs)
            s.nextDueMs = nowMs;
        return;
    }
    s.intent = Intent::Wanted;
    s.failed = true;
    s.parked = false;
    s.nextDueMs = nowMs;
}

bool Tracker::Chosen(GearDevice d, const std::string& choice)
{
    std::string& last = m_choice[Index(d)];
    if (last == choice)
        return false;
    last = choice;
    return true;
}

Tracker::ServerChangeCarry Tracker::BeforeServerChange() const
{
    ServerChangeCarry carry;
    for (int i = 0; i < 3; i++)
    {
        const State& s = m_state[i];
        carry.wanted[i] = s.intent == Intent::Wanted && s.failed;
        carry.reason[i] = s.reason;
    }
    return carry;
}

void Tracker::AfterServerChange(const ServerChangeCarry& carry, int64_t nowMs)
{
    for (int i = 0; i < 3; i++)
    {
        State& s = m_state[i];
        // Defensive: set_alpaca_server runs GearDialog's AlpacaServerChanging/Changed (BeforeServerChange,
        // then this) back to back with no user disconnect in between, so this cannot fire there today. Kept
        // for the header's contract, which a unit test pins: a device the user disconnected since the
        // snapshot is not wanted again (#110).
        if (!carry.wanted[i] || s.intent == Intent::UserDisconnected)
            continue;
        s.intent = Intent::Wanted;
        s.failed = true;
        s.parked = false;
        s.inFlight = false;
        s.attempts = 0;
        s.nextDueMs = nowMs;
        s.generation++;
        if (s.reason.empty())
            s.reason = carry.reason[i];
    }
}

uint64_t Tracker::BeginAttempt(GearDevice d)
{
    State& s = At(d);
    s.inFlight = true;
    return ++s.generation;
}

bool Tracker::AttemptCurrent(GearDevice d, uint64_t token) const
{
    const State& s = At(d);
    return s.inFlight && s.generation == token;
}

void Tracker::EndAttempt(GearDevice d)
{
    At(d).inFlight = false;
}

bool Tracker::AttemptInFlight(GearDevice d) const
{
    return At(d).inFlight;
}

bool Tracker::IsFailed(GearDevice d) const
{
    return At(d).failed;
}

bool Tracker::IsParked(GearDevice d) const
{
    return At(d).parked;
}

bool Tracker::UserDisconnected(GearDevice d) const
{
    return At(d).intent == Intent::UserDisconnected;
}

Intent Tracker::GetIntent(GearDevice d) const
{
    return At(d).intent;
}

bool Tracker::Retrying(GearDevice d) const
{
    const State& s = At(d);
    return s.failed && s.intent == Intent::Wanted && !s.parked;
}

bool Tracker::AnyRetrying() const
{
    return Retrying(GearDevice::Camera) || Retrying(GearDevice::Mount) || Retrying(GearDevice::Rotator);
}

bool Tracker::Due(GearDevice d, int64_t nowMs) const
{
    return Retrying(d) && !At(d).inFlight && nowMs >= At(d).nextDueMs;
}

int64_t Tracker::NextDueMs(GearDevice d) const
{
    return At(d).nextDueMs;
}

int Tracker::Attempts(GearDevice d) const
{
    return At(d).attempts;
}

uint64_t Tracker::Results(GearDevice d) const
{
    return At(d).results;
}

const std::string& Tracker::LastReason(GearDevice d) const
{
    return At(d).reason;
}

bool AlignmentToolBlocks(const AlignmentTools& tools)
{
    return tools.polarDriftDrifting || tools.driftToolOpen;
}

std::string RetryBlocker(GearDevice d, const Gates& g)
{
    if (g.shuttingDown)
        return "shutting down";
    if (g.darkBuild)
        return "dark-frame capture in progress";
    if (g.paSession)
        return "polar-alignment session in progress";
    if (g.alignmentTool)
        return "polar or drift alignment in progress";
    if (g.guidingAssistant)
        return "Guiding Assistant is running";
    if (g.connectInProgress)
        return "connect in progress";
    return ConnectRefusal(d, true, g.captureActive, g.calibratingOrGuiding, g.dialogOpen);
}

GuideStep PlanGuideConnect(bool selected, bool connected, Intent intent, bool refused, ProbeKind kind, bool probeInFlight)
{
    if (!selected || connected)
        return GuideStep::Nothing;
    if (intent == Intent::UserDisconnected)
        return GuideStep::UserDisconnected;
    if (refused)
        return GuideStep::Refused;
    if (probeInFlight)
        return GuideStep::WaitForProbe;
    switch (kind)
    {
    case ProbeKind::NotConfigured:
        return GuideStep::NotConfigured;
    case ProbeKind::Alpaca:
        return GuideStep::ProbeThenConnect;
    case ProbeKind::NotAlpaca:
        break;
    }
    return GuideStep::Connect;
}

} // namespace ConnectRetry

namespace
{
std::string CallReason(const AlpacaErrors::CallError& error, const char *deviceName)
{
    std::string reason = AlpacaErrors::CallErrorReason(error);
    if (reason.empty())
        reason = std::string("the ") + deviceName + " did not answer";
    return reason;
}

const char kCancelled[] = "cancelled";

ConnectRetry::ConnectError s_connectErrors[3];
int s_quietDepth = 0;

ConnectRetry::ConnectError& ErrorSlot(GearDevice d)
{
    switch (d)
    {
    case GearDevice::Mount:
        return s_connectErrors[1];
    case GearDevice::Rotator:
        return s_connectErrors[2];
    default:
        return s_connectErrors[0];
    }
}
} // namespace

namespace ConnectRetry
{

const char *const kOutOfTimeReason = "no response from the Alpaca server (Timeout was reached)";

ProbeResult RunProbe(ProbeOps& ops, const char *deviceName, int connectWaitMs, int pollIntervalMs, int64_t deadlineMs)
{
    ProbeResult result;
    // the deadline's share for the next call; false when nothing is left
    auto budget = [&]() -> bool
    {
        if (deadlineMs == kNoDeadline)
            return true;
        int64_t left = deadlineMs - ops.NowMs();
        if (left <= 0)
            return false;
        ops.SetCallBudgetMs(left);
        return true;
    };
    auto cancelled = [&]() -> bool
    {
        if (!ops.Cancelled())
            return false;
        result.cancelled = true;
        result.reason = kCancelled;
        return true;
    };

    if (cancelled())
        return result;
    if (!budget())
    {
        result.reason = kOutOfTimeReason;
        result.outOfTime = true;
        return result;
    }

    bool connected = false;
    AlpacaErrors::CallError error;
    if (!ops.GetConnected(&connected, &error))
    {
        result.reason = CallReason(error, deviceName);
        return result;
    }
    if (connected)
    {
        result.connected = true;
        return result;
    }

    if (cancelled())
        return result;
    if (!budget())
    {
        result.reason = kOutOfTimeReason;
        result.outOfTime = true;
        return result;
    }
    error = AlpacaErrors::CallError();
    if (!ops.PutConnected(&error))
    {
        result.reason = CallReason(error, deviceName);
        return result;
    }

    // a driver may accept the PUT and connect in the background: wait, a bounded time, until the server says so
    int64_t start = ops.NowMs();
    int64_t deadline = start + connectWaitMs;
    bool cutShort = deadlineMs != kNoDeadline && deadlineMs < deadline;
    if (cutShort)
        deadline = deadlineMs;
    std::string lastError;
    for (;;)
    {
        if (cancelled())
            return result;
        if (!budget())
            break;
        connected = false;
        error = AlpacaErrors::CallError();
        if (ops.GetConnected(&connected, &error))
        {
            if (connected)
            {
                result.connected = true;
                return result;
            }
            lastError.clear();
        }
        else
            lastError = CallReason(error, deviceName);
        int64_t now = ops.NowMs();
        if (now >= deadline)
            break;
        ops.SleepMs((int) std::min<int64_t>(pollIntervalMs, deadline - now));
    }

    if (cutShort)
        result.reason = std::string("the ") + deviceName + " did not report connected in time";
    else
    {
        int seconds = (connectWaitMs + 999) / 1000;
        result.reason = std::string("the ") + deviceName + " did not report connected within " + std::to_string(seconds) + " s";
    }
    if (!lastError.empty())
        result.reason += ": " + lastError;
    return result;
}

void ProbeHandoff::Begin()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_finished = false;
    m_taken = false;
    m_result = ProbeResult();
}

void ProbeHandoff::Finish(const ProbeResult& result)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result = result;
        m_finished = true;
    }
    m_cv.notify_all();
}

bool ProbeHandoff::WaitAndTake(int64_t timeoutMs, ProbeResult *result)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (timeoutMs > 0)
        m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return m_finished; });
    if (!m_finished || m_taken)
        return false;
    m_taken = true;
    *result = m_result;
    return true;
}

bool ProbeHandoff::TakeDelivered(ProbeResult *result)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_finished || m_taken)
        return false;
    m_taken = true;
    *result = m_result;
    return true;
}

std::string WithReason(const std::string& text, const std::string& reason)
{
    if (reason.empty())
        return text;
    return text + " (" + reason + ")";
}

std::string GuideRefusal(const std::vector<GuideFailure>& failures)
{
    std::string text = "all equipment must be connected first";
    const char *sep = ": ";
    for (const GuideFailure& f : failures)
    {
        std::string clause = std::string("the ") + GearDeviceName(f.device);
        switch (f.kind)
        {
        case GuideFailure::Kind::ConnectFailed:
            clause += " failed to connect";
            if (!f.reason.empty())
                clause += ": " + f.reason;
            break;
        case GuideFailure::Kind::UserDisconnected:
            clause += " was disconnected by the user";
            break;
        case GuideFailure::Kind::Refused:
            clause += " is not connected";
            if (!f.reason.empty())
                clause += ": " + f.reason;
            break;
        case GuideFailure::Kind::StillConnecting:
            clause += " is still connecting";
            break;
        case GuideFailure::Kind::OutOfTime:
            clause += " was not tried in time";
            break;
        }
        text += sep + clause;
        sep = "; ";
    }
    return text;
}

void RecordConnectError(GearDevice d, const std::string& reason, bool retryable)
{
    ConnectError& e = ErrorSlot(d);
    e.reason = reason;
    e.retryable = retryable;
}

void RecordConnectErrorIfNone(GearDevice d, const std::string& reason)
{
    if (ErrorSlot(d).reason.empty())
        RecordConnectError(d, reason);
}

void ClearConnectError(GearDevice d)
{
    ErrorSlot(d) = ConnectError();
}

ConnectError TakeConnectError(GearDevice d)
{
    ConnectError e = ErrorSlot(d);
    ErrorSlot(d) = ConnectError();
    return e;
}

QuietConnectScope::QuietConnectScope()
{
    ++s_quietDepth;
}

QuietConnectScope::~QuietConnectScope()
{
    --s_quietDepth;
}

bool QuietConnect()
{
    return s_quietDepth > 0;
}
} // namespace ConnectRetry
