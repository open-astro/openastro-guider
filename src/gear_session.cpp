/*
 *  gear_session.cpp
 *  PHD Guiding
 *
 *  See gear_session.h.
 */

#include "gear_session.h"

GearSession::GearSession(GearPort& port) : m_port(port) { }

// #139: what End acts on (the image-scale change, the camera-change warning) belongs to one session, begun by the
// outermost request; a camera change carries over to later requests until the mount's calibration is cleared for it
GearSession::Request::Request(GearSession& session) : m_session(session)
{
    if (BeginsGearRequest(m_session.m_depth))
        m_session.m_latch.BeginRequest();
    ++m_session.m_depth;
}

GearSession::Request::~Request()
{
    --m_session.m_depth;
}

void GearSession::QueueReconnected(GearDevice dev)
{
    m_reconnectedPending.push_back(dev);
}

bool GearSession::ConnectAll(std::string *error)
{
    // the session begins before the no-op and refusal checks, as it always did
    Request request(*this);

    if (m_port.Connected(GearDevice::Camera) && (!m_port.Selected(GearDevice::Mount) || m_port.Connected(GearDevice::Mount)) &&
        (!m_port.Selected(GearDevice::Rotator) || m_port.Connected(GearDevice::Rotator)))
    {
        // everything already connected
        return false;
    }

    if (m_port.CaptureActive())
    {
        *error = ConnectRefusal(GearDevice::All, true, true, false);
        return true;
    }

    // the log lines keep the text of the GearDialog handlers they came from, which debug-log searches use
    m_port.Trace("gear_dialog: ConnectAll calls OnButtonConnectAll\n");

    m_port.OnConnectRequested(GearDevice::All);

    m_port.Trace("gear_dialog: OnButtonConnectAll\n");
    // every leaf runs, selected or not and connected or not: an unselected camera still logs its failed connect, and
    // a connected device still reports its result to the #104 tracker
    m_port.ConnectLeaf(GearDevice::Camera);
    m_port.ConnectLeaf(GearDevice::Mount);
    m_port.ConnectLeaf(GearDevice::Rotator);

    // once, on success too: GearDialog ran it a second time when everything connected, which only loaded the
    // profile's calibration again (#115 step 3)
    End();

    // each failed device with the reason its driver gave, when there is one (#104)
    auto failed = [this](const char *label, GearDevice dev)
    {
        std::string reason = m_port.LastConnectError(dev);
        return std::string(" ") + label + (reason.empty() ? std::string() : " (" + reason + ")");
    };
    std::string fail;
    if (!m_port.Selected(GearDevice::Camera))
        fail += " camera";
    else if (!m_port.Connected(GearDevice::Camera))
        fail += failed("camera", GearDevice::Camera);
    if (m_port.Selected(GearDevice::Mount) && !m_port.Connected(GearDevice::Mount))
        fail += failed("mount", GearDevice::Mount);
    if (m_port.Selected(GearDevice::Rotator) && !m_port.Connected(GearDevice::Rotator))
        fail += failed("Rotator", GearDevice::Rotator); // capitalised, as it always was

    if (fail.empty())
        return false;
    *error = "equipment failed to connect:" + fail;
    return true;
}

bool GearSession::DisconnectAll(std::string *error)
{
    Request request(*this);

    if (!m_port.Connected(GearDevice::Camera) && !m_port.Connected(GearDevice::Mount) && !m_port.Connected(GearDevice::Rotator))
    {
        // nothing connected; still the user's word that none of it is to be connected behind their back (#104)
        m_port.NoteUserDisconnect(GearDevice::All);
        return false;
    }

    if (m_port.CaptureActive())
    {
        *error = ConnectRefusal(GearDevice::All, false, true, false);
        return true;
    }

    m_port.Trace("gear_dialog: DisconnectAll calls OnButtonDisconnectAll\n");
    m_port.Trace("gear_dialog: OnButtonDisconnectAll\n");
    m_port.DisconnectLeaf(GearDevice::Mount);
    m_port.DisconnectLeaf(GearDevice::Camera);
    m_port.DisconnectLeaf(GearDevice::Rotator);
    // the devices that were not connected (a failed mount) included, now that the disconnect was accepted (#104)
    m_port.NoteUserDisconnect(GearDevice::All);

    End();

    return false;
}

