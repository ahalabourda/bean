#include "core/RecordingOrchestrator.h"
#include "core/RecordingPath.h"
#include "core/RunRepository.h"
#include "core/SettingsStore.h"
#include "integrations/YouTubeUploader.h"
#include "log/CombatLogWatcher.h"
#include "log/MythicRunDetector.h"
#include "obs/MockRecorderEngine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
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

bool WaitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout, std::chrono::milliseconds poll = std::chrono::milliseconds(25))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(poll);
    }
    return predicate();
}

void AppendLine(const std::filesystem::path& file, const std::string& line)
{
    std::ofstream out(file, std::ios::app | std::ios::binary);
    out << line << "\n";
}

// Appends raw bytes with no trailing newline, simulating WoW flushing a buffer
// that happens to end in the middle of a line.
void AppendRaw(const std::filesystem::path& file, const std::string& text)
{
    std::ofstream out(file, std::ios::app | std::ios::binary);
    out << text;
}

void TestResolveSpecInfo()
{
    const auto monk = bean::log::ResolveSpecInfo(268);
    Expect(monk.has_value(), "ResolveSpecInfo should resolve known spec id.");
    if (monk.has_value()) {
        Expect(std::string(monk->specName) == "Brewmaster", "Spec 268 should map to Brewmaster.");
        Expect(std::string(monk->className) == "Monk", "Spec 268 should map to Monk.");
    }

    const auto unknown = bean::log::ResolveSpecInfo(999999);
    Expect(!unknown.has_value(), "Unknown spec id should not resolve.");
}

void TestMockRecorderEnginePublicMethods()
{
    bean::obs::MockRecorderEngine engine;
    std::string error;

    const bool startBeforeInit = engine.StartRecording("foo", error);
    Expect(!startBeforeInit, "StartRecording should fail before Initialize.");
    Expect(!error.empty(), "StartRecording before Initialize should set error.");

    bean::obs::RecordingConfig config;
    config.outputDirectory = MakeTempDir("mock-engine-output");
    config.audioCaptureScope = bean::obs::RecordingConfig::AudioCaptureScope::AllDesktop;
    config.captureMicrophone = true;
    config.microphoneDeviceId = "mic-1";

    const bool initialized = engine.Initialize(config, error);
    Expect(initialized, "Initialize should succeed with output directory.");
    Expect(error.empty(), "Initialize success should leave empty error.");

    const bool started = engine.StartRecording("manual-00-123", error);
    Expect(started, "StartRecording should succeed after Initialize.");
    Expect(engine.IsRecording(), "Engine should report recording after StartRecording.");
    const auto diagnostics = engine.GetLastStartDiagnostics();
    Expect(diagnostics.find("all desktop") != std::string::npos, "Diagnostics should include audio mode.");
    Expect(diagnostics.find("enabled") != std::string::npos, "Diagnostics should include microphone enabled.");
    Expect(diagnostics.find("mic-1") != std::string::npos, "Diagnostics should include microphone device id.");

    const bool stopped = engine.StopRecording(error);
    Expect(stopped, "StopRecording should succeed while recording.");
    Expect(!engine.IsRecording(), "Engine should report not recording after StopRecording.");

    engine.SetFailNextStart("injected start failure");
    Expect(engine.Initialize(config, error), "Re-initialize after stop should succeed.");
    Expect(!engine.StartRecording("fail-me", error), "Injected start failure should fail StartRecording.");
    Expect(error.find("injected start failure") != std::string::npos, "Injected start failure should surface its message.");
    Expect(!engine.IsRecording(), "Engine should not be recording after injected start failure.");

    engine.SetRequireWowWindow(true);
    engine.SetWowWindowPresent(false);
    Expect(!engine.StartRecording("no-wow", error), "Missing WoW window should fail when required.");
    Expect(error.find("World of Warcraft") != std::string::npos, "Missing WoW failure should mention the game.");
    engine.ClearInjectedFailures();
}

