#pragma once

#include <windows.h>

struct AppContext;

int RunApplication(
    HINSTANCE instance,
    int cmdShow,
    WNDPROC windowProc,
    void (*refreshAboutUpdate)(AppContext*));
