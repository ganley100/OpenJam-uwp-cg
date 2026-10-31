// NBA JAM: On Fire Edition - files outside the app's own storage
//
// See uwp_fromapp.cpp.

#pragma once

// Routes the process's plain Win32 file calls through their FromApp
// versions, so a USB drive and D:\DevelopmentFiles can be read and written
// by everything, the runtime included. Safe to call more than once; returns
// how many imports it changed this time.
int NbaRouteFilesThroughFromApp();
