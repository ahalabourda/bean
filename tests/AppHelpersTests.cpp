#include "app/AppContext.h"
#include "app/AppRecordingHelpers.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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

void TestFormatElapsed()
{
    Expect(FormatElapsed(std::chrono::seconds(65)) == L"01:05", "Under an hour should be mm:ss.");
    Expect(FormatElapsed(std::chrono::seconds(3661)) == L"61:01", "Over an hour should use total minutes.");
    Expect(FormatElapsed(std::chrono::seconds(0)) == L"00:00", "Zero elapsed should format.");
}

void TestParseClipTime()
{
    int seconds = -1;
    Expect(ParseClipTime(L"01:05", seconds) && seconds == 65, "mm:ss should parse.");
    Expect(ParseClipTime(L"1:05", seconds) && seconds == 65, "m:ss should parse.");
    Expect(ParseClipTime(L"90:00", seconds) && seconds == 5400, "mm:ss may exceed 59 minutes.");
    Expect(ParseClipTime(L"01:01:01", seconds) && seconds == 3661, "hh:mm:ss should parse.");
    Expect(ParseClipTime(L" 02:30 ", seconds) && seconds == 150, "Whitespace should be ignored.");
    Expect(!ParseClipTime(L"1:60", seconds), "Seconds over 59 should fail.");
    Expect(!ParseClipTime(L"1:2:60", seconds), "hh:mm:ss seconds over 59 should fail.");
    Expect(!ParseClipTime(L"1:2:3:4", seconds), "Four parts should fail.");
    Expect(!ParseClipTime(L"65", seconds), "Bare seconds should fail.");
    Expect(!ParseClipTime(L"", seconds), "Empty input should fail.");
    Expect(!ParseClipTime(L"01:", seconds), "Trailing colon should fail.");
}

void TestFormatBytes()
{
    Expect(FormatBytes(512) == L"512 B", "Bytes under 1 KiB should stay as B.");
    Expect(FormatBytes(2048).find(L"KB") != std::wstring::npos, "KiB range should use KB.");
    Expect(FormatBytes(3ull * 1024ull * 1024ull).find(L"MB") != std::wstring::npos, "MiB range should use MB.");
    Expect(FormatBytes(2ull * 1024ull * 1024ull * 1024ull).find(L"GB") != std::wstring::npos, "GiB range should use GB.");
}

void TestSpecAbbreviationFromName()
{
    Expect(SpecAbbreviationFromName(std::nullopt).empty(), "Missing spec should abbreviate empty.");
    Expect(SpecAbbreviationFromName(std::string("")).empty(), "Empty spec should abbreviate empty.");
    Expect(SpecAbbreviationFromName(std::string("Brewmaster")) == L"BRE",
        "Single-word specs fall back to first three letters.");
    // Multi-word uses initials (up to 3).
    Expect(SpecAbbreviationFromName(std::string("Beast Mastery")) == L"BM", "Beast Mastery -> BM.");
    Expect(SpecAbbreviationFromName(std::string("Demon Hunter")) == L"DH", "Demon Hunter -> DH.");
}

void TestIsLikelyInvalidParticipantName()
{
    Expect(IsLikelyInvalidParticipantName(L""), "Empty name is invalid.");
    Expect(IsLikelyInvalidParticipantName(L"12345"), "All-digit name is invalid.");
    Expect(IsLikelyInvalidParticipantName(L"0x511"), "Hex-looking name is invalid.");
    Expect(!IsLikelyInvalidParticipantName(L"Monkibo"), "Normal character name is valid.");
}

void TestClassColorForParticipant()
{
    Expect(ClassColorForParticipant(std::nullopt) == kColorTextMuted, "Missing class uses muted color.");
    Expect(ClassColorForParticipant(std::string("Monk")) == RGB(0, 255, 150), "Monk class color.");
    Expect(ClassColorForParticipant(std::string("WARRIOR")) == RGB(198, 155, 109), "Class match is case-insensitive.");
    Expect(ClassColorForParticipant(std::string("NotAClass")) == kColorTextPrimary, "Unknown class uses primary.");
}

void TestEnumerateRecordingMediaFiles()
{
    const auto dir = MakeTempDir("media-enumerate");
    {
        std::ofstream((dir / "older.mkv").string()) << "a";
        std::ofstream((dir / "newer.mp4").string()) << "b";
        std::ofstream((dir / "ignore.txt").string()) << "c";
        std::ofstream((dir / "also.MKV").string()) << "d";
    }
    // Ensure write-time ordering is stable on Windows.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::ofstream((dir / "newest.mp4").string()) << "e";
    }

    Expect(EnumerateRecordingMediaFiles({}).empty(), "Empty folder path should yield no media.");

    const auto files = EnumerateRecordingMediaFiles(dir);
    Expect(files.size() == 4, "Only mkv/mp4 media should be enumerated (case-insensitive).");
    if (!files.empty()) {
        Expect(files.front().filename() == "newest.mp4", "Newest write-time media should sort first.");
    }
    for (const auto& file : files) {
        const auto ext = file.extension().wstring();
        Expect(_wcsicmp(ext.c_str(), L".mp4") == 0 || _wcsicmp(ext.c_str(), L".mkv") == 0,
            "Enumerated files must be mp4 or mkv.");
    }
}

