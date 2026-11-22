/*
 *  gear_session.h
 *  PHD Guiding
 *
 *  The sequencing of a gear request (#115 step 3): connect all, disconnect all, one device, and the profile switch;
 *  the request depth, the camera-change latch session (#139), the queued EquipmentReconnected events (#104) and the
 *  end-of-request step. The devices, the drivers and the frame are behind GearPort, so this is kept free of wx and
 *  unit tested against a recording fake port (tests/equipment/test_gear_session.cpp).
 *
 *  Error texts are UTF-8 and are internal to the event server (not translated); they go out byte-for-byte as the
 *  set_connected and set_profile error strings.
 */

#ifndef GEAR_SESSION_H_INCLUDED
#define GEAR_SESSION_H_INCLUDED

#include "connect_request.h"

#include <functional>
#include <string>
#include <vector>

// What a gear request needs of the devices and the rest of the guider. Device arguments are never GearDevice::All,
// except NoteUserDisconnect and OnConnectRequested, which take All for the whole set.
struct GearPort
{
    virtual ~GearPort() = default;

    // a device object exists for the device (its choice is not "None")
    virtual bool Selected(GearDevice dev) const = 0;
    virtual bool Connected(GearDevice dev) const = 0;
    virtual bool CaptureActive() const = 0;
    virtual bool CalibratingOrGuiding() const = 0;

    // the connect and disconnect of one device. Called whatever its state: a leaf handles "not selected" and
    // "already connected" itself (a status line, its #104 result).
    virtual void ConnectLeaf(GearDevice dev) = 0;
    virtual void DisconnectLeaf(GearDevice dev) = 0;
    // the reason the driver gave for the device's last failed connect, or ""
    virtual std::string LastConnectError(GearDevice dev) const = 0;
    // #104: the guider was asked to connect the device (All: all three)
    virtual void OnConnectRequested(GearDevice dev) = 0;
    // #104: the user wants the device left disconnected (All: all three)
    virtual void NoteUserDisconnect(GearDevice dev) = 0;
    // a debug-log line of the request (the text ends with a newline)
    virtual void Trace(const std::string& line) = 0;

    // the end-of-request hooks, called by GearSession::End in this order
    virtual void RefreshFrameControls() = 0;
    virtual bool AutoLoadCalibration() const = 0; // the profile's auto-load is on (and an AO, if any, is connected)
    virtual void LoadCalibration() = 0;
    virtual void ClearCalibration() = 0;
    virtual void RefreshAdvancedDialog() = 0; // the pages of the devices that changed, then a preload
    virtual void HandleImageScaleChange() = 0;
    virtual void SendReconnected(GearDevice dev) = 0; // EquipmentReconnected
};

class GearSession
{
public:
    explicit GearSession(GearPort& port);

    // Each returns true with *error set when the request failed or was refused, false otherwise.

    // Connect all: a no-op when the camera and every selected mount and rotator are connected; refused while
    // capturing. The three leaves run in the order camera, mount, rotator, each whatever its state, then End once.
    bool ConnectAll(std::string *error);
    // Disconnect all: a no-op (still the user's word, for #104) when nothing is connected; refused while capturing.
    // The leaves run in the order mount, camera, rotator, then NoteUserDisconnect(All) and End.
    bool DisconnectAll(std::string *error);
    // one device; All forwards to ConnectAll or DisconnectAll
    bool SetDevice(GearDevice dev, bool connect, std::string *error);
    // Switch to profile `target`: a no-op when it is the current one; refused while anything is connected, or when
    // selectTarget (run only after those two checks, so its profile reads and selection come where they always did)
    // returns false. Then a latch session begins (at depth 0; this takes no depth of its own), reload runs, and End.
    bool SetProfile(int current, int target, const std::function<bool()>& selectTarget, const std::function<void()>& reload,
                    std::string *error);

    // One gear request on the stack (RAII): the outermost begins a session of the latch. A request nested in another
    // (from a nested event loop) is part of it. The #104 retry reads Depth() > 0 as a connect in progress.
    class Request
    {
    public:
        explicit Request(GearSession& session);
        ~Request();
        Request(const Request&) = delete;
        Request& operator=(const Request&) = delete;

    private:
        GearSession& m_session;
    };

    CameraChangeLatch& Latch() { return m_latch; }
    // #104: an EquipmentReconnected for dev, sent at the end of End (after the calibration action)
    void QueueReconnected(GearDevice dev);
    int Depth() const { return m_depth; }

    // The end-of-request step, run exactly once per accepted request, never on a refusal or a no-op: frame
    // controls, the calibration action (auto-load or #139 clear), the AdvancedDialog refresh, the image-scale change
    // (taken once), then the queued EquipmentReconnected events.
    void End();

private:
    GearPort& m_port;
    CameraChangeLatch m_latch;
    int m_depth = 0;
    std::vector<GearDevice> m_reconnectedPending;
};

#endif // GEAR_SESSION_H_INCLUDED