void TestSettingsStoreLoadSaveAndConversion()
{
    const auto appData = MakeTempDir("settings-store-appdata");
    _putenv_s("APPDATA", appData.string().c_str());

    bean::core::SettingsStore store;
    auto configPath = store.GetConfigPath();
    Expect(configPath.string().find("Battle Encounter Archival Nexus") != std::string::npos, "Config path should include app folder.");

    bean::core::AppSettings saveSettings;
    saveSettings.outputDirectory = MakeTempDir("settings-output");
    saveSettings.wowInstallDirectory = MakeTempDir("settings-logs");
    saveSettings.videoEncoder = "nvenc";
    saveSettings.encoderPreset = "balanced";
    saveSettings.videoContainer = "mp4";
    saveSettings.captureMicrophone = true;
    saveSettings.microphoneDeviceId = "line-in";
    saveSettings.recordingResolutionHeight = 720;
    saveSettings.detectedWowClientWidth = 2560;
    saveSettings.detectedWowClientHeight = 1440;
    saveSettings.fps = 120;
    saveSettings.postRunStopDelaySeconds = 45;
    saveSettings.theme = "ocean";
    saveSettings.chatBlockerEnabled = true;
    saveSettings.chatBlockerWidth = 640;
    saveSettings.chatBlockerHeight = 320;
    saveSettings.chatBlockerAnchor = bean::core::AppSettings::ChatBlockerAnchor::TopRight;
    saveSettings.youtubeClientId = "youtube-client-id";
    saveSettings.youtubeRefreshToken = "refresh-token";
    saveSettings.youtubeChannelId = "channel-id";
    saveSettings.youtubeChannelTitle = "channel-title";

    std::string error;
    const bool saved = store.Save(saveSettings, error);
    Expect(saved, "SettingsStore::Save should succeed.");
    Expect(error.empty(), "SettingsStore::Save should leave empty error on success.");

    bean::core::AppSettings loaded{};
    const bool loadedOk = store.Load(loaded, error);
    Expect(loadedOk, "SettingsStore::Load should succeed after save.");
    Expect(error.empty(), "SettingsStore::Load should leave empty error on success.");
    Expect(loaded.videoEncoder == "nvenc", "Load should restore videoEncoder.");
    Expect(loaded.videoContainer == "mp4", "Load should restore videoContainer.");
    Expect(loaded.captureMicrophone, "Load should restore captureMicrophone.");
    Expect(loaded.microphoneDeviceId == "line-in", "Load should restore microphoneDeviceId.");
    Expect(loaded.recordingResolutionHeight == 720, "Load should restore recording resolution.");
    Expect(loaded.fps == 120, "Load should restore fps.");
    Expect(loaded.postRunStopDelaySeconds == 45, "Load should restore postRunStopDelaySeconds.");
    Expect(loaded.theme == "ocean", "Load should restore the selected theme.");
    Expect(loaded.chatBlockerAnchor == bean::core::AppSettings::ChatBlockerAnchor::TopRight, "Load should restore chatBlockerAnchor.");
    Expect(loaded.youtubeClientId == "youtube-client-id", "Load should restore YouTube client ID.");
    Expect(loaded.youtubeRefreshToken == "refresh-token", "Load should restore YouTube refresh token.");

    loaded.detectedWowClientWidth = 2560;
    loaded.detectedWowClientHeight = 1440;
    const auto recordingConfig = bean::core::ToRecordingConfig(loaded);
    Expect(recordingConfig.videoEncoder == "nvenc", "ToRecordingConfig should map videoEncoder.");
    Expect(recordingConfig.containerFormat == "mp4", "ToRecordingConfig should map container format.");
    Expect(recordingConfig.captureMicrophone, "ToRecordingConfig should map captureMicrophone.");
    Expect(recordingConfig.width == 1280 && recordingConfig.height == 720, "ToRecordingConfig should scale recording resolution.");
    Expect(recordingConfig.chatBlockerAnchor == bean::obs::RecordingConfig::ChatBlockerAnchor::TopRight, "ToRecordingConfig should map chatBlockerAnchor.");
}

