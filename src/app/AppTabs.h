#pragma once

#include "app/AppContext.h"

using RefreshLiveStatusCallback = void (*)(AppContext*);

void ApplyActiveTab(
    AppContext* ctx,
    AppContext::MainTab tab,
    RefreshLiveStatusCallback refreshLiveStatus);
