#include "app/AppRecordings.h"

#include "app/AppDraw.h"
#include "app/AppRecordingHelpers.h"
#include "app/AppProbeController.h"
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


void RefreshRecordingsList(AppContext* ctx, bool startReconciliation)
{
    if (!ctx || !ctx->recordingsList || !ctx->recordingsLabel) {
        return;
    }

    const auto folders = CollectKnownRecordingFolders(ctx);
    const bool anyFolderAvailable = std::any_of(
        folders.begin(),
        folders.end(),
        [](const auto& folder) { return DirectoryExists(folder.wstring()); });
    if (!anyFolderAvailable) {
        if (!ctx->allRecordingItems.empty() || !ctx->recordingItems.empty()) {
            ctx->allRecordingItems.clear();
            ctx->recordingItems.clear();
            ctx->recordingsSelectedIndex = -1;
            RefreshBeanFileList(ctx->recordingsList);
            UpdateRecordingInfoPane(ctx, -1);
        }
        UpdateTransparentStaticText(ctx->recordingsLabel, L"Recordings folder is unavailable.");
        if (startReconciliation) {
            BeginRecordingReconciliation(ctx);
        }
        return;
    }

    // One query for the whole table instead of one per video file. A user with
    // a few hundred recordings previously triggered that many separate queries
    // on every refresh, and refresh happens on tab switches and path edits.
    std::unordered_map<std::string, bean::core::RunRecord> runsByVideoPath;
    if (ctx->runRepository) {
        std::string dbError;
        for (const auto& run : ctx->runRepository->ListRuns(dbError)) {
            runsByVideoPath.emplace(RecordingPathKey(run.videoPath), run);
            for (const auto& alias : run.pathAliases) {
                runsByVideoPath.emplace(RecordingPathKey(alias), run);
            }
        }
    }

    // Reuse metadata for files whose mtime has not changed, so a tab switch
    // does not rebuild participant rows for every historical recording.
    std::unordered_map<std::wstring, const AppContext::RecordingItem*> previousByPath;
    previousByPath.reserve(ctx->allRecordingItems.size());
    for (const auto& item : ctx->allRecordingItems) {
        previousByPath.emplace(item.path.wstring(), &item);
    }

    std::vector<AppContext::RecordingItem> nextItems;
    for (const auto& mediaPath : EnumerateRecordingMediaFilesInFolders(folders)) {
        std::error_code timeEc;
        const auto modified = std::filesystem::last_write_time(mediaPath, timeEc);
        const auto writeTime = timeEc ? std::filesystem::file_time_type::clock::now() : modified;

        const auto previousIt = previousByPath.find(mediaPath.wstring());
        if (previousIt != previousByPath.end() && previousIt->second->modified == writeTime) {
            nextItems.push_back(*previousIt->second);
            continue;
        }

        AppContext::RecordingItem row;
        row.path = mediaPath;
        row.fileName = row.path.filename().wstring();
        row.dungeonName.clear();
        row.modified = writeTime;
        row.dateText = FormatLocalDate(FileTimeToSystemClock(row.modified));

        {
            const auto runIt = runsByVideoPath.find(RecordingPathKey(row.path));
            const std::optional<bean::core::RunRecord> run = runIt == runsByVideoPath.end()
                ? std::nullopt
                : std::optional<bean::core::RunRecord>(runIt->second);
            if (run.has_value()) {
                if (run->dungeonName.has_value() && !run->dungeonName->empty()) {
                    row.dungeonName = ToWide(*run->dungeonName);
                } else if (run->challengeMapId.has_value()) {
                    const auto inferredDungeonName = bean::core::DungeonNameForChallengeMap(*run->challengeMapId);
                    if (!inferredDungeonName.empty()) {
                        row.dungeonName = ToWide(inferredDungeonName);
                    }
                } else if (_stricmp(run->triggerReason.c_str(), "manual") == 0) {
                    row.dungeonName = L"Manual Recording";
                }
                if (run->keystoneLevel.has_value()) {
                    row.keystoneLevel = *run->keystoneLevel;
                    row.keystoneText = L"+" + std::to_wstring(*run->keystoneLevel);
                }
                if (run->recordingEndedAt > run->recordingStartedAt) {
                    row.duration = std::chrono::duration_cast<std::chrono::seconds>(run->recordingEndedAt - run->recordingStartedAt);
                    row.durationText = FormatElapsed(row.duration);
                }
                if (_stricmp(run->result.c_str(), "success") == 0
                    || _stricmp(run->result.c_str(), "timed") == 0
                    || _stricmp(run->stopReason.c_str(), "mythic-success") == 0) {
                    row.outcome = AppContext::RecordingItem::Outcome::Success;
                } else if (_stricmp(run->result.c_str(), "failure") == 0
                    || _stricmp(run->result.c_str(), "depleted") == 0
                    || _stricmp(run->result.c_str(), "overtime") == 0
                    || _stricmp(run->stopReason.c_str(), "mythic-failure") == 0) {
                    row.outcome = AppContext::RecordingItem::Outcome::Failure;
                }
                for (const auto& participant : run->participants) {
                    AppContext::RecordingItem::ParticipantUi participantUi;
                    participantUi.guid = participant.guid;
                    if (participant.name.has_value()) {
                        participantUi.name = ToWide(*participant.name);
                    }
                    participantUi.specAbbrev = SpecAbbreviationFromName(participant.specName);
                    participantUi.specId = participant.specId;
                    participantUi.specName = participant.specName;
                    participantUi.className = participant.className;
                    participantUi.classColor = ClassColorForParticipant(participant.className);
                    row.participants.push_back(std::move(participantUi));
                }
                row.kind = ClassifyRecordingKind(
                    run->triggerReason,
                    run->keystoneLevel.has_value() || run->challengeMapId.has_value());
            }
        }

        if (row.dungeonName.empty()) {
            row.dungeonName = row.path.stem().wstring();
        }
        if (row.keystoneLevel < 0) {
            row.keystoneText = L"-";
        }
        nextItems.push_back(std::move(row));
    }

    ctx->allRecordingItems = std::move(nextItems);
    BackfillRecordingParticipantsFromKnownGuids(ctx);
    SortRecordingItems(ctx);
    ApplyRecordingFilters(ctx);
    if (startReconciliation) {
        BeginRecordingReconciliation(ctx);
    }
}

