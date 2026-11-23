/*
 *  camera.cpp
 *  PHD Guiding
 *
 *  Created by Craig Stark.
 *  Copyright (c) 2006-2010 Craig Stark.
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

// General camera routines not specific to any one cam

#include "phd.h"

#include "camera.h"
#include "gear_simulator.h"
#include "alpaca_errors.h"
#include "connect_retry.h"
#include "gear_selection.h"
#include "image_scale.h"

#include <wx/stdpaths.h>

static const int DefaultGuideCameraGain = 95;
static const int DefaultGuideCameraTimeoutMs = 15000;
static const bool DefaultUseSubframes = false;

const double GuideCamera::UnknownPixelSize = 0.0;

wxSize UNDEFINED_FRAME_SIZE = wxSize(0, 0);

#if defined(ALPACA_CAMERA)
# include "cam_alpaca.h"
#endif

const wxString GuideCamera::DEFAULT_CAMERA_ID = wxEmptyString;

wxString CameraSelectionKey(const wxString& camName)
{
    std::string name(camName.c_str());
    return wxString(GearSelection::CameraSelectionKeyFor(name));
}

double GuideCamera::GetProfilePixelSize()
{
    return pConfig->Profile.GetDouble("/camera/pixelsize", UnknownPixelSize);
}

GuideCamera::GuideCamera() : FrameSize(UNDEFINED_FRAME_SIZE)
{
    Connected = false;
    m_hasGuideOutput = false;
    PropertyDialogType = PROPDLG_NONE;
    HasGainControl = false;
    HasShutter = false;
    ShutterClosed = false;
    HasSubframes = false;
    HasFrameLimiting = false;
    HasCooler = false;
    HasBayer = false;
    UseSubframes = pConfig->Profile.GetBoolean("/camera/UseSubframes", DefaultUseSubframes);
    GuideCameraGain = pConfig->Profile.GetInt("/camera/gain", DefaultGuideCameraGain);
    m_timeoutMs = pConfig->Profile.GetInt("/camera/TimeoutMs", DefaultGuideCameraTimeoutMs);
    m_saturationADU = (unsigned short) wxMin(pConfig->Profile.GetInt("/camera/SaturationADU", 0), 65535);
    m_saturationByADU = pConfig->Profile.GetBoolean("/camera/SaturationByADU", true);
    MaxHwBinning = 1;
    LoadProfileSetup();
    CurrentDarkFrame = nullptr;
    CurrentDefectMap = nullptr;
}

void GuideCamera::LoadProfileSetup()
{
    // Not SetBinning(), which clips to MaxHwBinning (1 before the first connect), nor SetCameraPixelSize(), which
    // updates the stats window: connect clamps HwBinning to the device and takes the pixel size the driver
    // reports, as it does for the values the constructor reads (#117).
    m_pixelSize = GetProfilePixelSize();
    HwBinning = pConfig->Profile.GetInt("/camera/binning", 1);
    SwBinning = wxClip(pConfig->Profile.GetInt("/camera/SoftwareBinning", 1), 1, (int) GuideCamera::MAX_SOFTWARE_BINNING);
}

GuideCamera::~GuideCamera()
{
    ClearDarks();
    ClearDefectMap();
}

static int CompareNoCase(const wxString& first, const wxString& second)
{
    return first.CmpNoCase(second);
}

static wxString AlpacaCamName()
{
    wxString host = pConfig->Profile.GetString("/alpaca/host", wxEmptyString);
    long port = pConfig->Profile.GetLong("/alpaca/port", 0);
    long device = pConfig->Profile.GetLong("/alpaca/camera_device", 0);
    if (host.empty() || port <= 0)
        return _T("Alpaca Camera");
    return wxString::Format("Alpaca Camera [%s:%ld/%ld]", host, port, device);
}

wxArrayString GuideCamera::GuideCameraList()
{
    wxArrayString CameraList;

    CameraList.Add(_("None"));
#if defined(ALPACA_CAMERA)
    CameraList.Add(AlpacaCamName());
#endif

    CameraList.Sort(&CompareNoCase);

    return CameraList;
}

GuideCamera *GuideCamera::Factory(const wxString& choice)
{
    GuideCamera *pReturn = nullptr;

    try
    {
        if (choice.IsEmpty())
        {
            throw ERROR_INFO("CameraFactory called with choice.IsEmpty()");
        }

        Debug.AddLine(wxString::Format("CameraFactory(%s)", choice));

        if (false) // so else ifs can follow
        {
        }
        // Route by explicit prefix. Avoid plain Contains() matches: a vendor
        // name containing "Alpaca" anywhere would otherwise misroute.
#if defined(ALPACA_CAMERA)
        else if (choice.StartsWith(_T("Alpaca Camera")))
        {
            pReturn = AlpacaCameraFactory::MakeAlpacaCamera();
        }
#endif
        else if (choice == _("None"))
            pReturn = nullptr;
        else
        {
            throw ERROR_INFO("CameraFactory: Unknown camera choice");
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        if (pReturn)
        {
            delete pReturn;
            pReturn = nullptr;
        }
    }

    return pReturn;
}

// ConnectCamera is the one place where we call camera->Connect(). Any work done here
// applies to all camera types, regardless of how the various camera sub-ckasses
// implement Connect().
bool GuideCamera::ConnectCamera(GuideCamera *camera, const wxString& cameraId)
{
    bool err = camera->Connect(cameraId);
    if (err)
        return err;
    if (camera->HasFrameLimiting)
    {
        // restore the saved limit frame (if any)
        auto binning = camera->GetBinning();
        camera->LoadLimitFrame(binning);
    }
    return false; // err was returned above if set; success path here
}

bool GuideCamera::EnumCameras(wxArrayString& names, wxArrayString& ids)
{
    return true; // error
}

bool GuideCamera::CamConnectFailed(const wxString& errorMessage)
{
    // the reason for the caller of the connect (#104), unless the failure path recorded a better one: the first
    // line of the message
    wxString first = errorMessage.BeforeFirst('\n');
    ConnectRetry::RecordConnectErrorIfNone(GearDevice::Camera, AlpacaErrors::OneLine(std::string(first.utf8_str())));
    if (ConnectRetry::QuietConnect())
        Debug.Write("camera connect failed (background retry, no alert): " + errorMessage + "\n");
    else
        pFrame->Alert(errorMessage);
    return true; // error
}

bool GuideCamera::SetCameraGain(int cameraGain)
{
    bool bError = false;

    try
    {
        if (cameraGain < 0)
        {
            throw ERROR_INFO("cameraGain < 0");
        }
        else if (cameraGain > 100)
        {
            throw ERROR_INFO("cameraGain > 100");
        }
        GuideCameraGain = cameraGain;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        GuideCameraGain = DefaultGuideCameraGain;
    }

    pConfig->Profile.SetInt("/camera/gain", GuideCameraGain);

    return bError;
}

int GuideCamera::GetDefaultCameraGain()
{
    return DefaultGuideCameraGain;
}

bool GuideCamera::SetBinning(int binning)
{
    auto hwSwBin = GetHwAndSwBinning(binning);
    auto hwBin = hwSwBin.first;
    auto swBin = hwSwBin.second;
    return SetBinning(hwBin, swBin);
}

bool GuideCamera::SetBinning(int hwBinning, int swBinning)
{
    HwBinning = wxClip(hwBinning, 1, MaxHwBinning);
    SwBinning = wxClip(swBinning, 1, (int) MAX_SOFTWARE_BINNING);

    auto binning = GetBinning();
    Debug.Write(wxString::Format("camera: set binning = %u (hw = %u, sw = %u)\n", (unsigned int) binning,
                                 (unsigned int) HwBinning, (unsigned int) SwBinning));

    pConfig->Profile.SetInt("/camera/binning", HwBinning);
    pConfig->Profile.SetInt("/camera/SoftwareBinning", SwBinning);

    // if a Limit Frame ROI is in use, adjust it for the new binning
    if (HasFrameLimiting)
    {
        // restore the saved limit frame for this binning (if any)
        LoadLimitFrame(binning);
    }

    return false;
}

bool GuideCamera::SetLimitFrame(const wxRect& roi, int binning, wxString *errorMessage)
{
    if (pFrame->pGuider->IsCalibratingOrGuiding())
    {
        *errorMessage = "Cannot set the frame limit ROI while calibrating or guiding.";
        return true;
    }

    // Construct and store limit frames for all available binning values. This allows
    // accurate binning switching without rounding errors when switching from a higher
    // binning to a lower binning.  For example, if we have a limit frame coordinate
    // value of 101 at bin 1, after switching to bin 2 the coordinate will be 50; and
    // switching back to bin 1 we can recover the inital value of 101.
    for (auto choice : GetBinningChoices())
    {
        auto new_bin = choice.first;
        wxRect const limit_frame(roi.x * binning / new_bin, roi.y * binning / new_bin, roi.width * binning / new_bin,
                                 roi.height * binning / new_bin);
        wxString const key = wxString::Format("/camera/LimitFrameBin%d", new_bin);
        pConfig->Profile.SetRect(key, limit_frame);
    }

    // load the limit frame for the current camera binning level
    binning = GetBinning();
    LoadLimitFrame(binning);

    return false; // no error
}

void GuideCamera::LoadLimitFrame(int binning)
{
    wxString const key = wxString::Format("/camera/LimitFrameBin%d", binning);
    LimitFrame = pConfig->Profile.GetRect(key);
    Debug.Write(wxString::Format("camera: updated LimitFrame => (%d,%d),(%dx%d)\n", LimitFrame.x, LimitFrame.y,
                                 LimitFrame.width, LimitFrame.height));
}

void GuideCamera::SetTimeoutMs(int ms)
{
    static const int MIN_TIMEOUT_MS = 5000;

    m_timeoutMs = wxMax(ms, MIN_TIMEOUT_MS);

    pConfig->Profile.SetInt("/camera/TimeoutMs", m_timeoutMs);
}

void GuideCamera::SetSaturationByADU(bool saturationByADU, unsigned short saturationADU)
{
    m_saturationByADU = saturationByADU;
    pConfig->Profile.SetBoolean("/camera/SaturationByADU", saturationByADU);

    if (saturationByADU)
    {
        m_saturationADU = saturationADU;
        pConfig->Profile.SetInt("/camera/SaturationADU", saturationADU);
        Debug.Write(wxString::Format("Saturation detection set to Max-ADU value %d\n", saturationADU));
    }
    else
        Debug.Write("Saturation detection set to star-profile-mode\n");
}

bool GuideCamera::SetCameraPixelSize(double pixel_size)
{
    bool bError = false;

    try
    {
        if (pixel_size <= 0.0)
        {
            throw ERROR_INFO("pixel_size <= 0");
        }

        m_pixelSize = pixel_size;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_pixelSize = UnknownPixelSize;
    }

    pConfig->Profile.SetDouble("/camera/pixelsize", m_pixelSize);

    return bError;
}

bool GuideCamera::SetCoolerOn(bool on)
{
    return true; // error
}

bool GuideCamera::SetCoolerSetpoint(double temperature)
{
    return true; // error
}

bool GuideCamera::GetCoolerStatus(bool *on, double *setpoint, double *power, double *temperature)
{
    return true; // error
}

bool GuideCamera::GetSensorTemperature(double *temperature)
{
    return true; // error
}

// Get all the available binning choices for both hardware and software binning.
// Hardware binning takes precedence over software binning.
//
// The max combined binning level is the camera's max binning or the software max
// binning (4), whichever is greater.
//
// Example: maxHwBin = 2
//    combined     hw     sw
//         1        1      1
//         2        2      1
//         3        1      3
//         4        2      2
BinningChoices GuideCamera::GetBinningChoices(int maxHwBin)
{
    int maxSwBin = GuideCamera::MAX_SOFTWARE_BINNING;
    int maxCombinedBin = wxMax(maxHwBin, maxSwBin);
    BinningChoices choices;
    for (int hwBin = 1; hwBin <= maxHwBin; hwBin++)
        for (int swBin = 1; swBin <= maxSwBin; swBin++)
        {
            auto combined = hwBin * swBin;
            if (combined > maxCombinedBin)
                continue;
            auto it = choices.find(combined);
            if (it == choices.end() || hwBin > it->second.first)
                choices[combined] = std::make_pair(hwBin, swBin);
        }
    return choices;
}

// Get the hardware and software binning levels for a given combined binning level and
// maximum hardware binning level.
//
// Uses GuideCamera::GetBinningChoices() to generate the list of valid choices, then
// selects the closest match having a combined binning value less than or equal to the
// requested value.
std::pair<int, int> GuideCamera::GetHwAndSwBinning(int maxHwBinning, int combinedBinning)
{
    auto prev = std::make_pair(1, 1);
    for (auto choice : GetBinningChoices(maxHwBinning))
    {
        if (choice.first == combinedBinning)
            return choice.second;
        if (choice.first > combinedBinning)
            return prev;
        prev = choice.second;
    }
    return prev;
}

wxString GuideCamera::GetSettingsSummary()
{
    int darkDur;

    { // lock scope
        wxCriticalSectionLocker lck(DarkFrameLock);
        darkDur = CurrentDarkFrame ? CurrentDarkFrame->ImgExpDur : 0;
    } // lock scope

    // return a loggable summary of current camera settings
    wxString pixelSizeStr;
    if (m_pixelSize == UnknownPixelSize)
        pixelSizeStr = _("unspecified");
    else
        pixelSizeStr = wxString::Format(_("%0.1f um"), m_pixelSize);

    return wxString::Format("Camera = %s%s, full size = %d x %d, %s, %s, pixel size = %s\n", Name,
                            HasGainControl ? wxString::Format(", gain = %d", GuideCameraGain) : wxString(),
                            FrameSize.GetWidth(), FrameSize.GetHeight(),
                            darkDur ? wxString::Format("have dark, dark dur = %d", darkDur) : wxString("no dark"),
                            CurrentDefectMap ? "defect map in use" : "no defect map", pixelSizeStr);
}

void GuideCamera::AddDark(usImage *dark)
{
    int const expdur = dark->ImgExpDur;

    { // lock scope
        wxCriticalSectionLocker lck(DarkFrameLock);

        // free the prior dark with this exposure duration
        ExposureImgMap::iterator pos = Darks.find(expdur);
        if (pos != Darks.end())
        {
            usImage *prior = pos->second;
            if (prior == CurrentDarkFrame)
                CurrentDarkFrame = dark;
            delete prior;
        }

    } // lock scope

    Darks[expdur] = dark;
}

void GuideCamera::SelectDark(int exposureDuration)
{
    // select the dark frame with the smallest exposure >= the requested exposure.
    // if there are no darks with exposures > the select exposure, select the dark with the greatest exposure

    wxCriticalSectionLocker lck(DarkFrameLock);

    CurrentDarkFrame = 0;
    for (ExposureImgMap::const_iterator it = Darks.begin(); it != Darks.end(); ++it)
    {
        CurrentDarkFrame = it->second;
        if (it->first >= exposureDuration)
            break;
    }
}

void GuideCamera::GetDarkLibraryProperties(int *pNumDarks, double *pMinExp, double *pMaxExp)
{
    // The exposures are in ms, and 0 with no darks loaded. Darks is ordered by exposure, so the
    // first and last are the extremes (a sentinel start of 9999 capped the minimum at 9999, #295).
    double minExp = 0.0;
    double maxExp = 0.0;
    int ct = 0;

    { // lock scope
        wxCriticalSectionLocker lck(DarkFrameLock);

        ct = static_cast<int>(Darks.size());
        if (!Darks.empty())
        {
            minExp = Darks.begin()->first;
            maxExp = Darks.rbegin()->first;
        }
    } // lock scope

    *pNumDarks = ct;
    *pMinExp = minExp;
    *pMaxExp = maxExp;
}

void GuideCamera::ClearDefectMap()
{
    wxCriticalSectionLocker lck(DarkFrameLock);

    if (CurrentDefectMap)
    {
        Debug.AddLine("Clearing defect map...");
        delete CurrentDefectMap;
        CurrentDefectMap = nullptr;
    }
}

void GuideCamera::SetDefectMap(DefectMap *defectMap)
{
    wxCriticalSectionLocker lck(DarkFrameLock);
    delete CurrentDefectMap;
    CurrentDefectMap = defectMap;
}

void GuideCamera::ClearDarks()
{
    wxCriticalSectionLocker lck(DarkFrameLock);
    while (!Darks.empty())
    {
        ExposureImgMap::iterator it = Darks.begin();
        delete it->second;
        Darks.erase(it);
    }
    CurrentDarkFrame = nullptr;
}

void GuideCamera::SubtractDark(usImage& img)
{
    // dark subtraction is done in the camera worker thread, so we need to acquire the
    // DarkFrameLock to protect against the dark frame disappearing when the main
    // thread does "Load Darks" or "Clear Darks"

    wxCriticalSectionLocker lck(DarkFrameLock);

    if (CurrentDefectMap)
    {
        RemoveDefects(img, *CurrentDefectMap);
    }
    else if (CurrentDarkFrame)
    {
        Subtract(img, *CurrentDarkFrame);
    }
}

static void InitiateReconnect()
{
    WorkerThread *thr = WorkerThread::This();
    if (thr)
    {
        // Defer sending the completion of exposure message until after
        // the camera re-connecttion attempt
        thr->SetSkipExposeComplete();
    }
    pFrame->TryReconnect();
}

void GuideCamera::DisconnectWithAlert(CaptureFailType type)
{
    switch (type)
    {
    case CAPT_FAIL_MEMORY:
        DisconnectWithAlert(_("Memory allocation error during capture"), NO_RECONNECT);
        break;

    case CAPT_FAIL_TIMEOUT:
    {
        wxString msg;
        // Dark library exposure times won't match the selected exposure time in the pull-down menu of the main window
        if (!ShutterClosed)
            msg = (wxString::Format(
                _("After %.1f sec the camera has not completed a %.1f sec exposure, so "
                  "it has been disconnected to prevent other problems. Refer to Trouble-shooting section of Help."),
                (pFrame->RequestedExposureDuration() + m_timeoutMs) / 1000., pFrame->RequestedExposureDuration() / 1000.));
        else
            msg = _("The camera has not completed an exposure in at least 15 seconds, so "
                    "it has been disconnected to prevent other problems. Refer to Trouble-shooting section of Help.");

        DisconnectWithAlert(msg, RECONNECT);
    }
    break;
    }
}

void GuideCamera::DisconnectWithAlert(const wxString& msg, ReconnectType reconnect)
{
    // #97: the capture timeout and the memory error give the failed capture its reason. Every caller is on a
    // capture path; a failed Alpaca call recorded before this (1031 / repeated 1280) takes priority.
    NoteCaptureAlert(msg);

    Disconnect();

    // ARA integration (PHD2-GAP gap 2): structured fault event alongside the
    // human-readable Alert, so clients route "camera disconnected" without
    // string-parsing. The event server's client set and sockets belong to the
    // main thread, so from the worker thread or the dark-capture thread (#100)
    // the event is posted there. It is queued before InitiateReconnect below
    // queues the reconnect, so EquipmentDisconnected always goes out ahead of
    // EquipmentReconnected / EquipmentReconnectFailed.
    bool reconnecting = reconnect == RECONNECT;
    if (wxThread::IsMain())
        EvtServer.NotifyEquipmentDisconnected("camera", msg, reconnecting);
    else
        pFrame->CallAfter([msg, reconnecting]() { EvtServer.NotifyEquipmentDisconnected("camera", msg, reconnecting); });

    // CAUTION: this function can be called from the worker thread, so
    // care must be taken not to make any direct UI updates

    pFrame->UpdateStatusBarStateLabels();
    pFrame->NotifyUpdateButtonsStatus(); // in case camera dialog button depends on connected state

    if (reconnect == RECONNECT)
    {
        pFrame->Alert(msg + "\n" + _("PHD will make several attempts to re-connect the camera."));
        InitiateReconnect();
    }
    else
    {
        pFrame->Alert(msg + "\n" +
                      _("The camera has been disconnected. Please resolve the problem and re-connect the camera."));
    }
}

CaptureFailures::Failure GuideCamera::TakeCaptureFailure()
{
    wxCriticalSectionLocker lock(m_lastCaptureFailureLock);
    CaptureFailures::Failure failure = m_lastCaptureFailure;
    m_lastCaptureFailure = CaptureFailures::Failure();
    return failure;
}

void GuideCamera::ClearCaptureFailure()
{
    wxCriticalSectionLocker lock(m_lastCaptureFailureLock);
    m_lastCaptureFailure = CaptureFailures::Failure();
}

void GuideCamera::RecordCaptureFailure(const AlpacaErrors::CallError& error)
{
    wxCriticalSectionLocker lock(m_lastCaptureFailureLock);
    CaptureFailures::RecordCallError(m_lastCaptureFailure, error);
}

void GuideCamera::NoteCaptureAlert(const wxString& msg)
{
    std::string text(msg.utf8_str());
    wxCriticalSectionLocker lock(m_lastCaptureFailureLock);
    CaptureFailures::NoteDriverAlert(m_lastCaptureFailure, text);
}

void GuideCamera::CaptureAlert(const wxString& msg)
{
    NoteCaptureAlert(msg);
    pFrame->Alert(msg);
}

void GuideCamera::InitCapture() { }

// convert a rectangle from binned coordinates to un-binned coordinates
inline static wxRect unbinned_rect(const wxRect& binnedRect, int binning)
{
    auto x = binnedRect.GetLeft() * binning;
    auto y = binnedRect.GetTop() * binning;
    auto width = binnedRect.GetWidth() * binning;
    auto heigth = binnedRect.GetHeight() * binning;
    return wxRect(x, y, width, heigth);
}

// convert a rectangle from un-binned coordinates to binned coordinates
inline static wxRect binned_rect(const wxRect& unbinnedRect, int binning)
{
    auto x = unbinnedRect.GetLeft() / binning;
    auto y = unbinnedRect.GetTop() / binning;
    auto width = unbinnedRect.GetWidth() / binning;
    auto heigth = unbinnedRect.GetHeight() / binning;
    return wxRect(x, y, width, heigth);
}

inline static wxSize binned_size(const wxSize& unbinnedSize, int binning)
{
    return wxSize(unbinnedSize.x / binning, unbinnedSize.y / binning);
}

bool GuideCamera::Capture(GuideCamera *camera, usImage& img, const CaptureParams& captureParams)
{
    // The subframe and LimitFrame are in software-binned coordinates, but the camera
    // subclass Capture methods work with hardware coordinates.
    int swBinning = captureParams.swBinning;
    CaptureParams cameraParams(captureParams);
    if (swBinning > 1)
    {
        cameraParams.limitFrame = unbinned_rect(captureParams.limitFrame, swBinning);
        cameraParams.subframe = unbinned_rect(captureParams.subframe, swBinning);
    }

    img.InitImgStartTime();
    img.LimitFrame = captureParams.limitFrame;
    img.Binning = captureParams.hwBinning;
    img.BitsPerPixel = captureParams.bpp;
    img.Gain = captureParams.gain;
    img.ImgExpDur = captureParams.duration;

    camera->ClearCaptureFailure(); // a new capture never inherits an old reason (#97)
    bool err = camera->Capture(img, cameraParams);
    if (err)
        return err;

    // perform software binning if needed
    if (swBinning > 1)
    {
        usImage binnedImage;
        if (binnedImage.Init(binned_size(img.Size, swBinning)))
        {
            camera->DisconnectWithAlert(CAPT_FAIL_MEMORY);
            return true;
        }
        BinPixels(binnedImage.ImageData, img.ImageData, img.Size, swBinning);
        img.SwapImageData(binnedImage);
        img.Size = binnedImage.Size;
        img.NPixels = binnedImage.NPixels;
        img.Binning *= swBinning;
        // scale the subframe from camera coords to binned coords
        img.Subframe = binned_rect(img.Subframe, swBinning);
    }

    return false; // err was returned above if set; success path here
}