bool GearSession::SetDevice(GearDevice dev, bool connect, std::string *error)
{
    if (dev == GearDevice::All)
        return connect ? ConnectAll(error) : DisconnectAll(error);

    Request request(*this);

    bool selected = m_port.Selected(dev);
    bool connected = selected && m_port.Connected(dev);

    const std::string name = GearDeviceName(dev);
    if (!selected)
    {
        if (!connect)
            return false; // nothing to disconnect, and no intent recorded for it
        *error = "no " + name + " is selected in the profile";
        return true;
    }
    if (connected == connect)
    {
        // already in the requested state; a disconnect request for a device that failed to connect still means
        // "leave it disconnected" (#104)
        if (!connect)
            m_port.NoteUserDisconnect(dev);
        return false;
    }

    std::string refusal = ConnectRefusal(dev, connect, m_port.CaptureActive(), m_port.CalibratingOrGuiding());
    if (!refusal.empty())
    {
        *error = refusal;
        return true;
    }

    m_port.Trace("gear_dialog: SetDeviceConnected " + name + " " + (connect ? "1" : "0") + "\n");

    if (connect)
    {
        m_port.OnConnectRequested(dev);
        m_port.ConnectLeaf(dev);
    }
    else
        m_port.DisconnectLeaf(dev);

    End(); // the side effects (calibration auto-load, controls)

    if (m_port.Connected(dev) != connect)
    {
        *error = name + " failed to " + (connect ? "connect" : "disconnect");
        // the reason the driver gave (#104)
        if (connect)
        {
            std::string reason = m_port.LastConnectError(dev);
            if (!reason.empty())
                *error += " (" + reason + ")";
        }
        return true;
    }
    return false;
}

bool GearSession::SetProfile(int current, int target, const std::function<bool()>& selectTarget,
                             const std::function<void()>& reload, std::string *error)
{
    if (target == current)
        return false;

    if (m_port.Connected(GearDevice::Camera) || m_port.Connected(GearDevice::Mount) || m_port.Connected(GearDevice::Rotator))
    {
        *error = "cannot set profile when equipment is connected";
        return true;
    }

    if (!selectTarget())
    {
        *error = "invalid profile id";
        return true;
    }

    // after the validation, unlike the connect requests, and with no depth of its own, as it always was
    if (BeginsGearRequest(m_depth))
        m_latch.BeginRequest();

    reload();

    End();

    return false;
}

void GearSession::End()
{
    m_port.RefreshFrameControls();

    // a camera change is acted on by the first End with the mount connected, which may be a later request's (#139);
    // until the end of the session the saved calibration, made with the old camera, is not auto-loaded
    switch (m_latch.TakeCalibrationAction(m_port.AutoLoadCalibration(), m_port.Connected(GearDevice::Mount)))
    {
    case GearCalibrationAction::AutoLoad:
        m_port.LoadCalibration();
        break;
    case GearCalibrationAction::Clear:
        m_port.ClearCalibration();
        break;
    case GearCalibrationAction::None:
        break;
    }

    m_port.RefreshAdvancedDialog();

    // taken, so a later request does not redo it over what the client has set since (#139)
    if (m_latch.TakeScaleChange())
        m_port.HandleImageScaleChange();

    // #104: the one place EquipmentReconnected goes out for a device that had failed to connect, after the
    // calibration action above; every connect path (set_connected, ConnectAll, guide, the retry) ends here
    std::vector<GearDevice> pending;
    pending.swap(m_reconnectedPending);
    for (GearDevice dev : pending)
        m_port.SendReconnected(dev);
}
