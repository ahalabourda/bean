#include "core/ClipExportService.h"
#include "core/GameEnvironment.h"
#include "core/RecordingOrchestrator.h"
#include "core/RecordingPath.h"
#include "core/RunMetadataWriter.h"
#include "core/RunRepository.h"
#include "core/SettingsStore.h"
#include "core/WowData.h"
#include "obs/IRecorderEngine.h"
#include "obs/MockRecorderEngine.h"
#include "util/Json.h"
#include "util/Strings.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;

void Expect(bool condition, const std::string& message)
{
    if (!condition) {
        ++gFailures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::filesystem::path MakeTempDir(const std::string& name)
{
    const auto base = std::filesystem::temp_directory_path() / "bean-tests";
    const auto dir = base / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ec.clear();
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void TestJsonEscapeRoundTrip()
{
    using bean::util::EscapeJson;
    using bean::util::UnescapeJson;

    Expect(EscapeJson("plain") == "plain", "Plain text should escape unchanged.");
    Expect(EscapeJson("a\"b\\c") == "a\\\"b\\\\c", "Quotes and backslashes should escape.");
    Expect(EscapeJson("line\nfeed\tend") == "line\\nfeed\\tend", "Control characters should escape.");

    const std::string original = R"(path\with "quotes" and)" "\nnewline";
    Expect(UnescapeJson(EscapeJson(original)) == original, "Escape/Unescape should round-trip.");
}

void TestJsonReaders()
{
    using bean::util::ReadJsonBool;
    using bean::util::ReadJsonInt;
    using bean::util::ReadJsonString;

    const std::string doc =
        "{\n"
        "  \"name\": \"alpha\\\\beta\",\n"
        "  \"count\": 42,\n"
        "  \"enabled\": true,\n"
        "  \"disabled\": false\n"
        "}\n";

    Expect(ReadJsonString(doc, "name") == "alpha\\beta", "ReadJsonString should unescape values.");
    Expect(ReadJsonString(doc, "missing").empty(), "Missing string key should return empty.");
    Expect(ReadJsonInt(doc, "count", -1) == 42, "ReadJsonInt should parse integers.");
    Expect(ReadJsonInt(doc, "missing", 7) == 7, "Missing int key should return fallback.");
    Expect(ReadJsonBool(doc, "enabled", false), "ReadJsonBool should parse true.");
    Expect(!ReadJsonBool(doc, "disabled", true), "ReadJsonBool should parse false.");
    Expect(ReadJsonBool(doc, "missing", true), "Missing bool key should return fallback.");
}

void TestStringConversions()
{
    using bean::util::ToUtf8;
    using bean::util::ToWide;
    using bean::util::Trim;

    Expect(Trim("  hello\t\r\n") == "hello", "Trim should strip whitespace.");
    Expect(Trim("").empty(), "Trim of empty should stay empty.");
    Expect(Trim("nospace") == "nospace", "Trim should leave interior text alone.");

    const std::string utf8 = "Bean \xE2\x9C\x93"; // check mark
    const auto wide = ToWide(utf8);
    Expect(!wide.empty(), "ToWide should convert UTF-8.");
    Expect(ToUtf8(wide) == utf8, "ToWide/ToUtf8 should round-trip UTF-8.");
    Expect(ToUtf8(L"plain") == "plain", "ToUtf8(wchar*) should convert ASCII.");
}

void TestDungeonNameTable()
{
    Expect(bean::core::DungeonNameForChallengeMap(402) == "Algeth'ar Academy", "Map 402 should resolve.");
    Expect(bean::core::DungeonNameForChallengeMap(161) == "Skyreach", "Map 161 should resolve.");
    Expect(bean::core::DungeonNameForChallengeMap(560) == "Maisara Caverns", "Map 560 should resolve.");
    Expect(bean::core::DungeonNameForChallengeMap(99999).empty(), "Unknown map id should return empty.");
}

void TestEncoderPresetQualityValues()
{
    using bean::obs::ResolveConstantQualityValueForPreset;
    const int ultra = ResolveConstantQualityValueForPreset("ultra");
    const int high = ResolveConstantQualityValueForPreset("high");
    const int medium = ResolveConstantQualityValueForPreset("medium");
    const int low = ResolveConstantQualityValueForPreset("low");
    const int minimum = ResolveConstantQualityValueForPreset("minimum");
    Expect(ultra < high && high < medium && medium < low && low < minimum,
        "Higher quality presets should map to lower CQ/CRF values.");
    Expect(ResolveConstantQualityValueForPreset("unknown") == high,
        "Unknown preset should fall back to high.");
}

void TestAudioCaptureScopeLabels()
{
    using bean::core::AudioCaptureScopeLabel;
    using Scope = bean::core::AppSettings::AudioCaptureScope;
    Expect(std::string(AudioCaptureScopeLabel(Scope::WowOnly)) == "wow-only", "WowOnly label.");
    Expect(std::string(AudioCaptureScopeLabel(Scope::WowAndDiscord)) == "wow+discord", "WowAndDiscord label.");
    Expect(std::string(AudioCaptureScopeLabel(Scope::AllDesktop)) == "all-desktop", "AllDesktop label.");
}

void TestWowInstallPathResolution()
{
    using bean::core::ResolveWowInstallDirectoryFromLogDirectory;
    Expect(!ResolveWowInstallDirectoryFromLogDirectory({}).has_value(), "Empty log dir should not resolve.");
    Expect(
        !ResolveWowInstallDirectoryFromLogDirectory(R"(C:\Games\NotWow\Logs)").has_value(),
        "Non-_retail_ Logs path should not resolve.");

    const auto fromLogs = ResolveWowInstallDirectoryFromLogDirectory(
        R"(C:\Program Files\World of Warcraft\_retail_\Logs)");
    Expect(fromLogs.has_value(), "Logs path under _retail_ should resolve.");
    if (fromLogs.has_value()) {
        Expect(fromLogs->filename() == L"World of Warcraft" || fromLogs->wstring().find(L"World of Warcraft") != std::wstring::npos,
            "Resolved install should be the WoW root.");
    }

    const auto fromRetail = ResolveWowInstallDirectoryFromLogDirectory(
        R"(D:\Games\WoW\_retail_)");
    Expect(fromRetail.has_value() && fromRetail->filename() == L"WoW",
        "_retail_ path should resolve to its parent.");
}

void TestAdvancedCombatLoggingConfigParse()
{
    const auto install = MakeTempDir("wow-install-fake");
    const auto wtfDir = install / "_retail_" / "WTF";
    std::filesystem::create_directories(wtfDir);
    {
        std::ofstream cfg(wtfDir / "Config.wtf", std::ios::trunc);
        cfg << "SET advancedCombatLogging \"1\"\n";
        cfg << "SET otherThing \"0\"\n";
    }
    const auto logs = install / "_retail_" / "Logs";
    std::filesystem::create_directories(logs);
    Expect(bean::core::IsAdvancedCombatLoggingEnabled(logs),
        "Config.wtf with advancedCombatLogging 1 should report enabled.");

    {
        std::ofstream cfg(wtfDir / "Config.wtf", std::ios::trunc);
        cfg << "SET advancedCombatLogging \"0\"\n";
    }
    Expect(!bean::core::IsAdvancedCombatLoggingEnabled(logs),
        "Config.wtf with advancedCombatLogging 0 should report disabled.");
}

void TestRunMetadataWriterPersistsSuccess()
{
    const auto dir = MakeTempDir("metadata-writer");
    auto repo = std::make_shared<bean::core::RunRepository>(dir / "runs.db");
    std::string error;
    Expect(repo->Initialize(error), "Metadata writer repo should initialize.");

    bean::core::RunMetadataWriter writer(repo);
    std::vector<std::string> statuses;
    writer.SetStatusCallback([&](const std::string& status) { statuses.push_back(status); });

    bean::core::FinishedRecordingSnapshot snapshot;
    snapshot.triggerReason = bean::core::RecordingStartReason::MythicStart;
    snapshot.videoPath = dir / "out" / "skyreach-12.mkv";
    snapshot.recordingStartedAt = std::chrono::system_clock::from_time_t(1700000000);
    snapshot.recordingEndedAt = std::chrono::system_clock::from_time_t(1700001800);
    snapshot.challengeMapId = 161;
    snapshot.keystoneLevel = 12;
    snapshot.participants = {{"Player-1", "Alpha", "Area52", std::nullopt, 268, "Brewmaster", "Monk"}};

    writer.Persist(std::move(snapshot), bean::core::RecordingStopReason::MythicSuccess);

    const auto loaded = repo->GetRunByVideoPath(dir / "out" / "skyreach-12.mkv", error);
    Expect(loaded.has_value(), "Persisted run should be readable.");
    if (loaded.has_value()) {
        Expect(loaded->result == "success", "MythicSuccess should persist result=success.");
        Expect(loaded->dungeonName.has_value() && *loaded->dungeonName == "Skyreach",
            "Writer should fill dungeon name from challenge map id.");
        Expect(loaded->participants.size() == 1, "Writer should persist participants.");
    }
    Expect(statuses.empty() || statuses.front().find("Failed") == std::string::npos,
        "Successful persist should not push a failure status.");
}

void TestRunMetadataWriterManualFallbackAndMissingRepo()
{
    bean::core::RunMetadataWriter writer;
    std::vector<std::string> statuses;
    writer.SetStatusCallback([&](const std::string& status) { statuses.push_back(status); });

    bean::core::FinishedRecordingSnapshot snapshot;
    snapshot.triggerReason = bean::core::RecordingStartReason::Manual;
    snapshot.videoPath = "C:/tmp/manual.mkv";
    snapshot.recordingStartedAt = std::chrono::system_clock::now();
    snapshot.recordingEndedAt = snapshot.recordingStartedAt + std::chrono::seconds(10);
    writer.Persist(std::move(snapshot), bean::core::RecordingStopReason::Manual);
    Expect(!statuses.empty() && statuses.front().find("unavailable") != std::string::npos,
        "Missing repository should push an unavailable status.");

    const auto dir = MakeTempDir("metadata-manual");
    auto repo = std::make_shared<bean::core::RunRepository>(dir / "runs.db");
    std::string error;
    Expect(repo->Initialize(error), "Manual fallback repo should initialize.");
    writer.SetRepository(repo);
    statuses.clear();

    bean::core::FinishedRecordingSnapshot manual;
    manual.triggerReason = bean::core::RecordingStartReason::Manual;
    manual.videoPath = dir / "manual-take.mkv";
    manual.recordingStartedAt = std::chrono::system_clock::from_time_t(1700000000);
    manual.recordingEndedAt = std::chrono::system_clock::from_time_t(1700000100);
    writer.Persist(std::move(manual), bean::core::RecordingStopReason::Manual);

    const auto loaded = repo->GetRunByVideoPath(dir / "manual-take.mkv", error);
    Expect(loaded.has_value(), "Manual recording should persist.");
    if (loaded.has_value()) {
        Expect(loaded->dungeonName.has_value() && *loaded->dungeonName == "Manual Recording",
            "Manual runs without a map id should be labeled Manual Recording.");
        Expect(loaded->result == "unknown", "Manual stop should persist result=unknown.");
    }
}

void TestRunRepositoryListRuns()
{
    const auto dir = MakeTempDir("repo-list");
    bean::core::RunRepository repo(dir / "runs.db");
    std::string error;
    Expect(repo.Initialize(error), "ListRuns repo should initialize.");

    for (int i = 0; i < 3; ++i) {
        bean::core::RunRecord record;
        record.videoPath = dir / ("clip-" + std::to_string(i) + ".mkv");
        record.videoFileName = record.videoPath.filename().string();
        record.triggerReason = "manual";
        record.stopReason = "manual";
        record.result = "unknown";
        record.recordingStartedAt = std::chrono::system_clock::from_time_t(1700000000 + i);
        record.recordingEndedAt = record.recordingStartedAt + std::chrono::seconds(30);
        Expect(repo.UpsertRun(record, error), "ListRuns seed upsert should succeed.");
    }

    const auto all = repo.ListRuns(error);
    Expect(error.empty(), "ListRuns should leave empty error.");
    Expect(all.size() == 3, "ListRuns should return every stored run.");
}

void TestClipExportServiceJournalsRequests()
{
    const auto dir = MakeTempDir("clip-export-journal");
    bean::core::ClipExportService service;
    service.SetJournalDirectory(dir);
    service.SetClipDurationSeconds(20);

    std::vector<std::string> statuses;
    service.SetStatusCallback([&](const std::string& status) { statuses.push_back(status); });

    const auto videoPath = dir / "run.mkv";
    {
        std::ofstream video(videoPath, std::ios::trunc | std::ios::binary);
        video << "fake";
    }

    const auto startedAt = std::chrono::system_clock::now() - std::chrono::minutes(2);
    const auto startedSteady = std::chrono::steady_clock::now() - std::chrono::minutes(2);
    std::string error;
    Expect(service.RequestClip(videoPath, startedAt, startedSteady, error),
        "RequestClip should succeed while journaling.");
    Expect(error.empty(), "RequestClip success should leave empty error.");

    const auto journalPath = dir / "bean-clip-journal.log";
    Expect(std::filesystem::exists(journalPath), "Clip journal file should be created.");
    {
        std::ifstream in(journalPath);
        std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Expect(contents.find("REQ ") != std::string::npos, "Journal should contain a REQ line.");
        Expect(contents.find("run.mkv") != std::string::npos, "Journal should mention the video path.");
    }
    Expect(!statuses.empty() && statuses.back().find("Clip requested") != std::string::npos,
        "RequestClip should push a status line.");

    // Stop without waiting for ffmpeg: OnRecordingStopped may queue work that fails without ffmpeg,
    // but the service must shut down cleanly.
    service.Stop();
}

void TestSettingsEncoderPresetMigrationAndKeybinds()
{
    const auto appData = MakeTempDir("settings-preset-appdata");
    _putenv_s("APPDATA", appData.string().c_str());

    bean::core::SettingsStore store;
    bean::core::AppSettings settings;
    settings.outputDirectory = MakeTempDir("settings-preset-out");
    settings.wowLogDirectory = MakeTempDir("settings-preset-logs");
    settings.encoderPreset = "balanced"; // legacy name -> medium
    settings.audioCaptureScope = bean::core::AppSettings::AudioCaptureScope::WowAndDiscord;
    settings.microphoneNoiseSuppression = true;
    settings.clipDurationSeconds = 45;
    settings.clipKeybind = {6, 0x70};
    settings.manualStartKeybind = {6, 0x71};
    settings.manualStopKeybind = {6, 0x72};

    std::string error;
    Expect(store.Save(settings, error), "Preset settings save should succeed.");

    bean::core::AppSettings loaded;
    Expect(store.Load(loaded, error), "Preset settings load should succeed.");
    Expect(loaded.encoderPreset == "medium", "Legacy balanced preset should migrate to medium.");
    Expect(loaded.audioCaptureScope == bean::core::AppSettings::AudioCaptureScope::WowAndDiscord,
        "Audio capture scope should round-trip.");
    Expect(loaded.microphoneNoiseSuppression, "Mic noise suppression should round-trip.");
    Expect(loaded.clipDurationSeconds == 45, "Clip duration should round-trip.");
    Expect(loaded.clipKeybind.virtualKey == 0x70, "Clip keybind should round-trip.");
    Expect(loaded.manualStartKeybind.virtualKey == 0x71, "Manual start keybind should round-trip.");
    Expect(loaded.manualStopKeybind.virtualKey == 0x72, "Manual stop keybind should round-trip.");
}

void TestSettingsLegacyWowOnlyAudioFlag()
{
    const auto appData = MakeTempDir("settings-legacy-audio-appdata");
    _putenv_s("APPDATA", appData.string().c_str());
    bean::core::SettingsStore store;

    // Write a legacy-shaped config by hand (no audioCaptureScope key).
    std::filesystem::create_directories(store.GetConfigPath().parent_path());
    {
        std::ofstream out(store.GetConfigPath(), std::ios::trunc);
        out << "{\n"
            << "  \"schemaVersion\": 1,\n"
            << "  \"outputDirectory\": \"C:/Videos\",\n"
            << "  \"wowLogDirectory\": \"C:/Logs\",\n"
            << "  \"captureWowProcessAudioOnly\": false,\n"
            << "  \"videoEncoder\": \"gpu_auto\",\n"
            << "  \"encoderPreset\": \"high\",\n"
            << "  \"videoContainer\": \"mkv\",\n"
            << "  \"captureMicrophone\": false,\n"
            << "  \"microphoneNoiseSuppression\": false,\n"
            << "  \"microphoneDeviceId\": \"default\",\n"
            << "  \"windowWidth\": 960,\n"
            << "  \"windowHeight\": 560,\n"
            << "  \"recordingResolutionHeight\": 0,\n"
            << "  \"fps\": 60,\n"
            << "  \"postRunStopDelaySeconds\": 30,\n"
            << "  \"clipDurationSeconds\": 30,\n"
            << "  \"clipKeybindModifiers\": 6,\n"
            << "  \"clipKeybindVirtualKey\": 119,\n"
            << "  \"manualStartKeybindModifiers\": 6,\n"
            << "  \"manualStartKeybindVirtualKey\": 120,\n"
            << "  \"manualStopKeybindModifiers\": 6,\n"
            << "  \"manualStopKeybindVirtualKey\": 121,\n"
            << "  \"chatBlockerEnabled\": true,\n"
            << "  \"chatBlockerUseCustomImage\": false,\n"
            << "  \"chatBlockerCustomImagePath\": \"\",\n"
            << "  \"chatBlockerCustomImageSourceWidth\": 0,\n"
            << "  \"chatBlockerCustomImageSourceHeight\": 0,\n"
            << "  \"chatBlockerCustomImageSizesByFileName\": \"\",\n"
            << "  \"chatBlockerWidth\": 500,\n"
            << "  \"chatBlockerHeight\": 300,\n"
            << "  \"chatBlockerAnchor\": \"bottom_left\",\n"
            << "  \"youtubeClientId\": \"\",\n"
            << "  \"youtubeRefreshToken\": \"\",\n"
            << "  \"youtubeChannelId\": \"\",\n"
            << "  \"youtubeChannelTitle\": \"\"\n"
            << "}\n";
    }

    bean::core::AppSettings loaded;
    std::string error;
    Expect(store.Load(loaded, error), "Legacy audio config should load.");
    Expect(loaded.audioCaptureScope == bean::core::AppSettings::AudioCaptureScope::AllDesktop,
        "captureWowProcessAudioOnly=false should map to AllDesktop.");
}

void TestMockInitializeFailureAndDoubleStart()
{
    bean::obs::MockRecorderEngine engine;
    std::string error;
    bean::obs::RecordingConfig config;
    config.outputDirectory = MakeTempDir("mock-init-fail");

    engine.SetFailNextInitialize("disk full");
    Expect(!engine.Initialize(config, error), "Injected initialize failure should fail.");
    Expect(error.find("disk full") != std::string::npos, "Initialize failure should surface message.");

    Expect(engine.Initialize(config, error), "Initialize after clearing injected failure should succeed.");
    Expect(engine.StartRecording("a", error), "First start should succeed.");
    Expect(!engine.StartRecording("b", error), "Second start while recording should fail.");
    Expect(error.find("already") != std::string::npos, "Double-start error should mention already recording.");
    Expect(engine.StopRecording(error), "Stop should succeed.");
    Expect(!engine.StopRecording(error), "Stop while idle should fail.");
}

void TestOrchestratorRejectsClipWhenIdle()
{
    auto engine = std::make_unique<bean::obs::MockRecorderEngine>();
    bean::core::RecordingOrchestrator orchestrator(std::move(engine));
    bean::core::AppSettings settings;
    settings.outputDirectory = MakeTempDir("orch-clip-idle-out");
    settings.wowLogDirectory = MakeTempDir("orch-clip-idle-logs");
    orchestrator.ApplySettings(settings);

    std::string error;
    Expect(!orchestrator.RequestClip(error), "Clip request should fail when not recording.");
    Expect(error.find("recording") != std::string::npos, "Idle clip error should mention recording.");
}

void TestOrchestratorClipRequestWhileRecording()
{
    auto engine = std::make_unique<bean::obs::MockRecorderEngine>();
    bean::core::RecordingOrchestrator orchestrator(std::move(engine));
    bean::core::AppSettings settings;
    settings.outputDirectory = MakeTempDir("orch-clip-live-out");
    settings.wowLogDirectory = MakeTempDir("orch-clip-live-logs");
    settings.clipDurationSeconds = 15;
    settings.videoContainer = "mkv";
    orchestrator.ApplySettings(settings);

    auto repo = std::make_shared<bean::core::RunRepository>(
        settings.outputDirectory.parent_path() / "runs.db");
    std::string error;
    Expect(repo->Initialize(error), "Clip orchestrator repo should initialize.");
    orchestrator.SetRunRepository(repo);

    std::vector<std::string> statuses;
    std::mutex statusMutex;
    orchestrator.SetStatusCallback([&](const std::string& status) {
        std::scoped_lock lock(statusMutex);
        statuses.push_back(status);
    });

    Expect(orchestrator.StartManualRecording(error), "Manual recording should start for clip test.");
    Expect(orchestrator.RequestClip(error), "Clip request should succeed while recording.");
    Expect(error.empty(), "Successful clip request should leave empty error.");

    const bool sawClipStatus = [&]() {
        std::scoped_lock lock(statusMutex);
        return std::any_of(statuses.begin(), statuses.end(), [](const std::string& status) {
            return status.find("Clip requested") != std::string::npos;
        });
    }();
    Expect(sawClipStatus, "Clip request should push a status message.");

    Expect(orchestrator.StopManualRecording(error), "Manual recording should stop after clip request.");
}

void TestRecordingPathCaseInsensitiveContainer()
{
    const auto path = bean::core::BuildRecordingPath("D:/Videos", "stem", "MP4");
    Expect(path.extension() == ".mp4", "Container matching should be case-insensitive.");
}

void TestEnumerateDriveRootsNonEmpty()
{
    const auto roots = bean::core::EnumerateDriveRoots();
    Expect(!roots.empty(), "EnumerateDriveRoots should find at least one drive on Windows.");
}

void TestJsonReadersIgnoreMalformedValues()
{
    using bean::util::ReadJsonBool;
    using bean::util::ReadJsonInt;
    using bean::util::ReadJsonString;

    const std::string doc =
        "{\n"
        "  \"name\": \"ok\",\n"
        "  \"count\": not-a-number,\n"
        "  \"flag\": maybe\n"
        "}\n";

    Expect(ReadJsonString(doc, "name") == "ok", "Valid string should still parse beside bad neighbors.");
    Expect(ReadJsonInt(doc, "count", 11) == 11, "Non-numeric int should fall back.");
    Expect(ReadJsonBool(doc, "flag", true), "Non-bool value should fall back.");
}

void TestEscapeJsonControlCharacters()
{
    using bean::util::EscapeJson;
    Expect(EscapeJson("a\rb") == "a\\rb", "CR should escape to \\\\r.");
    const std::string withNul("x\0y", 3);
    Expect(EscapeJson(withNul).size() >= 3, "EscapeJson should preserve length across embedded NUL.");
}

void TestDungeonNameTableSeasonCoverage()
{
    Expect(bean::core::DungeonNameForChallengeMap(239) == "Seat of the Triumvirate", "Map 239 should resolve.");
    Expect(bean::core::DungeonNameForChallengeMap(556) == "Pit of Saron", "Map 556 should resolve.");
    Expect(bean::core::DungeonNameForChallengeMap(557) == "Windrunner Spire", "Map 557 should resolve.");
    Expect(bean::core::DungeonNameForChallengeMap(558) == "Magisters' Terrace", "Map 558 should resolve.");
    Expect(bean::core::DungeonNameForChallengeMap(559) == "Nexus-Point Xenas", "Map 559 should resolve.");
}

void TestKeybindIsBound()
{
    bean::core::Keybind unbound{};
    Expect(!unbound.IsBound(), "Default keybind should be unbound.");
    bean::core::Keybind bound{6, 0x70};
    Expect(bound.IsBound(), "Non-zero virtual key should count as bound.");
}

void TestSettingsClampsOutOfRangeValues()
{
    const auto appData = MakeTempDir("settings-clamp-appdata");
    _putenv_s("APPDATA", appData.string().c_str());
    bean::core::SettingsStore store;
    std::filesystem::create_directories(store.GetConfigPath().parent_path());
    {
        std::ofstream out(store.GetConfigPath(), std::ios::trunc);
        out << "{\n"
            << "  \"schemaVersion\": 1,\n"
            << "  \"outputDirectory\": \"C:/Videos\",\n"
            << "  \"wowLogDirectory\": \"C:/Logs\",\n"
            << "  \"videoEncoder\": \"gpu_auto\",\n"
            << "  \"encoderPreset\": \"quality\",\n"
            << "  \"videoContainer\": \"avi\",\n"
            << "  \"audioCaptureScope\": \"wow_only\",\n"
            << "  \"captureMicrophone\": false,\n"
            << "  \"microphoneNoiseSuppression\": false,\n"
            << "  \"microphoneDeviceId\": \"default\",\n"
            << "  \"windowWidth\": 100,\n"
            << "  \"windowHeight\": 50,\n"
            << "  \"recordingResolutionHeight\": -5,\n"
            << "  \"fps\": 60,\n"
            << "  \"postRunStopDelaySeconds\": 9999,\n"
            << "  \"clipDurationSeconds\": 0,\n"
            << "  \"clipKeybindModifiers\": 6,\n"
            << "  \"clipKeybindVirtualKey\": 119,\n"
            << "  \"manualStartKeybindModifiers\": 6,\n"
            << "  \"manualStartKeybindVirtualKey\": 120,\n"
            << "  \"manualStopKeybindModifiers\": 6,\n"
            << "  \"manualStopKeybindVirtualKey\": 121,\n"
            << "  \"chatBlockerEnabled\": true,\n"
            << "  \"chatBlockerUseCustomImage\": false,\n"
            << "  \"chatBlockerCustomImagePath\": \"\",\n"
            << "  \"chatBlockerCustomImageSourceWidth\": 0,\n"
            << "  \"chatBlockerCustomImageSourceHeight\": 0,\n"
            << "  \"chatBlockerCustomImageSizesByFileName\": \"\",\n"
            << "  \"chatBlockerWidth\": 500,\n"
            << "  \"chatBlockerHeight\": 300,\n"
            << "  \"chatBlockerAnchor\": \"top_right\",\n"
            << "  \"youtubeClientId\": \"\",\n"
            << "  \"youtubeRefreshToken\": \"\",\n"
            << "  \"youtubeChannelId\": \"\",\n"
            << "  \"youtubeChannelTitle\": \"\"\n"
            << "}\n";
    }

    bean::core::AppSettings loaded;
    std::string error;
    Expect(store.Load(loaded, error), "Clamp settings config should load.");
    Expect(loaded.encoderPreset == "high", "Legacy quality preset should migrate to high.");
    Expect(loaded.videoContainer == "mp4", "Invalid container should keep the default mp4.");
    Expect(loaded.windowWidth == bean::core::kDefaultWindowWidth, "Tiny windowWidth should clamp to default.");
    Expect(loaded.windowHeight == bean::core::kDefaultWindowHeight, "Tiny windowHeight should clamp to default.");
    Expect(loaded.recordingResolutionHeight == bean::core::kDefaultRecordingResolutionHeight,
        "Negative recordingResolutionHeight should reset to default.");
    Expect(loaded.postRunStopDelaySeconds == 30, "Huge postRunStopDelaySeconds should clamp to default.");
    Expect(loaded.clipDurationSeconds == 30, "Zero clipDurationSeconds should clamp to default.");
    Expect(loaded.chatBlockerAnchor == bean::core::AppSettings::ChatBlockerAnchor::TopRight,
        "top_right anchor should parse.");
}

void TestSettingsEncoderPresetLegacyAliases()
{
    const auto appData = MakeTempDir("settings-preset-aliases-appdata");
    _putenv_s("APPDATA", appData.string().c_str());

    auto roundTripPreset = [](const char* input, const char* expected, const char* label) {
        bean::core::SettingsStore store;
        bean::core::AppSettings settings;
        settings.outputDirectory = MakeTempDir(std::string("preset-alias-out-") + label);
        settings.wowLogDirectory = MakeTempDir(std::string("preset-alias-logs-") + label);
        settings.encoderPreset = input;
        std::string error;
        Expect(store.Save(settings, error), std::string("Save should succeed for ") + label);
        bean::core::AppSettings loaded;
        Expect(store.Load(loaded, error), std::string("Load should succeed for ") + label);
        Expect(loaded.encoderPreset == expected, std::string(label) + " should normalize to " + expected);
    };

    roundTripPreset("speed", "low", "speed");
    roundTripPreset("ultra", "ultra", "ultra");
    roundTripPreset("minimum", "minimum", "minimum");
    roundTripPreset("nope", "high", "unknown");
}

void TestToRecordingConfigMapsAudioAndChatAnchor()
{
    bean::core::AppSettings settings;
    settings.videoEncoder = "x264";
    settings.encoderPreset = "low";
    settings.videoContainer = "mkv";
    settings.audioCaptureScope = bean::core::AppSettings::AudioCaptureScope::WowAndDiscord;
    settings.captureMicrophone = true;
    settings.microphoneNoiseSuppression = true;
    settings.microphoneDeviceId = "mic-a";
    settings.recordingResolutionHeight = 1080;
    settings.detectedWowClientWidth = 2560;
    settings.detectedWowClientHeight = 1440;
    settings.fps = 144;
    settings.chatBlockerEnabled = false;
    settings.chatBlockerAnchor = bean::core::AppSettings::ChatBlockerAnchor::TopLeft;
    settings.chatBlockerWidth = 400;
    settings.chatBlockerHeight = 200;

    const auto config = bean::core::ToRecordingConfig(settings);
    Expect(config.videoEncoder == "x264", "ToRecordingConfig should map videoEncoder.");
    Expect(config.encoderPreset == "low", "ToRecordingConfig should map encoderPreset.");
    Expect(config.containerFormat == "mkv", "ToRecordingConfig should map container.");
    Expect(config.audioCaptureScope == bean::obs::RecordingConfig::AudioCaptureScope::WowAndDiscord,
        "ToRecordingConfig should map WowAndDiscord scope.");
    Expect(config.captureMicrophone && config.microphoneNoiseSuppression, "Mic flags should map.");
    Expect(config.microphoneDeviceId == "mic-a", "Mic device id should map.");
    Expect(config.fps == 144, "FPS should map.");
    Expect(!config.chatBlockerEnabled, "Chat blocker enabled flag should map.");
    Expect(config.chatBlockerAnchor == bean::obs::RecordingConfig::ChatBlockerAnchor::TopLeft,
        "Chat blocker anchor should map.");
    Expect(config.chatBlockerWidth == 400 && config.chatBlockerHeight == 200, "Chat blocker size should map.");
}

void TestRunRepositoryMissingPathReturnsEmpty()
{
    const auto dir = MakeTempDir("repo-missing");
    bean::core::RunRepository repo(dir / "runs.db");
    std::string error;
    Expect(repo.Initialize(error), "Missing-path repo should initialize.");
    const auto missing = repo.GetRunByVideoPath(dir / "nope.mkv", error);
    Expect(!missing.has_value(), "Unknown video path should return empty optional.");
    Expect(error.empty(), "Missing run lookup should not be an error.");
}

void TestRecordingPathDefaultsUnknownContainerToMkv()
{
    const auto path = bean::core::BuildRecordingPath("C:/out", "stem", "webm");
    Expect(path.filename() == "stem.mkv", "Unknown container should fall back to .mkv.");
    Expect(path.parent_path() == "C:/out" || path.string().find("out") != std::string::npos,
        "Output directory should be preserved in recording path.");
}

void TestOrchestratorRejectsDoubleManualStart()
{
    auto engine = std::make_unique<bean::obs::MockRecorderEngine>();
    bean::core::RecordingOrchestrator orchestrator(std::move(engine));

    bean::core::AppSettings settings;
    settings.outputDirectory = MakeTempDir("orch-double-start-out");
    settings.wowLogDirectory = MakeTempDir("orch-double-start-logs");
    settings.videoContainer = "mkv";
    orchestrator.ApplySettings(settings);

    std::string error;
    Expect(orchestrator.StartManualRecording(error), "First manual start should succeed.");
    Expect(orchestrator.GetState() == bean::core::OrchestratorState::Recording,
        "State should be Recording after first manual start.");
    Expect(!orchestrator.StartManualRecording(error), "Second manual start should fail while recording.");
    Expect(orchestrator.StopManualRecording(error), "Manual stop should succeed after double-start attempt.");
    Expect(orchestrator.GetState() != bean::core::OrchestratorState::Recording,
        "State should leave Recording after manual stop.");
}

void TestOrchestratorPersistsMythicFailureMetadata()
{
    auto engine = std::make_unique<bean::obs::MockRecorderEngine>();
    bean::core::RecordingOrchestrator orchestrator(std::move(engine));

    const auto outputDir = MakeTempDir("orch-fail-meta-out");
    const auto logDir = MakeTempDir("orch-fail-meta-logs");
    const auto logFile = logDir / "WoWCombatLog-020202_020202.txt";
    {
        std::ofstream seed(logFile, std::ios::trunc | std::ios::binary);
        seed << "";
    }

    bean::core::AppSettings settings;
    settings.outputDirectory = outputDir;
    settings.wowLogDirectory = logDir;
    settings.postRunStopDelaySeconds = 0;
    settings.videoContainer = "mkv";
    orchestrator.ApplySettings(settings);

    auto repo = std::make_shared<bean::core::RunRepository>(MakeTempDir("orch-fail-meta-repo") / "runs.db");
    std::string error;
    Expect(repo->Initialize(error), "Failure-metadata repo should initialize.");
    orchestrator.SetRunRepository(repo);

    Expect(orchestrator.StartMonitoring(error), "Monitoring should start for failure-metadata test.");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    {
        std::ofstream append(logFile, std::ios::app | std::ios::binary);
        append << "6/20/2026 00:00:00.000-7  CHALLENGE_MODE_START,\"Skyreach\",161,161,12,[1]\n";
    }

    const auto recordingStarted = [&]() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            orchestrator.Tick();
            if (orchestrator.GetState() == bean::core::OrchestratorState::Recording) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }();
    Expect(recordingStarted, "Mythic start should begin recording for failure-metadata test.");

    {
        std::ofstream append(logFile, std::ios::app | std::ios::binary);
        append << "6/20/2026 00:30:00.000-7  CHALLENGE_MODE_END,161,1,12,2000000.000000,-120.000000,1830.000000\n";
    }

    const auto returnedToArmed = [&]() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            orchestrator.Tick();
            if (orchestrator.GetState() == bean::core::OrchestratorState::Armed) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }();
    Expect(returnedToArmed, "Overtime end should stop recording and return to Armed.");

    orchestrator.StopMonitoring();

    const auto runs = repo->ListRuns(error);
    Expect(!runs.empty(), "Failure-metadata run should be persisted.");
    if (!runs.empty()) {
        Expect(runs.front().result == "failure", "Overtime mythic end should persist result=failure.");
        Expect(runs.front().challengeMapId.has_value() && *runs.front().challengeMapId == 161,
            "Persisted failure run should keep challenge map id.");
        Expect(runs.front().dungeonName.has_value() && *runs.front().dungeonName == "Skyreach",
            "Persisted failure run should resolve dungeon name.");
    }
}

} // namespace