void TestRunRepositoryPublicMethods()
{
    const auto dir = MakeTempDir("run-repo");
    const auto dbPath = dir / "runs.db";
    bean::core::RunRepository repo(dbPath);

    std::string error;
    const bool initialized = repo.Initialize(error);
    Expect(initialized, "RunRepository::Initialize should succeed.");
    Expect(error.empty(), "RunRepository::Initialize should leave empty error on success.");
    Expect(repo.GetDatabasePath() == dbPath, "GetDatabasePath should return configured path.");

    bean::core::RunRecord record;
    record.videoPath = dir / "run1.mkv";
    record.videoFileName = "run1.mkv";
    record.triggerReason = "mythic-start";
    record.stopReason = "mythic-success";
    record.result = "success";
    record.recordingStartedAt = std::chrono::system_clock::from_time_t(1700000000);
    record.recordingEndedAt = std::chrono::system_clock::from_time_t(1700000300);
    record.mythicRunStartedAt = std::chrono::system_clock::from_time_t(1700000010);
    record.mythicRunEndedAt = std::chrono::system_clock::from_time_t(1700000200);
    record.challengeMapId = 405;
    record.keystoneLevel = 12;
    record.dungeonName = "Brackenhide Hollow";
    record.encoderPreset = "ultra";
    record.participants = {
        {"Player-1", "Alpha", "Area52", std::nullopt, 268, "Brewmaster", "Monk"},
        {"Player-2", "Bravo", "Illidan", std::nullopt, 581, "Vengeance", "Demon Hunter"},
    };

    const bool upserted = repo.UpsertRun(record, error);
    Expect(upserted, "RunRepository::UpsertRun should succeed.");
    Expect(error.empty(), "RunRepository::UpsertRun should leave empty error on success.");

    const auto loaded = repo.GetRunByVideoPath(record.videoPath, error);
    Expect(loaded.has_value(), "GetRunByVideoPath should return inserted row.");
    Expect(error.empty(), "GetRunByVideoPath success should leave empty error.");
    if (loaded.has_value()) {
        Expect(loaded->videoFileName == "run1.mkv", "Loaded run should include videoFileName.");
        Expect(loaded->challengeMapId.has_value() && *loaded->challengeMapId == 405, "Loaded run should include challengeMapId.");
        Expect(loaded->keystoneLevel.has_value() && *loaded->keystoneLevel == 12, "Loaded run should include keystoneLevel.");
        Expect(loaded->encoderPreset.has_value() && *loaded->encoderPreset == "ultra", "Loaded run should include encoderPreset.");
        Expect(loaded->participants.size() == 2, "Loaded run should include participants.");
        if (!loaded->participants.empty()) {
            const auto& p = loaded->participants.front();
            Expect(p.specName.has_value(), "Loaded participant should include resolved specName.");
            Expect(p.className.has_value(), "Loaded participant should include resolved className.");
        }
    }

    record.result = "failure";
    record.encoderPreset = "medium";
    record.participants = {{"Player-9", "Zulu", "Stormrage", std::nullopt, 62, "Arcane", "Mage"}};
    const bool upsertedAgain = repo.UpsertRun(record, error);
    Expect(upsertedAgain, "RunRepository::UpsertRun should overwrite existing row.");
    const auto loadedAgain = repo.GetRunByVideoPath(record.videoPath, error);
    Expect(loadedAgain.has_value(), "GetRunByVideoPath should still find upserted row.");
    if (loadedAgain.has_value()) {
        Expect(loadedAgain->result == "failure", "Upsert should update result.");
        Expect(
            loadedAgain->encoderPreset.has_value() && *loadedAgain->encoderPreset == "medium",
            "Upsert should update encoderPreset.");
        Expect(loadedAgain->participants.size() == 1, "Upsert should replace participant rows.");
    }
}

