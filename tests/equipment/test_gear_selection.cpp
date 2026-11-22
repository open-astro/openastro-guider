// Which camera, mount and rotator a profile's saved choices select (#115 step 3).
//
// GearDialog resolved them through wxChoice::SetStringSelection, which matches without case and leaves no selection
// when nothing matches; the camera then took the first item. These rules live in gear_selection.cpp now, and the
// gear lane of scripts/e2e_equipment_scenarios.py pins the same cases on the daemon.

#include <gtest/gtest.h>

#include "gear_selection.h"

#include <map>
#include <utility>

using namespace GearSelection;

namespace
{
const std::wstring NONE = L"None";
// the lists as GuideCameraList / MountList / RotatorList build them for an Alpaca server on 192.168.1.234
const std::vector<std::wstring> CAMERAS = { L"Alpaca Camera [192.168.1.234:11111/0]", NONE };
const std::vector<std::wstring> MOUNTS = { L"Alpaca Mount [192.168.1.234:11111/0]", NONE };
const std::vector<std::wstring> ROTATORS = { L"Alpaca Rotator [192.168.1.234:11111/0]", NONE };
const std::vector<std::wstring> INDI_CAMERAS = { L"INDI Camera [ccd_simulator]", NONE };
} // namespace

TEST(GearSelection, ExactMatch)
{
    EXPECT_EQ(FindChoice(CAMERAS, L"Alpaca Camera [192.168.1.234:11111/0]"), 0);
    EXPECT_EQ(FindChoice(CAMERAS, NONE), 1);
    EXPECT_EQ(ResolveChoice(GearDevice::Mount, MOUNTS, L"Alpaca Mount [192.168.1.234:11111/0]"), MOUNTS[0]);
    EXPECT_EQ(ResolveChoice(GearDevice::Rotator, ROTATORS, NONE), NONE);
}

TEST(GearSelection, CaseInsensitiveReturnsListCasing)
{
    // wxChoice::FindString's bCase defaults to false: a host typed in another case still selects the device, and the
    // list's casing is what goes back into LastMenuChoice
    const std::vector<std::wstring> mounts = { L"Alpaca Mount [rc51.lan:11111/0]", NONE };
    EXPECT_EQ(ResolveChoice(GearDevice::Mount, mounts, L"Alpaca Mount [RC51.lan:11111/0]"), mounts[0]);
    EXPECT_EQ(ResolveChoice(GearDevice::Mount, mounts, L"ALPACA MOUNT [RC51.LAN:11111/0]"), mounts[0]);
    EXPECT_EQ(ResolveChoice(GearDevice::Camera, CAMERAS, L"alpaca camera [192.168.1.234:11111/0]"), CAMERAS[0]);
    EXPECT_EQ(ResolveChoice(GearDevice::Mount, MOUNTS, L"none"), NONE);
    // the first item equal without case wins
    const std::vector<std::wstring> twins = { L"Cam A", L"cam a" };
    EXPECT_EQ(FindChoice(twins, L"cam a"), 0);
    // equal length is part of the match
    EXPECT_EQ(FindChoice(MOUNTS, L"Non"), -1);
    EXPECT_EQ(FindChoice(MOUNTS, L"None "), -1);
}

TEST(GearSelection, IndiRule)
{
    // a saved INDI name selects the first INDI item, with or without the driver part
    EXPECT_EQ(ResolveChoice(GearDevice::Camera, INDI_CAMERAS, L"INDI Camera"), INDI_CAMERAS[0]);
    EXPECT_EQ(ResolveChoice(GearDevice::Camera, INDI_CAMERAS, L"INDI Camera [other driver]"), INDI_CAMERAS[0]);
    const std::vector<std::wstring> mounts = { NONE, L"INDI Mount [EQMod Mount]" };
    EXPECT_EQ(ResolveChoice(GearDevice::Mount, mounts, L"INDI Mount"), mounts[1]);
    // the INDI rule is case-sensitive, as wxString::Contains is
    EXPECT_EQ(FindChoice(mounts, L"indi mount"), -1);
    // and needs INDI in the saved name
    EXPECT_EQ(FindChoice(mounts, L"EQMod Mount"), -1);
}

TEST(GearSelection, StaleCameraFallsBackToFirstItem)
{
    // a server that changed since the profile was saved: the camera takes the first item of the sorted list
    EXPECT_EQ(FindChoice(CAMERAS, L"Alpaca Camera [old:1/0]"), -1);
    EXPECT_EQ(ResolveChoiceIndex(GearDevice::Camera, CAMERAS, L"Alpaca Camera [old:1/0]"), 0);
    EXPECT_EQ(ResolveChoice(GearDevice::Camera, CAMERAS, L"Alpaca Camera [old:1/0]"), CAMERAS[0]);
    EXPECT_EQ(ResolveChoice(GearDevice::Camera, CAMERAS, L""), CAMERAS[0]);
    // no server: the bare Alpaca name sorts first
    const std::vector<std::wstring> bare = { L"Alpaca Camera", NONE };
    EXPECT_EQ(ResolveChoice(GearDevice::Camera, bare, L"ZWO ASI Camera"), bare[0]);
}