int main()
{
    TestJsonEscapeRoundTrip();
    TestJsonReaders();
    TestJsonReadersIgnoreMalformedValues();
    TestEscapeJsonControlCharacters();
    TestStringConversions();
    TestDungeonNameTable();
    TestDungeonNameTableSeasonCoverage();
    TestEncoderPresetQualityValues();
    TestAudioCaptureScopeLabels();
    TestKeybindIsBound();
    TestWowInstallPathResolution();
    TestAdvancedCombatLoggingConfigParse();
    TestRunMetadataWriterPersistsSuccess();
    TestRunMetadataWriterManualFallbackAndMissingRepo();
    TestRunRepositoryListRuns();
    TestRunRepositoryMissingPathReturnsEmpty();
    TestClipExportServiceJournalsRequests();
    TestSettingsEncoderPresetMigrationAndKeybinds();
    TestSettingsEncoderPresetLegacyAliases();
    TestSettingsLegacyWowOnlyAudioFlag();
    TestSettingsClampsOutOfRangeValues();
    TestToRecordingConfigMapsAudioAndChatAnchor();
    TestMockInitializeFailureAndDoubleStart();
    TestOrchestratorRejectsClipWhenIdle();
    TestOrchestratorClipRequestWhileRecording();
    TestOrchestratorRejectsDoubleManualStart();
    TestOrchestratorPersistsMythicFailureMetadata();
    TestRecordingPathCaseInsensitiveContainer();
    TestRecordingPathDefaultsUnknownContainerToMkv();
    TestEnumerateDriveRootsNonEmpty();

    if (gFailures == 0) {
        std::cout << "All core logic tests passed.\n";
        return 0;
    }
    std::cerr << gFailures << " test(s) failed.\n";
    return 1;
}
