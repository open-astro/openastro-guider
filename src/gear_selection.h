/*
 *  gear_selection.h
 *  PHD Guiding
 *
 *  Which camera, mount and rotator a profile's saved choices select (#115 step 3): the saved name resolved against a
 *  device list, the camera's saved-name fallback chain, when a camera connect counts as a camera change, and the
 *  profile key of the camera id. Kept free of wx so it can be unit tested
 *  (tests/equipment/test_gear_selection.cpp).
 *
 *  The names are std::wstring, the characters of a wxString (wchar_t on Linux), so the case-insensitive match below
 *  folds case per character exactly as wxString::CmpNoCase does, with towlower.
 */

#ifndef GEAR_SELECTION_H_INCLUDED
#define GEAR_SELECTION_H_INCLUDED

#include "connect_request.h"

#include <functional>
#include <string>
#include <vector>

namespace GearSelection
{

// The index of the item a saved choice selects, or -1. As the Connect Equipment dialog's wxChoice::SetStringSelection
// did: the first item equal to it without case (wxChoice::FindString's default), then the INDI rule: a saved name
// containing "INDI" selects the first item containing "INDI" ("INDI Camera" or "INDI Camera [driver]" match either
// way). The item is the list's, so its casing is the one written back to LastMenuChoice.
int FindChoice(const std::vector<std::wstring>& items, const std::wstring& saved);

// FindChoice with the device's fallback when nothing matches (a stale choice): the camera takes the first item of its
// list (sorted, so the Alpaca camera before "None"), the mount and the rotator no item, -1, which reads as ''.
int ResolveChoiceIndex(GearDevice device, const std::vector<std::wstring>& items, const std::wstring& saved);

// the item ResolveChoiceIndex selects, or '' for none
std::wstring ResolveChoice(GearDevice device, const std::vector<std::wstring>& items, const std::wstring& saved);

// read(key, defaultValue): a profile string, as ConfigSection::GetString
using ProfileRead = std::function<std::wstring(const std::string& key, const std::wstring& defaultValue)>;

// The camera's saved choice: /camera/LastMenuChoice; when that is empty, the old spelling /camera/LastMenuchoice
// (read only then, defaulting to `none`); when that is empty too, `none`.
std::wstring SavedChoice(const ProfileRead& read, const std::wstring& none);

// A camera selection matching a saved one, case-sensitive: any INDI name matches any other, anything else must be
// equal.
bool SelectionMatches(const std::wstring& saved, const std::wstring& item);

// Whether connecting `newCam` changes the camera from `baseline` (the choice the profile was loaded with, or the
// last camera the change warning was given for): neither is `none` and they do not match. Case-sensitive, unlike
// FindChoice.
bool CameraChanged(const std::wstring& baseline, const std::wstring& newCam, const std::wstring& none);

// The profile key of the camera id saved for a camera choice: "/cam_hash/<std::hash of the name, %lx>/whichCamera".
// Stored profiles hold these keys, so the hash must not change (the golden values in the test).
std::string CameraSelectionKeyFor(const std::string& camName);

} // namespace GearSelection

#endif // GEAR_SELECTION_H_INCLUDED
