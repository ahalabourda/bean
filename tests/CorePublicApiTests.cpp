#include "core/RecordingOrchestrator.h"
#include "core/RunRepository.h"
#include "core/SettingsStore.h"
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
int gSkips = 0;

void Expect(bool condition, const std::string& message)
{
    if (!condition) {
        ++gFailures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void Skip(const std::string& message)
{
    ++gSkips;
    std::cout << "SKIP: " << message << '\n';
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
    saveSettings.wowLogDirectory = MakeTempDir("settings-logs");
    saveSettings.videoEncoder = "nvenc";
    saveSettings.encoderPreset = "balanced";
    saveSettings.videoContainer = "mp4";
    saveSettings.captureMicrophone = true;
    saveSettings.microphoneDeviceId = "line-in";
    saveSettings.width = 2560;
    saveSettings.height = 1440;
    saveSettings.fps = 120;
    saveSettings.postRunStopDelaySeconds = 45;
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
    Expect(loaded.width == 2560 && loaded.height == 1440, "Load should restore resolution.");
    Expect(loaded.fps == 120, "Load should restore fps.");
    Expect(loaded.postRunStopDelaySeconds == 45, "Load should restore postRunStopDelaySeconds.");
    Expect(loaded.chatBlockerAnchor == bean::core::AppSettings::ChatBlockerAnchor::TopRight, "Load should restore chatBlockerAnchor.");
    Expect(loaded.youtubeClientId == "youtube-client-id", "Load should restore YouTube client ID.");
    Expect(loaded.youtubeRefreshToken == "refresh-token", "Load should restore YouTube refresh token.");

    const auto recordingConfig = bean::core::ToRecordingConfig(loaded);
    Expect(recordingConfig.videoEncoder == "nvenc", "ToRecordingConfig should map videoEncoder.");
    Expect(recordingConfig.containerFormat == "mp4", "ToRecordingConfig should map container format.");
    Expect(recordingConfig.captureMicrophone, "ToRecordingConfig should map captureMicrophone.");
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
        Expect(loaded->participants.size() == 2, "Loaded run should include participants.");
        if (!loaded->participants.empty()) {
            const auto& p = loaded->participants.front();
            Expect(p.specName.has_value(), "Loaded participant should include resolved specName.");
            Expect(p.className.has_value(), "Loaded participant should include resolved className.");
        }
    }

    record.result = "failure";
    record.participants = {{"Player-9", "Zulu", "Stormrage", std::nullopt, 62, "Arcane", "Mage"}};
    const bool upsertedAgain = repo.UpsertRun(record, error);
    Expect(upsertedAgain, "RunRepository::UpsertRun should overwrite existing row.");
    const auto loadedAgain = repo.GetRunByVideoPath(record.videoPath, error);
    Expect(loadedAgain.has_value(), "GetRunByVideoPath should still find upserted row.");
    if (loadedAgain.has_value()) {
        Expect(loadedAgain->result == "failure", "Upsert should update result.");
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

void TestRecordingOrchestratorPublicMethods()
{
    auto engine = std::make_unique<bean::obs::MockRecorderEngine>();
    bean::core::RecordingOrchestrator orchestrator(std::move(engine));

    auto outputDir = MakeTempDir("orchestrator-output");
    auto logDir = MakeTempDir("orchestrator-logs");
    const auto logFile = logDir / "WoWCombatLog-010101_010101.txt";
    {
        std::ofstream seed(logFile, std::ios::trunc | std::ios::binary);
        seed << "";
    }

    bean::core::AppSettings settings;
    settings.outputDirectory = outputDir;
    settings.wowLogDirectory = logDir;
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

    AppendLine(logFile, "CHALLENGE_MODE_START");
    const bool recordingStarted = WaitUntil([&]() {
        orchestrator.Tick();
        return orchestrator.GetState() == bean::core::OrchestratorState::Recording && orchestrator.GetRecordingSessionId() >= 1;
    }, std::chrono::seconds(10), std::chrono::milliseconds(50));
    Expect(recordingStarted, "CHALLENGE_MODE_START should transition orchestrator to Recording.");
    Expect(orchestrator.GetRecordingSessionId() >= 1, "Recording session id should increment after recording starts.");

    AppendLine(logFile, "CHALLENGE_MODE_END");
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

void TestReplaySmokeAgainstRealCombatLog()
{
    const auto replayPathInRepo = std::filesystem::path("tests") / "WoWCombatLog-061926_232002.txt";
    const auto replayPathFromBuild = std::filesystem::path("..") / "tests" / "WoWCombatLog-061926_232002.txt";
    const auto replayPath = std::filesystem::exists(replayPathInRepo) ? replayPathInRepo : replayPathFromBuild;
    if (!std::filesystem::exists(replayPath)) {
        Skip("Replay smoke test skipped (combat log file not found).");
        return;
    }

    std::ifstream input(replayPath, std::ios::binary);
    if (!input.is_open()) {
        Skip("Replay smoke test skipped (unable to open combat log file).");
        return;
    }

    bean::log::MythicRunDetector detector;
    std::string line;
    int runStartedCount = 0;
    int runEndedCount = 0;
    size_t linesRead = 0;
    constexpr size_t kMaxLines = 250000;

    while (linesRead < kMaxLines && std::getline(input, line)) {
        ++linesRead;
        const auto event = detector.ProcessLine(line);
        if (!event.has_value()) {
            continue;
        }
        if (event->type == bean::log::MythicEventType::RunStarted) {
            ++runStartedCount;
        } else if (event->type == bean::log::MythicEventType::RunEndedSuccess || event->type == bean::log::MythicEventType::RunEndedFailure) {
            ++runEndedCount;
        }
        if (runStartedCount >= 1 && runEndedCount >= 1) {
            break;
        }
    }

    Expect(linesRead > 0, "Replay smoke test should read at least one line.");
    Expect(runStartedCount >= 1, "Replay smoke test should detect at least one RunStarted event.");
}

} // namespace

int main()
{
    TestResolveSpecInfo();
    TestMockRecorderEnginePublicMethods();
    TestSettingsStoreLoadSaveAndConversion();
    TestRunRepositoryPublicMethods();
    TestCombatLogWatcherPublicMethods();
    TestRecordingOrchestratorPublicMethods();
    TestReplaySmokeAgainstRealCombatLog();

    if (gFailures == 0) {
        std::cout << "All core public API tests passed.";
        if (gSkips > 0) {
            std::cout << " (" << gSkips << " skipped)";
        }
        std::cout << '\n';
        return 0;
    }

    std::cerr << gFailures << " test(s) failed.\n";
    return 1;
}