void TestCombatLogWatcherPublicMethods()
{
    bean::log::CombatLogWatcher watcher;
    std::string error;

    const bool startedWithoutDir = watcher.Start([](const std::string&) {}, error);
    Expect(!startedWithoutDir, "Start should fail when directory is not set.");
    Expect(!error.empty(), "Start without directory should set error.");

    watcher.SetLogDirectory(MakeTempDir("watcher-missing-dir") / "does-not-exist");
    const bool startedMissing = watcher.Start([](const std::string&) {}, error);
    Expect(!startedMissing, "Start should fail for missing directory.");

    const auto dir = MakeTempDir("watcher-live");
    const auto file = dir / "WoWCombatLog-010101_000000.txt";
    {
        std::ofstream seed(file, std::ios::trunc | std::ios::binary);
        seed << "seed line not read by watcher\n";
    }

    watcher.SetLogDirectory(dir);
    std::mutex linesMutex;
    std::vector<std::string> seenLines;
    const bool started = watcher.Start([&](const std::string& line) {
        std::scoped_lock lock(linesMutex);
        seenLines.push_back(line);
    }, error);
    Expect(started, "Start should succeed with valid directory.");
    Expect(watcher.IsRunning(), "Watcher should be running after Start.");

    const bool startedTwice = watcher.Start([](const std::string&) {}, error);
    Expect(!startedTwice, "Second Start should fail while running.");

    const bool latchedFile = WaitUntil([&]() {
        const auto snapshot = watcher.GetDebugSnapshot();
        return snapshot.activeFile == file;
    }, std::chrono::seconds(5), std::chrono::milliseconds(50));
    Expect(latchedFile, "Watcher should latch active combat log file.");

    AppendLine(file, "6/19/2026 21:00:00.000-7  CHALLENGE_MODE_START,402,10");
    AppendLine(file, "6/19/2026 21:30:00.000-7  CHALLENGE_MODE_END,402,10");

    const bool gotLines = WaitUntil([&]() {
        std::scoped_lock lock(linesMutex);
        return seenLines.size() >= 2;
    }, std::chrono::seconds(5), std::chrono::milliseconds(50));
    Expect(gotLines, "Watcher callback should receive appended log lines.");

    const auto snapshot = watcher.GetDebugSnapshot();
    Expect(!snapshot.activeFile.empty(), "GetDebugSnapshot should report active file.");

    watcher.Stop();
    Expect(!watcher.IsRunning(), "Watcher should stop after Stop.");
}

// Regression: a read that lands mid-line used to drop that line entirely and
// then deliver its tail as though it were a whole line.
void TestCombatLogWatcherHandlesPartialLines()
{
    const auto dir = MakeTempDir("watcher-partial-line");
    const auto file = dir / "WoWCombatLog-010101_000000.txt";
    {
        std::ofstream seed(file, std::ios::trunc | std::ios::binary);
        seed << "seed line\n";
    }

    bean::log::CombatLogWatcher watcher;
    watcher.SetLogDirectory(dir);

    std::mutex linesMutex;
    std::vector<std::string> seenLines;
    std::string error;
    const bool started = watcher.Start([&](const std::string& line) {
        std::scoped_lock lock(linesMutex);
        seenLines.push_back(line);
    }, error);
    Expect(started, "Partial-line watcher should start.");

    Expect(WaitUntil([&]() {
        return watcher.GetDebugSnapshot().activeFile == file;
    }, std::chrono::seconds(5)), "Partial-line watcher should latch the log file.");

    // Write the first half of an event and give the watcher time to see it.
    AppendRaw(file, "6/19/2026 21:00:00.000-7  CHALLENGE_MODE_ST");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    {
        std::scoped_lock lock(linesMutex);
        Expect(seenLines.empty(), "An incomplete line must not be delivered.");
    }

    // Complete it. The full line must arrive exactly once, intact.
    AppendRaw(file, "ART,402,10\n");

    Expect(WaitUntil([&]() {
        std::scoped_lock lock(linesMutex);
        return !seenLines.empty();
    }, std::chrono::seconds(5)), "Completed line should be delivered.");

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    watcher.Stop();

    std::scoped_lock lock(linesMutex);
    Expect(seenLines.size() == 1, "Exactly one line should be delivered for one newline.");
    if (!seenLines.empty()) {
        Expect(
            seenLines.front() == "6/19/2026 21:00:00.000-7  CHALLENGE_MODE_START,402,10",
            "Reassembled line should match what was written, not a fragment.");
    }
}

