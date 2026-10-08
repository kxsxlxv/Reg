#include "launcher/WindowIdentity.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

using reg::launcher::WindowChoice;
using reg::launcher::WindowTarget;
using reg::launcher::findTargetIndex;
using reg::launcher::matchesTarget;
using reg::launcher::targetOf;

int main() {
    const std::vector<WindowChoice> running{
        {101, 4000, "MissionPlanner.exe", "Mission Planner", 
         "C:\\Tools\\MissionPlanner.exe", "WindowsForms10.Window"},
        {102, 5000, "notepad.exe", "Notes — Work",
         "C:\\Windows\\System32\\notepad.exe", "Notepad"},
        {103, 5000, "notepad.exe", "Notes — Personal",
         "C:\\Windows\\System32\\notepad.exe", "Notepad"},
        {104, 6000, "chrome.exe", "Chat",
         "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
         "Chrome_WidgetWin_1"},
        {105, 7000, "Elevated app", "Elevated app — Settings",
         "", "RestrictedWindow"}
    };

    const WindowTarget mission = targetOf(running[0]);
    assert(findTargetIndex(running, mission) == 0);
    assert(findTargetIndex(running, mission, 102) == 0);
    assert(!matchesTarget(running[1], mission));

    // Case-insensitive path and class comparisons (Windows semantics).
    const WindowTarget chrome{
        "c:\\program files\\google\\chrome\\application\\CHROME.EXE",
        "chrome_widgetwin_1", "Chat"};
    assert(findTargetIndex(running, chrome) == 3);

    const WindowTarget selectedNotes = targetOf(running[2]);
    assert(findTargetIndex(running, selectedNotes) == 2);
    // Current handle wins while the app is running, even if its title changes.
    auto updated = running;
    updated[2].title = "New Notes — Personal";
    assert(findTargetIndex(updated, selectedNotes, 103) == 2);
    // Without a live preferred handle, multiple windows with renamed titles
    // are ambiguous and must NEVER cause a random app window to be moved.
    assert(findTargetIndex(updated, selectedNotes) == -1);
    assert(findTargetIndex(std::vector<WindowChoice>{updated[2]},
                           selectedNotes) == 0);
    // Reused HWND 103 belonging to another app must not match old identity.
    updated[2].executablePath = "C:\\Other\\malware.exe";
    updated.push_back({106, 5000, "notepad.exe", "Unrelated document",
         "C:\\Windows\\System32\\notepad.exe", "Notepad"});
    assert(findTargetIndex(updated, selectedNotes, 103) == -1);

    const WindowTarget restricted = targetOf(running[4]);
    assert(findTargetIndex(running, restricted) == 4);
    auto renamed = running;
    renamed[4].title = "Not the same window";
    assert(findTargetIndex(renamed, restricted, 105) == -1);
    assert(findTargetIndex(running, WindowTarget{}) == -1);
    assert(reg::launcher::caseInsensitiveEqual("Notepad.EXE", "notepad.exe"));
    assert(!reg::launcher::caseInsensitiveEqual("notepad.exe", "calc.exe"));
}
