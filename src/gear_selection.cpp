/*
 *  gear_selection.cpp
 *  PHD Guiding
 *
 *  See gear_selection.h.
 */

#include "gear_selection.h"

#include <cstdio>
#include <cwctype>

namespace GearSelection
{

// wxString::CmpNoCase(...) == 0: the same length, and each character equal after towlower
static bool EqualNoCase(const std::wstring& a, const std::wstring& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::towlower((wint_t) a[i]) != std::towlower((wint_t) b[i]))
            return false;
    }
    return true;
}

static bool ContainsINDI(const std::wstring& s)
{
    return s.find(L"INDI") != std::wstring::npos;
}

bool SelectionMatches(const std::wstring& saved, const std::wstring& item)
{
    if (ContainsINDI(saved))
        return ContainsINDI(item);
    return saved == item;
}

int FindChoice(const std::vector<std::wstring>& items, const std::wstring& saved)
{
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (EqualNoCase(items[i], saved))
            return (int) i;
    }

    for (size_t i = 0; i < items.size(); ++i)
    {
        if (SelectionMatches(saved, items[i]))
            return (int) i;
    }

    return -1;
}

int ResolveChoiceIndex(GearDevice device, const std::vector<std::wstring>& items, const std::wstring& saved)
{
    int i = FindChoice(items, saved);
    if (i < 0 && device == GearDevice::Camera && !items.empty())
        i = 0;
    return i;
}

std::wstring ResolveChoice(GearDevice device, const std::vector<std::wstring>& items, const std::wstring& saved)
{
    int i = ResolveChoiceIndex(device, items, saved);
    return i < 0 ? std::wstring() : items[i];
}

std::wstring SavedChoice(const ProfileRead& read, const std::wstring& none)
{
    std::wstring choice = read("/camera/LastMenuChoice", std::wstring());
    if (choice.empty())
        choice = read("/camera/LastMenuchoice", none);
    if (choice.empty())
        choice = none;
    return choice;
}

bool CameraChanged(const std::wstring& baseline, const std::wstring& newCam, const std::wstring& none)
{
    return baseline != none && newCam != none && !SelectionMatches(baseline, newCam);
}

std::string CameraSelectionKeyFor(const std::string& camName)
{
    std::hash<std::string> hash_fn;
    char key[64];
    snprintf(key, sizeof(key), "/cam_hash/%lx/whichCamera", (unsigned long) hash_fn(camName));
    return key;
}

} // namespace GearSelection