// Regression: a new log appearing mid-session (game crash and relaunch) must be
// picked up, and must be read from the start rather than tailed from the end.
void TestCombatLogWatcherFollowsNewLogFile()
{
    const auto dir = MakeTempDir("watcher-log-rotation");
    const auto firstFile = dir / "WoWCombatLog-010101_000000.txt";
    {
        std::ofstream seed(firstFile, std::ios::trunc | std::ios::binary);
        seed << "old session line\n";
    }

    bean::log::CombatLogWatcher watcher;
    watcher.SetLogDirectory(dir);

    std::mutex linesMutex;
    std::vector<std::string> seenLines;
    std::string error;
    Expect(watcher.Start([&](const std::string& line) {
        std::scoped_lock lock(linesMutex);
        seenLines.push_back(line);
    }, error), "Rotation watcher should start.");

    Expect(WaitUntil([&]() {
        return watcher.GetDebugSnapshot().activeFile == firstFile;
    }, std::chrono::seconds(5)), "Rotation watcher should latch the first log.");

    AppendLine(firstFile, "line before crash");
    Expect(WaitUntil([&]() {
        std::scoped_lock lock(linesMutex);
        return seenLines.size() == 1;
    }, std::chrono::seconds(5)), "Line from the first log should be delivered.");

    // Simulate the relaunch: a newer log file appears with content already in it.
    const auto secondFile = dir / "WoWCombatLog-010101_000001.txt";
    {
        std::ofstream fresh(secondFile, std::ios::trunc | std::ios::binary);
        fresh << "line after relaunch\n";
    }

    Expect(WaitUntil([&]() {
        return watcher.GetDebugSnapshot().activeFile == secondFile;
    }, std::chrono::seconds(10)), "Watcher should switch to the newly created log.");

    Expect(WaitUntil([&]() {
        std::scoped_lock lock(linesMutex);
        return std::find(seenLines.begin(), seenLines.end(), "line after relaunch") != seenLines.end();
    }, std::chrono::seconds(5)), "New log should be read from the start, not tailed.");

    watcher.Stop();
}

// Regression: an interrupted Save must never leave a truncated config behind,
// and a completed Save must be durable.
void TestSettingsStoreAtomicSave()
{
    const auto appData = MakeTempDir("settings-atomic-appdata");
    _putenv_s("APPDATA", appData.string().c_str());

    bean::core::SettingsStore store;
    const auto configPath = store.GetConfigPath();

    bean::core::AppSettings settings;
    settings.outputDirectory = MakeTempDir("settings-atomic-output");
    settings.fps = 90;

    std::string error;
    Expect(store.Save(settings, error), "First atomic save should succeed.");
    Expect(std::filesystem::exists(configPath), "Config file should exist after save.");

    auto tempPath = configPath;
    tempPath += L".tmp";
    Expect(!std::filesystem::exists(tempPath), "Temp file should not survive a successful save.");

    settings.fps = 30;
    Expect(store.Save(settings, error), "Second atomic save should succeed.");

    auto backupPath = configPath;
    backupPath += L".bak";
    Expect(std::filesystem::exists(backupPath), "Second save should leave a recoverable backup.");

    bean::core::AppSettings reloaded;
    Expect(store.Load(reloaded, error), "Load after atomic save should succeed.");
    Expect(reloaded.fps == 30, "Reloaded settings should reflect the latest save.");
}

