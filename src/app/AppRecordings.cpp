#include "app/AppRecordings.h"

#include "app/AppDraw.h"
#include "app/AppRecordingHelpers.h"
#include "app/AppStatusLog.h"
#include "app/AppUtilities.h"
#include "core/WowData.h"
#include "util/Strings.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using bean::util::ToWide;

void BackfillRecordingParticipantsFromKnownGuids(AppContext* ctx)
{
    if (!ctx) {
        return;
    }

    struct GuidProfile {
        std::wstring bestName;
        std::wstring bestSpecAbbrev;
        std::optional<std::string> bestSpecName;
        std::optional<std::string> bestClassName;
        COLORREF bestColor = kColorTextMuted;
    };

    std::unordered_map<std::string, GuidProfile> profiles;
    for (const auto& recording : ctx->allRecordingItems) {
        for (const auto& participant : recording.participants) {
            if (participant.guid.empty()) {
                continue;
            }
            auto& profile = profiles[participant.guid];
            if (profile.bestName.empty() && !participant.name.empty() && !IsLikelyInvalidParticipantName(participant.name)) {
                profile.bestName = participant.name;
            }
            if (profile.bestSpecAbbrev.empty() && !participant.specAbbrev.empty()) {
                profile.bestSpecAbbrev = participant.specAbbrev;
            }
            if (!profile.bestSpecName.has_value() && participant.specName.has_value() && !participant.specName->empty()) {
                profile.bestSpecName = participant.specName;
            }
            if (!profile.bestClassName.has_value() && participant.className.has_value() && !participant.className->empty()) {
                profile.bestClassName = participant.className;
            }
            if (profile.bestColor == kColorTextMuted && participant.classColor != kColorTextMuted) {
                profile.bestColor = participant.classColor;
            }
        }
    }

    for (auto& recording : ctx->allRecordingItems) {
        for (auto& participant : recording.participants) {
            if (participant.guid.empty()) {
                continue;
            }
            const auto profileIt = profiles.find(participant.guid);
            if (profileIt == profiles.end()) {
                continue;
            }
            const auto& profile = profileIt->second;
            if ((participant.name.empty() || IsLikelyInvalidParticipantName(participant.name)) && !profile.bestName.empty()) {
                participant.name = profile.bestName;
            }
            if (participant.specAbbrev.empty() && !profile.bestSpecAbbrev.empty()) {
                participant.specAbbrev = profile.bestSpecAbbrev;
            }
            if ((!participant.specName.has_value() || participant.specName->empty()) && profile.bestSpecName.has_value()) {
                participant.specName = profile.bestSpecName;
            }
            if ((!participant.className.has_value() || participant.className->empty()) && profile.bestClassName.has_value()) {
                participant.className = profile.bestClassName;
            }
            if (participant.classColor == kColorTextMuted && profile.bestColor != kColorTextMuted) {
                participant.classColor = profile.bestColor;
            }
        }
    }
}

void UpdateRecordingParticipantsPane(AppContext* ctx, int)
{
    if (!ctx || !ctx->recordingsInfoText) {
        return;
    }
    ctx->participantsSelectedIndex = -1;
    RefreshBeanFileList(ctx->recordingsInfoText);
}

void UpdateRecordingInfoPane(AppContext* ctx, int selectedIndex)
{
    if (!ctx) {
        return;
    }
    UpdateRecordingParticipantsPane(ctx, selectedIndex);
    if (selectedIndex < 0 || static_cast<size_t>(selectedIndex) >= ctx->recordingItems.size()) {
        return;
    }
}

