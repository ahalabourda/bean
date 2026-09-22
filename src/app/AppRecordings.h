#pragma once

#include "app/AppContext.h"

void BackfillRecordingParticipantsFromKnownGuids(AppContext* ctx);
void UpdateRecordingInfoPane(AppContext* ctx, int selectedIndex);
void ApplyRecordingFilters(AppContext* ctx);
void SortRecordingItems(AppContext* ctx);
void RepopulateRecordingsListControl(AppContext* ctx);