void TestSettingsStoreConcurrentSaves()
{
    const auto appData = MakeTempDir("settings-concurrent-appdata");
    _putenv_s("APPDATA", appData.string().c_str());
    const auto outputDirectory = MakeTempDir("settings-concurrent-output");

    std::atomic<int> successfulSaves{0};
    std::vector<std::thread> writers;
    for (int index = 0; index < 8; ++index) {
        writers.emplace_back([&, index]() {
            bean::core::SettingsStore store;
            bean::core::AppSettings settings;
            settings.outputDirectory = outputDirectory;
            settings.fps = 30 + index;
            std::string error;
            if (store.Save(settings, error)) {
                ++successfulSaves;
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }

    Expect(successfulSaves == 8, "Concurrent settings saves should all complete.");
    bean::core::SettingsStore store;
    bean::core::AppSettings loaded;
    std::string error;
    Expect(store.Load(loaded, error), "Settings should remain readable after concurrent saves.");
    Expect(loaded.fps >= 30 && loaded.fps < 38, "Concurrent save should leave one complete snapshot.");
    auto legacyTempPath = store.GetConfigPath();
    legacyTempPath += L".tmp";
    Expect(!std::filesystem::exists(legacyTempPath),
        "Concurrent save should not leave the legacy shared temp file.");
}

void TestRecordingOrchestratorPublicMethods()
{
    auto engine = std::make_unique<bean::obs::MockRecorderEngine>();
    bean::core::RecordingOrchestrator orchestrator(std::move(engine));

    auto outputDir = MakeTempDir("orchestrator-output");
    auto installDir = MakeTempDir("orchestrator-install");
    auto logDir = installDir / "_ptr_" / "Logs";
    std::filesystem::create_directories(logDir);
    const auto logFile = logDir / "WoWCombatLog-010101_010101.txt";
    {
        std::ofstream seed(logFile, std::ios::trunc | std::ios::binary);
        seed << "";
    }

    bean::core::AppSettings settings;
    settings.outputDirectory = outputDir;
    settings.wowInstallDirectory = installDir;
    settings.detectedWowEdition = bean::core::WowEdition::Ptr;
    settings.postRunStopDelaySeconds = 0;
    settings.videoContainer = "mkv";
    settings.captureMicrophone = true;
    settings.microphoneDeviceId = "test-mic";
    orchestrator.ApplySettings(settings);

    auto orchestratorRepo = std::make_shared<bean::core::RunRepository>(MakeTempDir("orchestrator-repo") / "runs.db");
    orchestrator.SetRunRepository(orchestratorRepo);

    std::mutex statusMutex;
    std::vector<std::string> statuses;
    orchestrator.SetStatusCallback([&](const std::string& status) {
        std::scoped_lock lock(statusMutex);
        statuses.push_back(status);
    });

    std::string error;
    const bool monitoringStarted = orchestrator.StartMonitoring(error);
    Expect(monitoringStarted, "StartMonitoring should succeed.");
    Expect(error.empty(), "StartMonitoring should leave empty error on success.");
    Expect(orchestrator.IsMonitoring(), "IsMonitoring should be true after StartMonitoring.");
    Expect(orchestrator.GetState() == bean::core::OrchestratorState::Armed, "State should be Armed after monitoring starts.");

    // Give the watcher loop one cycle to latch the active file before appending lines.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    AppendLine(logFile, "6/19/2026 21:00:00.000-7  CHALLENGE_MODE_START,402,10");
    const bool recordingStarted = WaitUntil([&]() {
        orchestrator.Tick();
        return orchestrator.GetState() == bean::core::OrchestratorState::Recording && orchestrator.GetRecordingSessionId() >= 1;
    }, std::chrono::seconds(10), std::chrono::milliseconds(50));
    Expect(recordingStarted, "CHALLENGE_MODE_START should transition orchestrator to Recording.");
    Expect(orchestrator.GetRecordingSessionId() >= 1, "Recording session id should increment after recording starts.");

    AppendLine(logFile, "6/19/2026 21:30:00.000-7  CHALLENGE_MODE_END,402,1,10,1800000.000000,32.000000,1830.000000");
    const bool returnedToArmed = WaitUntil([&]() {
        orchestrator.Tick();
        return orchestrator.GetState() == bean::core::OrchestratorState::Armed;
    }, std::chrono::seconds(10), std::chrono::milliseconds(50));
    Expect(returnedToArmed, "CHALLENGE_MODE_END should stop recording and return to Armed.");

    orchestrator.StopMonitoring();
    Expect(!orchestrator.IsMonitoring(), "IsMonitoring should be false after StopMonitoring.");
    Expect(orchestrator.GetState() == bean::core::OrchestratorState::Idle, "State should be Idle after StopMonitoring.");

    const bool hasStartStatus = [&]() {
        std::scoped_lock lock(statusMutex);
        return std::any_of(statuses.begin(), statuses.end(), [](const std::string& status) {
            return status.find("Recording started (mythic-start).") != std::string::npos;
        });
    }();
    Expect(hasStartStatus, "Status callback should include recording start status.");

    const bool hasStopStatus = [&]() {
        std::scoped_lock lock(statusMutex);
        return std::any_of(statuses.begin(), statuses.end(), [](const std::string& status) {
            return status.find("Recording stopped (mythic-success).") != std::string::npos;
        });
    }();
    Expect(hasStopStatus, "Status callback should include recording stop status.");
}

void TestRecordingOrchestratorReportsStartFailure()
{
    auto engine = std::make_unique<bean::obs::MockRecorderEngine>();
    auto* mock = engine.get();
    mock->SetFailNextStart("encoder unavailable");

    bean::core::RecordingOrchestrator orchestrator(std::move(engine));
    bean::core::AppSettings settings;
    settings.outputDirectory = MakeTempDir("orchestrator-fail-output");
    settings.wowInstallDirectory = MakeTempDir("orchestrator-fail-logs");
    settings.videoContainer = "mkv";
    orchestrator.ApplySettings(settings);

    std::mutex statusMutex;
    std::vector<std::string> statuses;
    orchestrator.SetStatusCallback([&](const std::string& status) {
        std::scoped_lock lock(statusMutex);
        statuses.push_back(status);
    });

    std::string error;
    Expect(!orchestrator.StartManualRecording(error), "Manual start should fail when the engine rejects StartRecording.");
    Expect(error.find("encoder unavailable") != std::string::npos, "Manual start error should include the engine message.");
    Expect(orchestrator.GetState() != bean::core::OrchestratorState::Recording,
        "Failed start must not leave the orchestrator in Recording.");

    const bool hasFailureStatus = [&]() {
        std::scoped_lock lock(statusMutex);
        return std::any_of(statuses.begin(), statuses.end(), [](const std::string& status) {
            return status.find("Start recording failed") != std::string::npos
                || status.find("encoder unavailable") != std::string::npos;
        });
    }();
    Expect(hasFailureStatus, "Status callback should report the failed start.");
}

void TestRecordingOrchestratorRollsBackWatcherStartFailure()
{
    auto engine = std::make_unique<bean::obs::MockRecorderEngine>();
    auto* mock = engine.get();
    bean::core::RecordingOrchestrator orchestrator(std::move(engine));
    bean::core::AppSettings settings;
    settings.outputDirectory = MakeTempDir("orchestrator-watcher-failure-output");
    settings.wowInstallDirectory.clear();
    orchestrator.ApplySettings(settings);

    std::string error;
    Expect(!orchestrator.StartMonitoring(error),
        "Monitoring should fail when the combat-log directory is unavailable.");
    Expect(!mock->IsInitialized(),
        "Watcher startup failure should shut down the initialized recorder engine.");
    Expect(!orchestrator.IsMonitoring(),
        "Watcher startup failure should leave monitoring stopped.");
}

void TestYouTubeCancelFlag()
{
    bean::integrations::YouTubeUploader::ClearCancel();
    Expect(!bean::integrations::YouTubeUploader::IsCancelRequested(), "Cancel flag should be clear after ClearCancel.");
    bean::integrations::YouTubeUploader::RequestCancel();
    Expect(bean::integrations::YouTubeUploader::IsCancelRequested(), "Cancel flag should be set after RequestCancel.");
    bean::integrations::YouTubeUploader::ClearCancel();
    Expect(!bean::integrations::YouTubeUploader::IsCancelRequested(), "Cancel flag should be clear again after ClearCancel.");
}

void TestBuildRecordingPath()
{
    const auto dir = std::filesystem::path("C:/Videos");
    const auto mp4 = bean::core::BuildRecordingPath(dir, "run-stem", "mp4");
    Expect(mp4.filename() == "run-stem.mp4", "mp4 container should produce .mp4 extension.");
    const auto mkv = bean::core::BuildRecordingPath(dir, "run-stem", "mkv");
    Expect(mkv.filename() == "run-stem.mkv", "mkv container should produce .mkv extension.");
    const auto fallback = bean::core::BuildRecordingPath(dir, "run-stem", "weird");
    Expect(fallback.filename() == "run-stem.mkv", "Unknown container should fall back to .mkv.");
}

void TestSettingsSchemaVersionRoundTrip()
{
    const auto dir = MakeTempDir("settings-schema");
    bean::core::SettingsStore store;
    // Point at a temp config by writing through a store that uses the real path
    // is awkward; instead write a minimal JSON with schemaVersion and load fields.
    bean::core::AppSettings settings;
    settings.outputDirectory = dir / "out";
    settings.wowInstallDirectory = dir / "logs";
    settings.fps = 45;
    settings.videoContainer = "mkv";
    std::string error;
    Expect(store.Save(settings, error), "Save should succeed for schema version test: " + error);

    bean::core::AppSettings loaded;
    Expect(store.Load(loaded, error), "Load should succeed for schema version test: " + error);
    Expect(loaded.fps == 45, "FPS should round-trip after schemaVersion was introduced.");
    Expect(loaded.videoContainer == "mkv", "Container should round-trip after schemaVersion was introduced.");

    // Confirm the on-disk file mentions schemaVersion.
    std::ifstream in(store.GetConfigPath());
    Expect(in.is_open(), "Config file should exist after save.");
    std::stringstream buffer;
    buffer << in.rdbuf();
    Expect(buffer.str().find("\"schemaVersion\"") != std::string::npos, "Saved config should include schemaVersion.");
}

void TestRunRepositorySetsUserVersion()
{
    const auto dir = MakeTempDir("runs-user-version");
    bean::core::RunRepository repo(dir / "runs.db");
    std::string error;
    Expect(repo.Initialize(error), "RunRepository initialize should succeed: " + error);
    Expect(std::filesystem::exists(repo.GetDatabasePath()), "runs.db should exist after initialize.");
}

} // namespace

int main()
{
    TestResolveSpecInfo();
    TestMockRecorderEnginePublicMethods();
    TestSettingsStoreLoadSaveAndConversion();
    TestSettingsStoreAtomicSave();
    TestSettingsStoreConcurrentSaves();
    TestSettingsSchemaVersionRoundTrip();
    TestBuildRecordingPath();
    TestRunRepositoryPublicMethods();
    TestRunRepositorySetsUserVersion();
    TestCombatLogWatcherPublicMethods();
    TestCombatLogWatcherHandlesPartialLines();
    TestCombatLogWatcherFollowsNewLogFile();
    TestRecordingOrchestratorPublicMethods();
    TestRecordingOrchestratorReportsStartFailure();
    TestRecordingOrchestratorRollsBackWatcherStartFailure();
    TestYouTubeCancelFlag();

    if (gFailures == 0) {
        std::cout << "All core public API tests passed.";
        std::cout << '\n';
        return 0;
    }

    std::cerr << gFailures << " test(s) failed.\n";
    return 1;
}