void TestEnumerateYouTubeMediaFiles()
{
    const auto dir = MakeTempDir("youtube-media-enumerate");
    const auto clipsDir = dir / "Clips";
    std::error_code ec;
    std::filesystem::create_directories(clipsDir, ec);
    {
        std::ofstream((dir / "recording.mkv").string()) << "recording";
        std::ofstream((clipsDir / "clip.mp4").string()) << "clip";
        std::ofstream((clipsDir / "ignore.txt").string()) << "ignore";
    }

    const auto files = EnumerateYouTubeMediaFiles(dir);
    Expect(files.size() == 2, "YouTube media should include recordings and clips only.");
    bool foundRecording = false;
    bool foundClip = false;
    for (const auto& file : files) {
        if (file.path.filename() == "recording.mkv") {
            foundRecording = file.type == YouTubeMediaType::Recording;
        }
        if (file.path.filename() == "clip.mp4") {
            foundClip = file.type == YouTubeMediaType::Clip;
        }
    }
    Expect(foundRecording, "Root media should be classified as a recording.");
    Expect(foundClip, "Clips-folder media should be classified as a clip.");
    Expect(EnumerateYouTubeMediaFiles({}).empty(), "Empty YouTube media folder should yield no files.");
}

void TestSortYouTubeMediaFiles()
{
    std::vector<YouTubeMediaFile> files{
        {std::filesystem::path(L"zeta.mkv"), YouTubeMediaType::Recording, {}, 0},
        {std::filesystem::path(L"alpha.mp4"), YouTubeMediaType::Clip, {}, 0},
        {std::filesystem::path(L"Beta.mp4"), YouTubeMediaType::Recording, {}, 0},
    };
    SortYouTubeMediaFiles(files, YouTubeMediaSortColumn::Name, true);
    Expect(files.front().path.filename() == L"alpha.mp4", "Name sorting should be case-insensitive ascending.");
    Expect(files.back().path.filename() == L"zeta.mkv", "Name sorting should place zeta last.");

    SortYouTubeMediaFiles(files, YouTubeMediaSortColumn::Type, false);
    Expect(files.front().type == YouTubeMediaType::Clip, "Descending type sort should place clips first.");
}

void TestClassifyRecordingKind()
{
    Expect(ClassifyRecordingKind("manual", false) == RecordingKind::Manual, "Manual trigger is Manual.");
    Expect(ClassifyRecordingKind("mythic-start", false) == RecordingKind::MythicPlus, "Mythic trigger is M+.");
    Expect(ClassifyRecordingKind("raid-start", false) == RecordingKind::Raid, "Raid trigger is Raid.");
    Expect(ClassifyRecordingKind("pvp", false) == RecordingKind::Pvp, "PvP trigger is PvP.");
    Expect(ClassifyRecordingKind("arena", false) == RecordingKind::Pvp, "Arena trigger is PvP.");
    Expect(ClassifyRecordingKind("", true) == RecordingKind::MythicPlus, "Keystone metadata without trigger is M+.");
    Expect(ClassifyRecordingKind("", false) == RecordingKind::Manual, "No trigger and no mythic metadata is Manual.");
    Expect(ClassifyRecordingKind("MYTHIC-START", false) == RecordingKind::MythicPlus, "Trigger match is case-insensitive.");
}

void TestParseRecordingKeyLevelFilter()
{
    RecordingKeyLevelFilter filter;
    Expect(ParseRecordingKeyLevelFilter(L"", filter) && !filter.active, "Empty key filter is inactive.");
    Expect(ParseRecordingKeyLevelFilter(L"  ", filter) && !filter.active, "Whitespace key filter is inactive.");
    Expect(ParseRecordingKeyLevelFilter(L"17", filter) && filter.active && filter.minLevel == 17 && filter.maxLevel == 17,
        "Single key level should match itself.");
    Expect(ParseRecordingKeyLevelFilter(L"+12", filter) && filter.active && filter.minLevel == 12 && filter.maxLevel == 12,
        "Leading plus should be accepted.");
    Expect(ParseRecordingKeyLevelFilter(L"12-16", filter) && filter.active && filter.minLevel == 12 && filter.maxLevel == 16,
        "Inclusive range should parse.");
    Expect(ParseRecordingKeyLevelFilter(L" 12 - 16 ", filter) && filter.active && filter.minLevel == 12 && filter.maxLevel == 16,
        "Range whitespace should be ignored.");
    Expect(ParseRecordingKeyLevelFilter(L"16-12", filter) && filter.active && filter.minLevel == 12 && filter.maxLevel == 16,
        "Reversed range should swap.");
    Expect(!ParseRecordingKeyLevelFilter(L"12-", filter), "Trailing hyphen should fail.");
    Expect(!ParseRecordingKeyLevelFilter(L"abc", filter), "Non-numeric key filter should fail.");
    Expect(!ParseRecordingKeyLevelFilter(L"12-16-18", filter), "Three-part range should fail.");
}

