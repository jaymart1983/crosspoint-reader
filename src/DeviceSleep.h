#pragma once

// Device-level power actions that live in main.cpp, declared here so an
// activity can reach them the way SilentRestart.h exposes the restart paths.

// Deep sleep. fromTimeout distinguishes the inactivity timer from a deliberate
// request, which the sleep-screen mode reads.
void enterDeepSleep(bool fromTimeout = false);

// Ask for deep sleep at the top of the next loop() instead of right now.
// enterDeepSleep() replaces the whole activity stack, so an activity must never
// call it from inside its own loop() or event handler -- it would be destroyed
// with its handler still on the stack. Used by the control centre's Sleep tile.
void requestDeviceSleep();
