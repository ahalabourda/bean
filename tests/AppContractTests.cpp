#include "app/AppContext.h"
#include "app/AppLiveStatus.h"
#include "app/AppRecordingHelpers.h"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int gFailures = 0;

void Expect(bool condition, const std::string& message)
{
    if (!condition) {
        ++gFailures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct TrackedPayload {
    static inline int destructions = 0;

    ~TrackedPayload()
    {
        ++destructions;
    }
};

void TestOwnedMessageRejectsAndDeletesWithoutWindow()
{
    TrackedPayload::destructions = 0;
    AppContext context;

    Expect(
        !PostOwnedAppMessage(&context, WM_APP + 200, new TrackedPayload()),
        "Owned app messages should fail when no target window exists.");
    Expect(
        TrackedPayload::destructions == 1,
        "Rejected owned app messages must delete their payload.");

    context.shuttingDown.store(true);
    Expect(
        !PostOwnedAppMessage(&context, WM_APP + 201, new TrackedPayload()),
        "Owned app messages should fail after shutdown begins.");
    Expect(
        TrackedPayload::destructions == 2,
        "Shutdown-rejected owned app messages must delete their payload.");
}

void TestWorkerAdmissionAndJoining()
{
    AppContext context;
    std::atomic<bool> ran{false};

    Expect(
        LaunchAppWorker(&context, [&ran]() { ran.store(true); }),
        "Live contexts should admit app workers.");
    JoinAppWorkers(&context);
    Expect(ran.load(), "JoinAppWorkers should wait for admitted work to finish.");

    context.shuttingDown.store(true);
    Expect(
        !LaunchAppWorker(&context, []() {}),
        "Shutting-down contexts must reject new app workers.");
}

void TestRecordingPathHelpers()
{
    AppContext context;
    context.settings.outputDirectory = std::filesystem::path(L"C:/Bean/Recordings");

    Expect(
        ResolveRecordingsFolderPath(&context) == std::filesystem::path(L"C:/Bean/Recordings"),
        "Recording folder resolution should fall back to saved settings.");
    Expect(
        ResolveClipsOutputFolderPath(&context)
            == std::filesystem::path(L"C:/Bean/Recordings/Clips"),
        "Clips output should be rooted under the recording folder.");

    std::vector<std::filesystem::path> folders;
    AddKnownRecordingFolder(folders, L"C:/Bean/Recordings");
    AddKnownRecordingFolder(folders, L"c:/bean/recordings");
    AddKnownRecordingFolder(folders, L"C:/Bean/Other");
    Expect(folders.size() == 2, "Known recording folders should deduplicate case-insensitively.");
    Expect(
        RecordingPathKey(L"C:/Bean/Recordings/Run.MKV")
            == RecordingPathKey(L"c:/bean/recordings/run.mkv"),
        "Recording path keys should be case-insensitive.");
    Expect(
        RecordingFileNameKey(L"C:/Bean/Run.MKV") == RecordingFileNameKey(L"run.mkv"),
        "Recording filename keys should ignore parent folders and case.");
}

void TestClipTimeFormatting()
{
    Expect(FormatClipTimeMs(0) == L"00:00:00", "Zero clip time should format as zero.");
    Expect(FormatClipTimeMs(-1) == L"00:00:00", "Negative clip time should clamp to zero.");
    Expect(FormatClipTimeMs(3'723'000) == L"01:02:03", "Clip time should format hours, minutes, and seconds.");
}

} // namespace

int main()
{
    TestOwnedMessageRejectsAndDeletesWithoutWindow();
    TestWorkerAdmissionAndJoining();
    TestRecordingPathHelpers();
    TestClipTimeFormatting();

    if (gFailures == 0) {
        std::cout << "All app contract tests passed.\n";
        return 0;
    }
    std::cerr << gFailures << " test(s) failed.\n";
    return 1;
}
