/*
 *  scope_alpaca.cpp
 *  PHD Guiding
 *
 *  Copyright (c) 2026 PHD2 Developers
 *  All rights reserved.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of Craig Stark, Stark Labs nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "phd.h"

#ifdef GUIDE_ALPACA

# include "scope_alpaca.h"
# include "scope.h"
# include "connect_retry.h"
# include "alpaca_client.h"
# include "pulse_wait.h"
# include <wx/stopwatch.h>

ScopeAlpaca::ScopeAlpaca() : m_client(nullptr), m_host(pConfig->Profile.GetString("/alpaca/host", _T("localhost")))
{
    m_port = pConfig->Profile.GetLong("/alpaca/port", 0);
    m_deviceNumber = pConfig->Profile.GetLong("/alpaca/telescope_device", 0);
    m_Name = wxString::Format("Alpaca Mount [%s:%ld/%ld]", m_host, m_port, m_deviceNumber);

    // Initialize capability flags
    m_canCheckPulseGuiding = false;
    m_canGetCoordinates = false;
    m_canGetGuideRates = false;
    m_canPulseGuide = false;
    m_canGetSiteLatLong = false;
}

ScopeAlpaca::~ScopeAlpaca()
{
    Disconnect();
    if (m_client)
    {
        delete m_client;
        m_client = nullptr;
    }
}

bool ScopeAlpaca::Connect()
{
    bool bError = false;

    try
    {
        Debug.Write("Alpaca Mount: Connecting\n");

        if (IsConnected())
        {
            // no message box: Connect() runs from JSON-RPC on the headless
            // daemon, where a modal dialog would hang the main loop; the
            // thrown error reaches the caller
            throw ERROR_INFO("Alpaca Mount: Connected - Already Connected");
        }

        // If not configured, reload the profile settings in case they were
        // set over the API since this scope object was created. Never open
        // the setup dialog from Connect(): connects arrive over JSON-RPC on
        // the headless daemon, where a modal dialog would hang the main loop.
        if (m_port == 0)
        {
            m_host = pConfig->Profile.GetString("/alpaca/host", _T("localhost"));
            m_port = pConfig->Profile.GetLong("/alpaca/port", 0);
            m_deviceNumber = pConfig->Profile.GetLong("/alpaca/telescope_device", 0);
            if (m_port == 0)
            {
                Debug.Write("Alpaca Mount: not configured, skipping connection\n");
                ConnectRetry::RecordConnectError(GearDevice::Mount, ConnectRetry::kNotConfiguredReason, false);
                throw ERROR_INFO("Alpaca Mount: not configured - set the Alpaca server host and port first");
            }
        }

        if (!m_client)
        {
            m_client = new AlpacaClient(m_host, m_port, m_deviceNumber);
        }

        // Check if device is connected
        wxString endpoint = wxString::Format("telescope/%ld/connected", m_deviceNumber);
        bool connected = false;
        long errorCode = 0;
        AlpacaErrors::CallError callError;
        if (!m_client->GetBool(endpoint, &connected, &errorCode, nullptr, &callError))
        {
            // no message box (headless: a modal would hang the daemon when the
            // mount is simply unreachable); the thrown error reaches the caller.
            // The reason goes to the caller of the connect too (#104).
            std::string reason = AlpacaErrors::CallErrorReason(callError);
            ConnectRetry::RecordConnectError(GearDevice::Mount, reason);
            throw ERROR_INFO("Alpaca Mount: Could not check connection status, HTTP " + wxString::Format("%ld", errorCode) +
                             ": " + wxString(reason.c_str(), wxConvUTF8));
        }

        if (!connected)
        {
            // Try to connect
            wxString connectEndpoint = wxString::Format("telescope/%ld/connected", m_deviceNumber);
            wxString params = "Connected=true";
            JsonParser parser;
            // Put leaves errorCode alone when no response came: don't report the GET's 200 for it
            errorCode = 0;
            if (!m_client->Put(connectEndpoint, params, parser, &errorCode, nullptr, &callError))
            {
                // no message box (see above)
                std::string reason = AlpacaErrors::CallErrorReason(callError);
                ConnectRetry::RecordConnectError(GearDevice::Mount, reason);
                throw ERROR_INFO("Alpaca Mount: Could not connect device, HTTP " + wxString::Format("%ld", errorCode) + ": " +
                                 wxString(reason.c_str(), wxConvUTF8));
            }
        }

        // Check capabilities - mirror ASCOM approach

        // Check if we can check pulse guiding status
        m_canCheckPulseGuiding = true;
        endpoint = wxString::Format("telescope/%ld/ispulseguiding", m_deviceNumber);
        bool dummyBool = false;
        if (!m_client->GetBool(endpoint, &dummyBool, &errorCode))
        {
            m_canCheckPulseGuiding = false;
            Debug.Write("Alpaca Mount: cannot check IsPulseGuiding\n");
        }

        // Check if we can get coordinates
        m_canGetCoordinates = true;
        endpoint = wxString::Format("telescope/%ld/declination", m_deviceNumber);
        double dummyDouble = 0.0;
        if (!m_client->GetDouble(endpoint, &dummyDouble, &errorCode))
        {
            Debug.Write("Alpaca Mount: cannot get declination\n");
            m_canGetCoordinates = false;
        }
        else
        {
            endpoint = wxString::Format("telescope/%ld/rightascension", m_deviceNumber);
            if (!m_client->GetDouble(endpoint, &dummyDouble, &errorCode))
            {
                Debug.Write("Alpaca Mount: cannot get rightascension\n");
                m_canGetCoordinates = false;
            }
            else
            {
                endpoint = wxString::Format("telescope/%ld/siderealtime", m_deviceNumber);
                if (!m_client->GetDouble(endpoint, &dummyDouble, &errorCode))
                {
                    Debug.Write("Alpaca Mount: cannot get siderealtime\n");
                    m_canGetCoordinates = false;
                }
            }
        }

        // Check if we can get site latitude/longitude
        m_canGetSiteLatLong = true;
        endpoint = wxString::Format("telescope/%ld/sitelatitude", m_deviceNumber);
        if (!m_client->GetDouble(endpoint, &dummyDouble, &errorCode))
        {
            Debug.Write("Alpaca Mount: cannot get sitelatitude\n");
            m_canGetSiteLatLong = false;
        }
        else
        {
            endpoint = wxString::Format("telescope/%ld/sitelongitude", m_deviceNumber);
            if (!m_client->GetDouble(endpoint, &dummyDouble, &errorCode))
            {
                Debug.Write("Alpaca Mount: cannot get sitelongitude\n");
                m_canGetSiteLatLong = false;
            }
        }

        // Check if we can get guide rates
        m_canGetGuideRates = true;
        endpoint = wxString::Format("telescope/%ld/guideratedeclination", m_deviceNumber);
        if (!m_client->GetDouble(endpoint, &dummyDouble, &errorCode))
        {
            Debug.Write("Alpaca Mount: cannot get guideratedeclination\n");
            m_canGetGuideRates = false;
        }
        else
        {
            endpoint = wxString::Format("telescope/%ld/guideraterightascension", m_deviceNumber);
            if (!m_client->GetDouble(endpoint, &dummyDouble, &errorCode))
            {
                Debug.Write("Alpaca Mount: cannot get guideraterightascension\n");
                m_canGetGuideRates = false;
            }
        }

        // Check if we can pulse guide
        m_canPulseGuide = true;
        endpoint = wxString::Format("telescope/%ld/canpulseguide", m_deviceNumber);
        if (!m_client->GetBool(endpoint, &dummyBool, &errorCode) || !dummyBool)
        {
            Debug.Write("Alpaca Mount: connecting to scope that does not support PulseGuide\n");
            m_canPulseGuide = false;
        }

        // Get the scope name
        endpoint = wxString::Format("telescope/%ld/name", m_deviceNumber);
        wxString name;
        if (!m_client->GetString(endpoint, &name, &errorCode))
        {
            name.Clear();
        }

        if (!name.IsEmpty())
        {
            m_Name = wxString::Format("Alpaca Mount [%s:%ld/%ld] - %s", m_host, m_port, m_deviceNumber, name);
        }
        else
        {
            m_Name = wxString::Format("Alpaca Mount [%s:%ld/%ld]", m_host, m_port, m_deviceNumber);
        }

        Debug.Write(wxString::Format("Scope reports its name as %s\n", m_Name));

        Debug.Write(wxString::Format("%s connected\n", Name()));

        Scope::Connect();

        Debug.Write("Alpaca Mount: Connect success\n");
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
    }

    return bError;
}

bool ScopeAlpaca::Disconnect()
{
    bool bError = false;

    try
    {
        Debug.Write("Alpaca Mount: Disconnecting\n");

        if (!IsConnected())
        {
            throw ERROR_INFO("Alpaca Mount: attempt to disconnect when not connected");
        }

        if (m_client)
        {
            // Disconnect the device
            wxString endpoint = wxString::Format("telescope/%ld/connected", m_deviceNumber);
            wxString params = "Connected=false";
            long errorCode = 0;
            JsonParser parser;
            m_client->Put(endpoint, params, parser, &errorCode);
            // Don't fail if disconnect fails - device might already be disconnected
        }

        Debug.Write("Alpaca Mount: Disconnected Successfully\n");
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
    }

    Scope::Disconnect();

    return bError;
}

static wxString PulseGuideFailedAlertEnabledKey()
{
    // we want the key to be under "/Confirm" so ConfirmDialog::ResetAllDontAskAgain() resets it, but we also want the setting
    // to be per-profile
    return wxString::Format("/Confirm/%d/PulseGuideFailedAlertEnabled", pConfig->GetCurrentProfileId());
}

static void SuppressPulseGuideFailedAlert(intptr_t)
{
    pConfig->Global.SetBoolean(PulseGuideFailedAlertEnabledKey(), false);
}

static wxString SlewWarningEnabledKey()
{
    // we want the key to be under "/Confirm" so ConfirmDialog::ResetAllDontAskAgain() resets it, but we also want the setting
    // to be per-profile
    return wxString::Format("/Confirm/%d/SlewWarningEnabled", pConfig->GetCurrentProfileId());
}

static void SuppressSlewAlert(intptr_t)
{
    // If the user doesn't want to see these, we shouldn't be checking for the condition
    TheScope()->EnableStopGuidingWhenSlewing(false);
}

# define CheckSlewing(result)                                                                                                  \
     do                                                                                                                        \
     {                                                                                                                         \
         if (IsStopGuidingWhenSlewingEnabled() && Slewing())                                                                   \
         {                                                                                                                     \
             *(result) = MOVE_ERROR_SLEWING;                                                                                   \
             throw ERROR_INFO("attempt to guide while slewing");                                                               \
         }                                                                                                                     \
     } while (0)

// The PulseGuide request's abort check (#294): a stop (stop_capture) or an exit requested of the worker thread the
// pulse runs on. Polled by curl on that thread while the request waits on the mount; 0 off the worker thread.
static bool PulseGuideInterruptRequested()
{
    return WorkerThread::InterruptRequested() != 0;
}

Mount::MOVE_RESULT ScopeAlpaca::Guide(GUIDE_DIRECTION direction, int duration)
{
    MOVE_RESULT result = MOVE_OK;

    try
    {
        Debug.Write(wxString::Format("Guiding  Dir = %d, Dur = %d\n", direction, duration));

        if (!IsConnected())
        {
            throw ERROR_INFO("Alpaca Mount: attempt to guide when not connected");
        }

        if (!m_canPulseGuide)
        {
            // Could happen if move command is issued on the Aux mount or CanPulseGuide property got changed on the fly
            pFrame->Alert(_("Alpaca driver does not support PulseGuide. Check your Alpaca driver settings."));
            throw ERROR_INFO("Alpaca Mount: guide command issued but PulseGuide not supported");
        }

        // First, check to see if already moving
        CheckSlewing(&result);

        if (IsGuiding())
        {
            Debug.Write("Entered PulseGuideScope while moving\n");
            int i;
            for (i = 0; i < 20; i++)
            {
                wxMilliSleep(50);

                CheckSlewing(&result);

                if (!IsGuiding())
                    break;

                Debug.Write("Still moving\n");
            }
            if (i == 20)
            {
                Debug.Write("Still moving after 1s - aborting\n");
                throw ERROR_INFO("Alpaca Mount: scope is still moving after 1 second");
            }
            else
            {
                Debug.Write("Movement stopped - continuing\n");
            }
        }

        // Do the move - Alpaca pulseguide is asynchronous
        int alpacaDirection = -1;
        switch (direction)
        {
        case NORTH:
            alpacaDirection = 0;
            break;
        case SOUTH:
            alpacaDirection = 1;
            break;
        case EAST:
            alpacaDirection = 2;
            break;
        case WEST:
            alpacaDirection = 3;
            break;
        default:
            Debug.Write("Alpaca Mount: Invalid guide direction\n");
            throw ERROR_INFO("Alpaca Mount: Invalid guide direction");
        }

        wxString endpoint = wxString::Format("telescope/%ld/pulseguide", m_deviceNumber);
        wxString params = wxString::Format("Direction=%d&Duration=%d", alpacaDirection, duration);

        long errorCode = 0;
        // The request's own limits (#294): a mount holding it fails the move after the pulse plus a margin, not the
        // client's 30 s, and a stop gives it up within about a second instead of waiting for either
        AlpacaRequestLimits limits;
        limits.timeoutMs = PulseWait::RequestTimeoutMs((long) duration);
        limits.abortRequested = &PulseGuideInterruptRequested;
        AlpacaErrors::CallError callError;
        // Started before the request, as upstream ScopeASCOM does: a PulseGuide that blocks
        // while the pulse runs has already spent that time, and it must not be waited for again.
        wxStopWatch swatch;
        if (!m_client->PutAction(endpoint, "PulseGuide", params, &errorCode, nullptr, &callError, &limits))
        {
            if (callError.kind == AlpacaErrors::CallError::Kind::Transport &&
                callError.curlCode == AlpacaErrors::kCurlAbortedByCallback)
            {
                // A stop no longer waits for the mount; the mount may still run the pulse. No canpulseguide check:
                // it would wait on the same mount
                Debug.Write(wxString::Format("Alpaca Mount: PulseGuide %s %d ms given up after %ld ms, a stop was "
                                             "requested\n",
                                             DirectionChar(direction), duration, swatch.Time()));
                throw ERROR_INFO("Alpaca Mount: pulseguide request given up on a stop");
            }

            Debug.Write(wxString::Format("pulseguide: HTTP %ld\n", errorCode));

            // Make sure nothing got by us and the mount can really handle pulse guide
            wxString canPulseEndpoint = wxString::Format("telescope/%ld/canpulseguide", m_deviceNumber);
            bool canPulse = false;
            if (m_client->GetBool(canPulseEndpoint, &canPulse, &errorCode) && !canPulse)
            {
                Debug.Write("Tried to guide mount that has no PulseGuide support\n");
                // This will trigger a nice alert the next time through Guide
                m_canPulseGuide = false;
            }
            throw ERROR_INFO("Alpaca Mount: pulseguide command failed, HTTP " + wxString::Format("%ld", errorCode));
        }

        long callMs = swatch.Time();
        Debug.Write(wxString::Format("PulseGuide call took %ld ms\n", callMs));

        // Alpaca pulseguide is asynchronous, so we need to wait for it to complete
        // Wait at least the duration, then check if still guiding
        long sleepMs = PulseWait::SleepBeforeFirstPollMs((long) duration, callMs);
        if (sleepMs > 0)
        {
            Debug.Write(wxString::Format("PulseGuide returned control before completion, sleep %ld\n", sleepMs));

            if (WorkerThread::MilliSleep(sleepMs))
                throw ERROR_INFO("Alpaca Mount: thread terminate requested");
        }

        if (IsGuiding())
        {
            Debug.Write("scope still moving after pulse duration time elapsed\n");

            // try waiting a little longer. If scope does not stop moving after 2 seconds, bail out with an error
            bool timeoutExceeded = false;

            while (true)
            {
                ::wxMilliSleep(20);

                if (WorkerThread::InterruptRequested())
                    throw ERROR_INFO("Alpaca Mount: thread interrupt requested");

                CheckSlewing(&result);

                if (!IsGuiding())
                {
                    Debug.Write(wxString::Format("scope move finished after %ld + %ld ms\n", (long) duration,
                                                 swatch.Time() - (long) duration));
                    break;
                }

                if (PulseWait::CompletionTimedOut((long) duration, swatch.Time()))
                {
                    timeoutExceeded = true;
                    break;
                }
            }

            if (timeoutExceeded && IsGuiding())
            {
                throw ERROR_INFO("timeout exceeded waiting for guiding pulse to complete");
            }
        }
    }
    catch (const wxString& msg)
    {
        POSSIBLY_UNUSED(msg);

        if (result == MOVE_OK)
        {
            result = MOVE_ERROR;

            // At most one alert every 30 s, not one per failed pulse (guide_move_plan.h, #188)
            if (!WorkerThread::InterruptRequested() && m_pulseFailedAlertThrottle.ShouldRaise(guide_move_plan::AlertClockS()))
            {
                pFrame->SuppressibleAlert(PulseGuideFailedAlertEnabledKey(),
                                          _("PulseGuide command to mount has failed - guiding is likely to be ineffective."),
                                          SuppressPulseGuideFailedAlert, 0);
            }
        }
    }

    if (result == MOVE_ERROR_SLEWING)
    {
        pFrame->SuppressibleAlert(SlewWarningEnabledKey(), _("Guiding stopped: the scope started slewing."), SuppressSlewAlert,
                                  0);
    }

    return result;
}

bool ScopeAlpaca::IsGuiding()
{
    bool bReturn = true;

    try
    {
        if (!m_canCheckPulseGuiding)
        {
            // Assume all is good - best we can do as this is really a fail-safe check.  If we can't call this property (lame
            // driver) guides will have to enforce the wait.  But, enough don't support this that we can't throw an error.
            throw ERROR_INFO("Alpaca Mount: IsGuiding - !m_canCheckPulseGuiding");
        }

        wxString endpoint = wxString::Format("telescope/%ld/ispulseguiding", m_deviceNumber);
        bool isGuiding = false;
        long errorCode = 0;
        if (!m_client->GetBool(endpoint, &isGuiding, &errorCode))
        {
            pFrame->Alert(_("Alpaca driver failed checking IsPulseGuiding. See the debug log for more information."));
            throw ERROR_INFO("Alpaca Mount: IsGuiding - IsPulseGuiding failed, HTTP " + wxString::Format("%ld", errorCode));
        }

        bReturn = isGuiding;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bReturn = false;
    }

    Debug.Write(wxString::Format("IsGuiding returns %d\n", bReturn));

    return bReturn;
}

bool ScopeAlpaca::IsSlewing()
{
    wxString endpoint = wxString::Format("telescope/%ld/slewing", m_deviceNumber);
    bool slewing = false;
    long errorCode = 0;
    if (!m_client->GetBool(endpoint, &slewing, &errorCode))
    {
        Debug.Write(wxString::Format("ScopeAlpaca::IsSlewing failed: HTTP %ld\n", errorCode));
        pFrame->Alert(_("Alpaca driver failed checking for slewing, see the debug log for more information."));
        // Deliberate fail-open: on an HTTP/transport error we report "not
        // slewing" so a transient glitch does not interrupt an in-progress
        // guiding session. Failing closed (returning true) could stop guiding
        // on every intermittent error, which is worse in practice than missing
        // a rare real slew. Do not change this to fail-closed without a
        // debounce/confirmation strategy.
        return false;
    }

    bool result = slewing;

    Debug.Write(wxString::Format("IsSlewing returns %d\n", result));

    return result;
}

bool ScopeAlpaca::HasNonGuiMove()
{
    return true;
}

bool ScopeAlpaca::CanCheckSlewing()
{
    return true;
}

bool ScopeAlpaca::Slewing()
{
    bool bReturn = true;

    try
    {
        if (!IsConnected())
        {
            throw ERROR_INFO("Alpaca Mount: Cannot check Slewing when not connected to mount");
        }

        bReturn = IsSlewing();
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bReturn = false;
    }

    return bReturn;
}

// return the declination in radians, or UNKNOWN_DECLINATION
double ScopeAlpaca::GetDeclinationRadians()
{
    double dReturn = UNKNOWN_DECLINATION;

    try
    {
        if (!IsConnected())
        {
            throw ERROR_INFO("Alpaca Mount: cannot get Declination when not connected to mount");
        }

        if (!m_canGetCoordinates)
        {
            throw THROW_INFO("!m_canGetCoordinates");
        }

        wxString endpoint = wxString::Format("telescope/%ld/declination", m_deviceNumber);
        double decDegrees = 0.0;
        long errorCode = 0;
        if (!m_client->GetDouble(endpoint, &decDegrees, &errorCode))
        {
            throw ERROR_INFO("GetDeclinationRadians() fails, HTTP " + wxString::Format("%ld", errorCode));
        }

        dReturn = radians(decDegrees);
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        // Do not clear m_canGetCoordinates here: a transient transport/HTTP
        // error (e.g. timeout) would otherwise permanently disable coordinate
        // reads. Just return UNKNOWN_DECLINATION for this call. The capability
        // flag is managed by Connect()'s capability probe.
    }

    Debug.Write(wxString::Format("ScopeAlpaca::GetDeclinationRadians() returns %s\n", DeclinationStr(dReturn)));

    return dReturn;
}

// Return RA and Dec guide rates in native ASCOM units, degrees/sec.
// Convention is to return true on an error
bool ScopeAlpaca::GetGuideRates(double *pRAGuideRate, double *pDecGuideRate)
{
    bool bError = false;

    try
    {
        if (!IsConnected())
        {
            throw ERROR_INFO("Alpaca Mount: cannot get guide rates when not connected");
        }

        if (!m_canGetGuideRates)
        {
            throw THROW_INFO("Alpaca Mount: not capable of getting guide rates");
        }

        wxString endpoint = wxString::Format("telescope/%ld/guideratedeclination", m_deviceNumber);
        double decRate = 0.0;
        long errorCode = 0;
        if (!m_client->GetDouble(endpoint, &decRate, &errorCode))
        {
            throw ERROR_INFO("Alpaca Mount: GuideRateDec() failed, HTTP " + wxString::Format("%ld", errorCode));
        }

        *pDecGuideRate = decRate;

        endpoint = wxString::Format("telescope/%ld/guideraterightascension", m_deviceNumber);
        double raRate = 0.0;
        if (!m_client->GetDouble(endpoint, &raRate, &errorCode))
        {
            throw ERROR_INFO("Alpaca Mount: GuideRateRA() failed, HTTP " + wxString::Format("%ld", errorCode));
        }

        *pRAGuideRate = raRate;

        if (!ValidGuideRates(*pRAGuideRate, *pDecGuideRate))
        {
            if (!m_bogusGuideRatesFlagged)
            {
                pFrame->Alert(_("The mount's Alpaca driver is reporting invalid guide speeds. Some guiding functions including "
                                "PPEC will be impaired. Contact the Alpaca driver provider or mount vendor for support."),
                              0, wxEmptyString, 0, 0, true);
                m_bogusGuideRatesFlagged = true;
            }
            // Don't throw - allow connection to proceed with warning
            // Some mounts may not report guide speeds or may return 0.0 initially
            Debug.Write(wxString::Format(
                "Alpaca Mount: Warning - invalid guide speeds (RA: %.4f, Dec: %.4f), but allowing connection\n", *pRAGuideRate,
                *pDecGuideRate));
            bError = true; // Return error but don't block connection
            return bError;
        }
    }
    catch (const wxString& Msg)
    {
        bError = true;
        POSSIBLY_UNUSED(Msg);
    }

    Debug.Write(wxString::Format("ScopeAlpaca::GetGuideRates returns %u %.3f %.3f a-s/sec\n", bError,
                                 bError ? 0.0 : *pDecGuideRate * 3600., bError ? 0.0 : *pRAGuideRate * 3600.));

    return bError;
}

bool ScopeAlpaca::GetCoordinates(double *ra, double *dec, double *siderealTime)
{
    bool bError = false;

    try
    {
        if (!IsConnected())
        {
            throw ERROR_INFO("Alpaca Mount: cannot get coordinates when not connected");
        }

        if (!m_canGetCoordinates)
        {
            throw THROW_INFO("Alpaca Mount: not capable of getting coordinates");
        }

        wxString endpoint = wxString::Format("telescope/%ld/rightascension", m_deviceNumber);
        double raHours = 0.0;
        long errorCode = 0;
        if (!m_client->GetDouble(endpoint, &raHours, &errorCode))
        {
            throw ERROR_INFO("Alpaca Mount: get right ascension failed, HTTP " + wxString::Format("%ld", errorCode));
        }

        endpoint = wxString::Format("telescope/%ld/declination", m_deviceNumber);
        double decDegrees = 0.0;
        if (!m_client->GetDouble(endpoint, &decDegrees, &errorCode))
        {
            throw ERROR_INFO("Alpaca Mount: get declination failed, HTTP " + wxString::Format("%ld", errorCode));
        }

        endpoint = wxString::Format("telescope/%ld/siderealtime", m_deviceNumber);
        double stHours = 0.0;
        if (!m_client->GetDouble(endpoint, &stHours, &errorCode))
        {
            throw ERROR_INFO("Alpaca Mount: get sidereal time failed, HTTP " + wxString::Format("%ld", errorCode));
        }

        // PHD2's GetCoordinates convention is (hours, degrees, hours) — see
        // ScopeManualPointing::GetCoordinates; callers compute hour angles as
        // lst - ra. Alpaca already reports hours/degrees, so pass through.
        if (ra)
            *ra = raHours;
        if (dec)
            *dec = decDegrees;
        if (siderealTime)
            *siderealTime = stHours;
    }
    catch (const wxString& Msg)
    {
        bError = true;
        POSSIBLY_UNUSED(Msg);
    }

    return bError;
}

bool ScopeAlpaca::GetSiteLatLong(double *latitude, double *longitude)
{
    if (!m_canGetSiteLatLong)
        return true;

    bool bError = false;

    try
    {
        if (!IsConnected())
        {
            throw ERROR_INFO("Alpaca Mount: cannot get site latitude/longitude when not connected");
        }

        wxString endpoint = wxString::Format("telescope/%ld/sitelatitude", m_deviceNumber);
        double lat = 0.0;
        long errorCode = 0;
        if (!m_client->GetDouble(endpoint, &lat, &errorCode))
        {
            throw ERROR_INFO("Alpaca Mount: get site latitude failed, HTTP " + wxString::Format("%ld", errorCode));
        }

        endpoint = wxString::Format("telescope/%ld/sitelongitude", m_deviceNumber);
        double lon = 0.0;
        if (!m_client->GetDouble(endpoint, &lon, &errorCode))
        {
            throw ERROR_INFO("Alpaca Mount: get site longitude failed, HTTP " + wxString::Format("%ld", errorCode));
        }

        *latitude = lat;
        *longitude = lon;
    }
    catch (const wxString& Msg)
    {
        bError = true;
        POSSIBLY_UNUSED(Msg);
    }

    return bError;
}

bool ScopeAlpaca::CanReportPosition()
{
    return true;
}

bool ScopeAlpaca::CanPulseGuide()
{
    return m_canPulseGuide;
}

PierSide ScopeAlpaca::SideOfPier()
{
    PierSide pierSide = PIER_SIDE_UNKNOWN;

    try
    {
        if (!IsConnected())
        {
            throw ERROR_INFO("Alpaca Mount: cannot get side of pier when not connected");
        }

        wxString endpoint = wxString::Format("telescope/%ld/sideofpier", m_deviceNumber);
        int sideOfPier = -1;
        long errorCode = 0;
        if (!m_client->GetInt(endpoint, &sideOfPier, &errorCode))
        {
            throw THROW_INFO("Alpaca Mount: not capable of getting side of pier");
        }

        switch (sideOfPier)
        {
        case 0:
            pierSide = PIER_SIDE_EAST;
            break;
        case 1:
            pierSide = PIER_SIDE_WEST;
            break;
        default:
            pierSide = PIER_SIDE_UNKNOWN;
            break;
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    Debug.Write(wxString::Format("ScopeAlpaca::SideOfPier() returns %d\n", pierSide));

    return pierSide;
}

Scope *AlpacaScopeFactory::MakeAlpacaScope()
{
    return new ScopeAlpaca();
}

#endif // GUIDE_ALPACA
