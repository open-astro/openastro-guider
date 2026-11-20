/*
 *  connect_retry.h
 *  PHD Guiding
 *
 *  Recovery of a selected camera, mount or rotator that failed to connect (#104):
 *  the slow background retry's schedule and per-device state, when a retry has to
 *  wait, the Alpaca reachability probe that runs before a connect, and the texts
 *  clients get. Kept free of wx so it can be unit tested
 *  (tests/equipment/test_connect_retry.cpp); GearDialog drives it.
 */

#ifndef CONNECT_RETRY_H_INCLUDED
#define CONNECT_RETRY_H_INCLUDED

#include "alpaca_errors.h"
#include "connect_request.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ConnectRetry
{
// The background retry: 15 s, then 30 s, then 60 s after each failure, and every 60 s from then on, with no limit
// on the number of attempts.
const int kFirstRetryDelaySec = 15;
const int kSecondRetryDelaySec = 30;
const int kMaxRetryDelaySec = 60;
// How often GearDialog's timer checks for a due device. A check is cheap; the attempt itself runs on a thread.
const int kTimerTickMs = 1000;

// The probe's own AlpacaClient timeouts, much shorter than the drivers' 10 s connect / 30 s total.
const long kProbeConnectTimeoutMs = 1500;
const long kProbeRequestTimeoutMs = 3000;
// The background retry's Connected=true PUT gets longer: on a server where it is synchronous (Alpaca interface v1-v3,
// ASCOM Remote, a bridge opening a serial mount or a USB camera) a connect that succeeds can take several seconds.
// It runs on the probe thread, so it holds nothing up. Still well below the drivers' 30 s.
const long kProbePutTimeoutMs = 10000;
// The guide RPC's connect-first (GearDialog::ConnectForGuide) runs on the main thread, which serves no other RPC or
// event meanwhile, so all of it, camera and mount together, gets this much: every GET, PUT and poll gets what is left,
// whatever its own timeout. A device that has not connected by then is refused and left to the background retry.
// It stays below the 5 s per-call timeout of Ara's liveness ping.
const int kGuideConnectBudgetMs = 4000;
// A device the server reports connected is then connected through its driver (a GET connected and the property
// reads), which the budget does not bound. The guide RPC starts that only with at least this much of the budget left;
// with less, it refuses the device as still connecting and the background retry connects it at once.
const int kGuideDriverConnectMarginMs = 1000;
// No deadline: each call has its own timeout only (the background retry).
const int64_t kNoDeadline = -1;
// After the probe's Connected=true PUT is accepted, how long it waits for the server to report the device connected
// (a camera driver may connect in the background), and how often it asks.
const int kProbeConnectWaitMs = 5000;
const int kProbePollIntervalMs = 250;

// Seconds from the attempt-th consecutive failure (1-based) to the next attempt: 15, 30, 60, 60, ...
int RetryDelaySeconds(int attempt);

// What the user asked for a device. UserDisconnected (set_connected false, DisconnectAll, a Gear dialog Disconnect
// button) is never retried and the guide RPC does not connect it.
enum class Intent
{
    None,
    Wanted,
    UserDisconnected,
};

// Per-device retry state for the camera, mount and rotator. Main thread only: every call comes from GearDialog,
// the RPC handlers or the timer, and the probe thread only hands its result back through CallAfter.
class Tracker
{
public:
    // The guider was asked to connect d (set_connected true, ConnectAll, the guide RPC): the intent is Wanted again.
    // All = every device.
    void OnConnectRequested(GearDevice d);

    // The result of a connect the guider made for a selected device. On a failure the device is Wanted and failed,
    // and the next attempt is due RetryDelaySeconds(attempts) from nowMs; retryable == false ("not configured") parks
    // it until Forget or AfterServerChange instead. Returns true when the device was failed and is now connected: the one time
    // EquipmentReconnected is due for this failure.
    bool OnConnectResult(GearDevice d, bool connected, int64_t nowMs, const std::string& reason, bool retryable = true);

    // The user disconnected d (All = every device) and the disconnect was accepted: no more retries. A device that
    // had failed stays failed, so connecting it later still reports EquipmentReconnected once.
    void OnUserDisconnect(GearDevice d);

    // What a connect leaf (DoConnectCamera, OnButtonConnectScope, OnButtonConnectRotator) reports. reconnected: the
    // one EquipmentReconnected this recovery is due. failed: the device failed and the retry has work.
    struct LeafOutcome
    {
        bool reconnected = false;
        bool failed = false;
    };
    // A connect leaf ended with the device's final state. The camera auto-reconnect (#105, autoReconnecting) sends
    // its own EquipmentReconnected, so a success there is recorded without one; its failure is not recorded here at
    // all, but once DoTryReconnect has sent EquipmentReconnectFailed (OnConnectResult through
    // GearDialog::NoteCameraReconnectFailed).
    LeafOutcome OnLeafResult(GearDevice d, bool connected, bool autoReconnecting, int64_t nowMs, const std::string& reason,
                             bool retryable = true);

    // After the retry or the guide RPC called SetDeviceConnected for d (results was Results(d) just before): when d
    // is still not connected and no connect leaf recorded a result (SetDeviceConnected returned before a connect, a
    // refusal the gates missed), records the failure with reason, so the schedule still moves on. Returns true when
    // it recorded one: reason is then the failure's reason, not the driver's.
    bool OnConnectCallEnded(GearDevice d, uint64_t results, bool connected, int64_t nowMs, const std::string& reason);

    // The guide RPC's budget ran out for d before it had a result of its own: no request was sent for d
    // (ProbeResult::outOfTime), or the server reports d connected but too little time is left for the driver's
    // connect. No failure is counted and the last reason stays. A device that is retrying keeps its schedule, unless
    // dueNow brings its next attempt forward to nowMs; any other device (not the user's disconnect) becomes Wanted
    // and failed, due at nowMs, so the background retry takes it over.
    void OnGuideOutOfTime(GearDevice d, int64_t nowMs, bool dueNow);

    // The selection or the profile changed: d is a new device and its old state no longer applies.
    void Forget(GearDevice d);

    // A device was chosen (the Gear dialog's choice handlers, which also run when the same device is chosen again:
    // set_selected_*, the re-selection set_alpaca_server makes). Returns true when choice differs from the last one,
    // and the caller must then Forget d; choosing the same device again keeps its state and its retries.
    bool Chosen(GearDevice d, const std::string& choice);

    // set_alpaca_server re-selects each Alpaca device for the new server, which forgets it. What to carry over the
    // change: the devices still wanted after a failure (retrying, or parked as not configured).
    struct ServerChangeCarry
    {
        bool wanted[3] = { false, false, false };
        std::string reason[3];
    };
    ServerChangeCarry BeforeServerChange() const;
    // The server settings changed: every carried device that the user has not disconnected since is wanted again,
    // still failed (so its connect reports EquipmentReconnected once), on a fresh schedule, and due at nowMs.
    void AfterServerChange(const ServerChangeCarry& carry, int64_t nowMs);

    // A background attempt (the probe) for d starts. The returned token identifies it: AttemptCurrent(d, token) is
    // false once anything has changed the device's state since (a connect result, a user disconnect, Forget), and the
    // probe's result must then be dropped.
    uint64_t BeginAttempt(GearDevice d);
    bool AttemptCurrent(GearDevice d, uint64_t token) const;
    // The attempt ended without a connect result (a gate closed while the probe ran): still due, no new delay.
    void EndAttempt(GearDevice d);
    bool AttemptInFlight(GearDevice d) const;

    bool IsFailed(GearDevice d) const;
    bool IsParked(GearDevice d) const;
    bool UserDisconnected(GearDevice d) const;
    Intent GetIntent(GearDevice d) const;
    // failed, Wanted and not parked: the timer has work for d now or later
    bool Retrying(GearDevice d) const;
    bool AnyRetrying() const;
    bool Due(GearDevice d, int64_t nowMs) const;
    int64_t NextDueMs(GearDevice d) const;
    // consecutive failures since the last success (0 when connected or never tried)
    int Attempts(GearDevice d) const;
    // how many results OnConnectResult has recorded for d, ever; lets a caller tell whether a connect path reached a
    // connect at all
    uint64_t Results(GearDevice d) const;
    const std::string& LastReason(GearDevice d) const;

private:
    struct State
    {
        Intent intent = Intent::None;
        bool failed = false;
        bool parked = false;
        bool inFlight = false;
        int attempts = 0;
        int64_t nextDueMs = 0;
        uint64_t generation = 0;
        uint64_t results = 0;
        std::string reason;
    };
    State m_state[3];
    std::string m_choice[3]; // the choice each state belongs to (Chosen); kept by Forget

    static int Index(GearDevice d);
    State& At(GearDevice d) { return m_state[Index(d)]; }
    const State& At(GearDevice d) const { return m_state[Index(d)]; }
};

// What can keep a background attempt from running now.
struct Gates
{
    bool shuttingDown = false;
    bool darkBuild = false; // a dark-library / defect-map build is capturing (#100)
    bool paSession = false; // a set_pa_session lease is held
    bool guidingAssistant = false;
    bool connectInProgress = false; // a connect or disconnect call is on the stack (re-entrant event loop)
    bool captureActive = false;
    bool calibratingOrGuiding = false;
    bool dialogOpen = false; // the Gear dialog is modal
};

// Why a background attempt for d must wait, or "" when it can run. Past the gates of its own, the ConnectRefusal
// rules of set_connected apply: the camera waits while capture is active, the mount and rotator while calibrating or
// guiding.
std::string RetryBlocker(GearDevice d, const Gates& gates);

// The probe: GET <type>/<n>/connected and, when the server says false, PUT Connected=true, then GET connected until it
// reads true or kProbeConnectWaitMs has passed. It runs on a background thread for the retry, and on the main thread
// for the guide RPC, each time with its own short-timeout AlpacaClient; only a device the server reports connected is
// then connected through GearDialog, which is fast because the drivers skip their PUT and wait for such a device.
class ProbeOps
{
public:
    virtual ~ProbeOps() = default;
    // GET connected; false with *error filled when the call failed
    virtual bool GetConnected(bool *connected, AlpacaErrors::CallError *error) = 0;
    // PUT Connected=true; false with *error filled when the call failed
    virtual bool PutConnected(AlpacaErrors::CallError *error) = 0;
    // The next GET or PUT must end within ms (> 0), below its own timeout if that is longer. Only called when RunProbe
    // has a deadline; each call sets it again.
    virtual void SetCallBudgetMs(int64_t ms) { (void) ms; }
    virtual void SleepMs(int ms) = 0;
    virtual int64_t NowMs() = 0;
    // true when the probe should give up: shutdown, or the user disconnected the device, chose another one or changed
    // the Alpaca server while it ran
    virtual bool Cancelled() = 0;
};

struct ProbeResult
{
    bool connected = false;
    bool cancelled = false; // stopped by ProbeOps::Cancelled: not a result, to be dropped
    // the deadline passed before the GET, or after a GET that read false but before the PUT: nothing was asked of
    // the device, so this is no attempt (reason is kOutOfTimeReason)
    bool outOfTime = false;
    std::string reason; // the one-line reason when !connected
};

// deviceName is "camera", "mount" or "rotator", for the reason text. deadlineMs (ops.NowMs() time, or kNoDeadline)
// bounds the whole probe: each call gets what is left of it (SetCallBudgetMs), the wait for connected ends with it,
// and a probe whose time is up before a call fails with kOutOfTimeReason without making it (outOfTime when that call
// was the GET or the PUT).
ProbeResult RunProbe(ProbeOps& ops, const char *deviceName, int connectWaitMs = kProbeConnectWaitMs,
                     int pollIntervalMs = kProbePollIntervalMs, int64_t deadlineMs = kNoDeadline);

// "no response from the Alpaca server (Timeout was reached)": the reason of a probe whose deadline passed, worded as
// a call that timed out (curl's text)
extern const char *const kOutOfTimeReason;

// The reason a connect gives when the Alpaca server settings are missing; not retried until they change.
extern const char *const kNotConfiguredReason;

// How a selected device is reached, from its choice string and the profile's Alpaca settings.
enum class ProbeKind
{
    Alpaca, // probed first
    NotAlpaca, // a local driver: connected directly, no probe
    NotConfigured, // an Alpaca device with no server port set
};

// What the guide RPC does for one selected device, decided in this order: a device that is not selected or already
// connected needs nothing; one the user disconnected is left alone, whatever else holds; a connect set_connected
// would refuse is refused; a device whose background probe is running waits for that probe's result (no second
// probe, and its result is not lost); an Alpaca device with no server fails as not configured; an Alpaca device is
// probed and then connected; a local one is connected.
enum class GuideStep
{
    Nothing,
    UserDisconnected,
    Refused,
    WaitForProbe,
    NotConfigured,
    ProbeThenConnect,
    Connect,
};
GuideStep PlanGuideConnect(bool selected, bool connected, Intent intent, bool refused, ProbeKind kind,
                           bool probeInFlight = false);

// The background probe's result, from the probe thread to the main thread. The probe thread hands it over here and
// then posts a CallAfter delivery. The guide RPC cannot wait for that delivery (it holds the main thread), so it
// waits here and takes the result itself; the delivery then finds nothing to do.
class ProbeHandoff
{
public:
    // main thread, before a probe thread starts
    void Begin();
    // probe thread, when its probe has ended
    void Finish(const ProbeResult& result);
    // main thread (guide): waits up to timeoutMs for the result; true with *result when it came, and it is then taken
    bool WaitAndTake(int64_t timeoutMs, ProbeResult *result);
    // main thread, the CallAfter delivery: true with *result unless the guide RPC took it
    bool TakeDelivered(ProbeResult *result);

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_finished = false;
    bool m_taken = false;
    ProbeResult m_result;
};

// The text a failed set_connected for one device gives: "the mount failed to connect" style prefixes are the
// caller's; this appends " (<reason>)" when there is a reason.
std::string WithReason(const std::string& text, const std::string& reason);

// One device the guide RPC could not connect.
struct GuideFailure
{
    enum class Kind
    {
        ConnectFailed, // the probe or the connect failed: reason
        UserDisconnected, // left alone on purpose
        Refused, // the connect was refused (ConnectRefusal): reason
        StillConnecting, // the background attempt had not ended within the guide's budget, or the server reports the
                         // device connected with too little of it left for the driver's connect: left to the retry
        OutOfTime, // the budget ran out before a request was sent for the device: left to the background retry
    };
    GearDevice device;
    Kind kind;
    std::string reason;
};

// "all equipment must be connected first", the old refusal text, as the prefix, then one clause per device in the
// order given, joined with "; ":
//   "the mount failed to connect: <reason>" (no ": <reason>" when there is none)
//   "the mount was disconnected by the user"
//   "the camera is not connected: <reason>"
//   "the mount is still connecting"
//   "the mount was not tried in time"
std::string GuideRefusal(const std::vector<GuideFailure>& failures);

// The reason the drivers give for a failed connect, handed to GearDialog. The drivers write it on the main thread
// during Connect; GearDialog clears the slot just before it calls Connect and takes it just after, so a write from a
// temporary camera object (get_selected_camera_pixelsize) can't leak into the selected device's result.
struct ConnectError
{
    std::string reason;
    bool retryable = true;
};
void RecordConnectError(GearDevice d, const std::string& reason, bool retryable = true);
// The same, unless a reason is already recorded (a generic failure path that runs after a specific one)
void RecordConnectErrorIfNone(GearDevice d, const std::string& reason);
void ClearConnectError(GearDevice d);
// read and clear
ConnectError TakeConnectError(GearDevice d);

// While one is alive (the background retry's connect), a driver's connect-failure Alert goes to the debug log only,
// so a device that keeps failing in its driver (the server reports it connected, a later property call fails) does
// not raise an Alert at every attempt. Main thread only; nests.
class QuietConnectScope
{
public:
    QuietConnectScope();
    ~QuietConnectScope();
    QuietConnectScope(const QuietConnectScope&) = delete;
    QuietConnectScope& operator=(const QuietConnectScope&) = delete;
};
bool QuietConnect();
} // namespace ConnectRetry

#endif