TEST(GearSelection, StaleMountAndRotatorResolveToNothing)
{
    EXPECT_EQ(ResolveChoiceIndex(GearDevice::Mount, MOUNTS, L"Alpaca Mount [old:1/0]"), -1);
    EXPECT_EQ(ResolveChoice(GearDevice::Mount, MOUNTS, L"Alpaca Mount [old:1/0]"), L"");
    EXPECT_EQ(ResolveChoice(GearDevice::Rotator, ROTATORS, L"Alpaca Rotator [old:1/0]"), L"");
    EXPECT_EQ(ResolveChoice(GearDevice::Mount, MOUNTS, L""), L"");
}

TEST(GearSelection, EmptyList)
{
    const std::vector<std::wstring> empty;
    EXPECT_EQ(ResolveChoiceIndex(GearDevice::Camera, empty, NONE), -1);
    EXPECT_EQ(ResolveChoice(GearDevice::Camera, empty, NONE), L"");
    EXPECT_EQ(ResolveChoice(GearDevice::Mount, empty, NONE), L"");
}

TEST(GearSelection, SavedChoiceFallbackChain)
{
    std::map<std::string, std::wstring> profile;
    std::vector<std::pair<std::string, std::wstring>> reads;
    ProfileRead read = [&](const std::string& key, const std::wstring& def)
    {
        reads.emplace_back(key, def);
        auto it = profile.find(key);
        return it == profile.end() ? def : it->second;
    };

    // LastMenuChoice set: the old spelling is not read
    profile["/camera/LastMenuChoice"] = L"Alpaca Camera";
    profile["/camera/LastMenuchoice"] = L"Old Camera";
    EXPECT_EQ(SavedChoice(read, NONE), L"Alpaca Camera");
    using Reads = std::vector<std::pair<std::string, std::wstring>>;
    EXPECT_EQ(reads, (Reads { { "/camera/LastMenuChoice", std::wstring() } }));

    // empty: the old spelling, read with "None" as its default
    reads.clear();
    profile["/camera/LastMenuChoice"] = L"";
    EXPECT_EQ(SavedChoice(read, NONE), L"Old Camera");
    EXPECT_EQ(reads, (Reads { { "/camera/LastMenuChoice", std::wstring() }, { "/camera/LastMenuchoice", NONE } }));

    // missing: the same
    profile.erase("/camera/LastMenuChoice");
    EXPECT_EQ(SavedChoice(read, NONE), L"Old Camera");

    // neither: None
    profile.erase("/camera/LastMenuchoice");
    EXPECT_EQ(SavedChoice(read, NONE), NONE);

    // the old spelling saved empty: None
    profile["/camera/LastMenuchoice"] = L"";
    EXPECT_EQ(SavedChoice(read, NONE), NONE);
}

TEST(GearSelection, CameraChanged)
{
    const std::wstring a = L"Alpaca Camera [192.168.1.234:11111/0]";
    const std::wstring b = L"Alpaca Camera [192.168.1.234:11111/1]";
    EXPECT_FALSE(CameraChanged(a, a, NONE));
    EXPECT_TRUE(CameraChanged(a, b, NONE));
    // None on either side is not a change
    EXPECT_FALSE(CameraChanged(NONE, a, NONE));
    EXPECT_FALSE(CameraChanged(a, NONE, NONE));
    EXPECT_FALSE(CameraChanged(NONE, NONE, NONE));
    // case-sensitive, unlike the list match
    EXPECT_TRUE(CameraChanged(a, L"alpaca camera [192.168.1.234:11111/0]", NONE));
    // the INDI family: any INDI camera matches any other, and nothing else
    EXPECT_FALSE(CameraChanged(L"INDI Camera", L"INDI Camera [ccd_simulator]", NONE));
    EXPECT_FALSE(CameraChanged(L"INDI Camera [a]", L"INDI Camera [b]", NONE));
    EXPECT_TRUE(CameraChanged(L"INDI Camera", a, NONE));
    EXPECT_TRUE(CameraChanged(a, L"INDI Camera", NONE));
    // an empty baseline is not None
    EXPECT_TRUE(CameraChanged(L"", a, NONE));
}

TEST(GearSelection, SelectionMatches)
{
    EXPECT_TRUE(SelectionMatches(L"x", L"x"));
    EXPECT_FALSE(SelectionMatches(L"x", L"X"));
    EXPECT_TRUE(SelectionMatches(L"INDI Camera", L"INDI Mount"));
    EXPECT_FALSE(SelectionMatches(L"INDI Camera", L"Alpaca Camera"));
}

TEST(GearSelection, CameraSelectionKeyGolden)
{
    // Taken from GearDialog::CameraSelectionKey before it moved (std::hash<std::string> of the name, libstdc++ on
    // arm64, the RC51 build): stored profiles hold these keys. A 32-bit size_t hashes differently.
#if !defined(__GLIBCXX__)
    GTEST_SKIP() << "the golden keys are those of libstdc++'s std::hash";
#endif
    if (sizeof(size_t) != 8)
        GTEST_SKIP() << "the golden keys are those of a 64-bit libstdc++";
    EXPECT_EQ(CameraSelectionKeyFor("Alpaca Camera"), "/cam_hash/54a1fc0cbc14d507/whichCamera");
    EXPECT_EQ(CameraSelectionKeyFor("Alpaca Camera [192.168.1.234:11111/0]"),
              "/cam_hash/ecf0945e7d5b928/whichCamera"); // %lx: no leading zero
    EXPECT_EQ(CameraSelectionKeyFor("None"), "/cam_hash/fb6ac968ce0ca5aa/whichCamera");
    EXPECT_EQ(CameraSelectionKeyFor(""), "/cam_hash/553e93901e462a6e/whichCamera");
}
