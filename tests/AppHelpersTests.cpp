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
    Expect(FormatElapsed(std::chrono::seconds(3661)) == L"01:01:01", "Over an hour should be hh:mm:ss.");
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

    if (gFailures == 0) {
        std::cout << "All app helper tests passed.\n";
        return 0;
    }
    std::cerr << gFailures << " test(s) failed.\n";
    return 1;
}