void TestParseRecordingCharacterNameFilter()
{
    Expect(ParseRecordingCharacterNameFilter(L"").empty(), "Empty character filter has no names.");
    const auto one = ParseRecordingCharacterNameFilter(L" Ibblez ");
    Expect(one.size() == 1 && one.front() == L"Ibblez", "Single name should trim.");
    const auto two = ParseRecordingCharacterNameFilter(L"Ibblez, Taek");
    Expect(two.size() == 2 && two[0] == L"Ibblez" && two[1] == L"Taek", "Comma-separated names should split.");
    const auto skipped = ParseRecordingCharacterNameFilter(L"Ibblez,, ,Taek,");
    Expect(skipped.size() == 2, "Empty tokens should be skipped.");
}

void TestRecordingMatchesFilter()
{
    RecordingFilterItem mythicTimed;
    mythicTimed.kind = RecordingKind::MythicPlus;
    mythicTimed.timed = true;
    mythicTimed.keystoneLevel = 14;
    mythicTimed.participantNames = {L"Ibblez", L"Taek"};

    RecordingFilterItem mythicDepleted = mythicTimed;
    mythicDepleted.timed = false;
    mythicDepleted.depleted = true;
    mythicDepleted.keystoneLevel = 11;

    RecordingFilterItem manual;
    manual.kind = RecordingKind::Manual;
    manual.participantNames = {L"Ibblez"};

    RecordingFilterCriteria criteria;
    Expect(RecordingMatchesFilter(mythicTimed, criteria), "Default criteria should show M+ timed runs.");
    Expect(RecordingMatchesFilter(manual, criteria), "Default criteria should show manuals.");
    Expect(!RecordingFilterIsRestricting(criteria), "Default criteria is not restricting.");

    criteria.includeManual = false;
    Expect(!RecordingMatchesFilter(manual, criteria), "Unchecked Manual should hide manuals.");
    Expect(RecordingMatchesFilter(mythicTimed, criteria), "Unchecked Manual should still show M+.");
    Expect(RecordingFilterIsRestricting(criteria), "Hiding a type is restricting.");

    criteria = {};
    criteria.includeMythicPlus = false;
    Expect(!RecordingMatchesFilter(mythicTimed, criteria), "Unchecked M+ should hide mythic runs.");

    criteria = {};
    criteria.outcome = RecordingOutcomeFilter::Timed;
    Expect(RecordingMatchesFilter(mythicTimed, criteria), "Timed filter should keep timed runs.");
    Expect(!RecordingMatchesFilter(mythicDepleted, criteria), "Timed filter should hide depleted runs.");
    Expect(!RecordingMatchesFilter(manual, criteria), "Timed filter should hide manuals without an outcome.");

    criteria.outcome = RecordingOutcomeFilter::Depleted;
    Expect(RecordingMatchesFilter(mythicDepleted, criteria), "Depleted filter should keep depleted runs.");
    Expect(!RecordingMatchesFilter(mythicTimed, criteria), "Depleted filter should hide timed runs.");

    criteria = {};
    Expect(ParseRecordingKeyLevelFilter(L"12-16", criteria.keyLevel), "Test range should parse.");
    Expect(RecordingMatchesFilter(mythicTimed, criteria), "Range 12-16 should keep +14.");
    Expect(!RecordingMatchesFilter(mythicDepleted, criteria), "Range 12-16 should hide +11.");
    Expect(!RecordingMatchesFilter(manual, criteria), "Key filter should hide recordings without a level.");

    criteria = {};
    criteria.characterNames = ParseRecordingCharacterNameFilter(L"ibb, taek");
    Expect(RecordingMatchesFilter(mythicTimed, criteria), "Comma names should AND-match case-insensitively.");
    Expect(!RecordingMatchesFilter(manual, criteria), "Missing party member should fail the AND match.");
    criteria.characterNames = ParseRecordingCharacterNameFilter(L"voke");
    Expect(!RecordingMatchesFilter(mythicTimed, criteria), "Unrelated name should hide the recording.");
}

} // namespace

int main()
{
    TestFormatElapsed();
    TestParseClipTime();
    TestFormatBytes();
    TestSpecAbbreviationFromName();
    TestIsLikelyInvalidParticipantName();
    TestClassColorForParticipant();
    TestEnumerateRecordingMediaFiles();
    TestEnumerateYouTubeMediaFiles();
    TestSortYouTubeMediaFiles();
    TestClassifyRecordingKind();
    TestParseRecordingKeyLevelFilter();
    TestParseRecordingCharacterNameFilter();
    TestRecordingMatchesFilter();

    if (gFailures == 0) {
        std::cout << "All app helper tests passed.\n";
        return 0;
    }
    std::cerr << gFailures << " test(s) failed.\n";
    return 1;
}