bool RecordingFilterCheckboxChecked(HWND control)
{
    return control && SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

RecordingFilterCriteria ReadRecordingFilterCriteria(const AppContext* ctx)
{
    RecordingFilterCriteria criteria;
    if (!ctx) {
        return criteria;
    }
    if (ctx->recordingsFilterTypeManualCheck
        || ctx->recordingsFilterTypeMythicCheck
        || ctx->recordingsFilterTypeRaidCheck
        || ctx->recordingsFilterTypePvpCheck) {
        criteria.includeManual = RecordingFilterCheckboxChecked(ctx->recordingsFilterTypeManualCheck);
        criteria.includeMythicPlus = RecordingFilterCheckboxChecked(ctx->recordingsFilterTypeMythicCheck);
        criteria.includeRaid = RecordingFilterCheckboxChecked(ctx->recordingsFilterTypeRaidCheck);
        criteria.includePvp = RecordingFilterCheckboxChecked(ctx->recordingsFilterTypePvpCheck);
    }
    if (ctx->recordingsFilterTimedCombo) {
        const LRESULT selected = SendMessageW(ctx->recordingsFilterTimedCombo, CB_GETCURSEL, 0, 0);
        if (selected == 1) {
            criteria.outcome = RecordingOutcomeFilter::Timed;
        } else if (selected == 2) {
            criteria.outcome = RecordingOutcomeFilter::Depleted;
        }
    }
    if (ctx->recordingsFilterKeyEdit) {
        RecordingKeyLevelFilter keyFilter;
        if (ParseRecordingKeyLevelFilter(GetWindowTextString(ctx->recordingsFilterKeyEdit), keyFilter)) {
            criteria.keyLevel = keyFilter;
        }
    }
    if (ctx->recordingsFilterCharsEdit) {
        criteria.characterNames = ParseRecordingCharacterNameFilter(GetWindowTextString(ctx->recordingsFilterCharsEdit));
    }
    return criteria;
}

RecordingFilterItem ToRecordingFilterItem(const AppContext::RecordingItem& item)
{
    RecordingFilterItem filterItem;
    filterItem.kind = item.kind;
    filterItem.timed = item.outcome == AppContext::RecordingItem::Outcome::Success;
    filterItem.depleted = item.outcome == AppContext::RecordingItem::Outcome::Failure;
    filterItem.keystoneLevel = item.keystoneLevel;
    for (const auto& participant : item.participants) {
        if (!participant.name.empty() && !IsLikelyInvalidParticipantName(participant.name)) {
            filterItem.participantNames.push_back(participant.name);
        }
    }
    return filterItem;
}

bool RecordingListDisplayEqual(
    const std::vector<AppContext::RecordingItem>& left,
    const std::vector<AppContext::RecordingItem>& right);

void UpdateRecordingsFolderSummary(AppContext* ctx, const std::wstring& folder)
{
    if (!ctx || !ctx->recordingsLabel) {
        return;
    }
    std::wostringstream summary;
    summary << L"Folder: " << folder << L" (";
    if (ctx->recordingItems.size() != ctx->allRecordingItems.size()) {
        summary << ctx->recordingItems.size() << L" of " << ctx->allRecordingItems.size();
    } else {
        summary << ctx->allRecordingItems.size();
    }
    summary << L" file";
    if (ctx->allRecordingItems.size() != 1) {
        summary << L"s";
    }
    summary << L")";
    UpdateTransparentStaticText(ctx->recordingsLabel, summary.str().c_str());
}

void ApplyRecordingFilters(AppContext* ctx)
{
    if (!ctx) {
        return;
    }

    std::filesystem::path selectedPath;
    if (ctx->recordingsSelectedIndex >= 0
        && static_cast<size_t>(ctx->recordingsSelectedIndex) < ctx->recordingItems.size()) {
        selectedPath = ctx->recordingItems[static_cast<size_t>(ctx->recordingsSelectedIndex)].path;
    }

    const auto criteria = ReadRecordingFilterCriteria(ctx);
    std::vector<AppContext::RecordingItem> visible;
    visible.reserve(ctx->allRecordingItems.size());
    for (const auto& item : ctx->allRecordingItems) {
        if (RecordingMatchesFilter(ToRecordingFilterItem(item), criteria)) {
            visible.push_back(item);
        }
    }

    int nextSelectedIndex = -1;
    if (!selectedPath.empty()) {
        for (size_t index = 0; index < visible.size(); ++index) {
            if (visible[index].path == selectedPath) {
                nextSelectedIndex = static_cast<int>(index);
                break;
            }
        }
    }

    const bool displayChanged = !RecordingListDisplayEqual(ctx->recordingItems, visible)
        || ctx->recordingsSelectedIndex != nextSelectedIndex;
    ctx->recordingItems = std::move(visible);
    ctx->recordingsSelectedIndex = nextSelectedIndex;
    if (displayChanged && ctx->recordingsList) {
        RefreshBeanFileList(ctx->recordingsList);
        UpdateRecordingInfoPane(ctx, ctx->recordingsSelectedIndex);
    }

    std::wstring folder = GetWindowTextString(ctx->outputEdit);
    if (folder.empty()) {
        folder = ToWide(ctx->settings.outputDirectory.string());
    }
    if (!folder.empty() && DirectoryExists(folder)) {
        UpdateRecordingsFolderSummary(ctx, folder);
    }
}

void SortRecordingItems(AppContext* ctx)
{
    if (!ctx) {
        return;
    }

    const auto column = ctx->recordingSortColumn;
    const bool asc = ctx->recordingSortAscending;
    std::sort(ctx->allRecordingItems.begin(), ctx->allRecordingItems.end(), [column, asc](const AppContext::RecordingItem& a, const AppContext::RecordingItem& b) {
        int cmp = 0;
        switch (column) {
        case AppContext::RecordingSortColumn::Dungeon:
            cmp = _wcsicmp(a.dungeonName.c_str(), b.dungeonName.c_str());
            break;
        case AppContext::RecordingSortColumn::Keystone:
            if (a.keystoneLevel < b.keystoneLevel) {
                cmp = -1;
            } else if (a.keystoneLevel > b.keystoneLevel) {
                cmp = 1;
            } else {
                cmp = 0;
            }
            break;
        case AppContext::RecordingSortColumn::Duration:
            if (a.duration < b.duration) {
                cmp = -1;
            } else if (a.duration > b.duration) {
                cmp = 1;
            } else {
                cmp = 0;
            }
            break;
        case AppContext::RecordingSortColumn::Date:
            if (a.modified < b.modified) {
                cmp = -1;
            } else if (a.modified > b.modified) {
                cmp = 1;
            } else {
                cmp = 0;
            }
            break;
        }
        return asc ? (cmp < 0) : (cmp > 0);
    });
}

void RepopulateRecordingsListControl(AppContext* ctx)
{
    if (!ctx || !ctx->recordingsList) {
        return;
    }

    ctx->recordingsSelectedIndex = -1;
    RefreshBeanFileList(ctx->recordingsList);
    UpdateRecordingInfoPane(ctx, ctx->recordingsSelectedIndex);
}

bool RecordingListDisplayEqual(
    const std::vector<AppContext::RecordingItem>& left,
    const std::vector<AppContext::RecordingItem>& right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        const auto& a = left[i];
        const auto& b = right[i];
        if (a.path != b.path
            || a.modified != b.modified
            || a.dungeonName != b.dungeonName
            || a.keystoneText != b.keystoneText
            || a.durationText != b.durationText
            || a.dateText != b.dateText
            || a.outcome != b.outcome
            || a.kind != b.kind
            || a.participants.size() != b.participants.size()) {
            return false;
        }
    }
    return true;
}

