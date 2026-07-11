#include "log/MythicRunDetector.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string ExtractTimestamp(const std::string& line)
{
    const auto firstDoubleSpace = line.find("  ");
    if (firstDoubleSpace == std::string::npos) {
        return "<no-timestamp>";
    }
    return line.substr(0, firstDoubleSpace);
}

std::string EventTypeToString(bean::log::MythicEventType type)
{
    using bean::log::MythicEventType;
    switch (type) {
    case MythicEventType::RunStarted:
        return "RunStarted";
    case MythicEventType::RunEndedSuccess:
        return "RunEndedSuccess";
    case MythicEventType::RunEndedFailure:
        return "RunEndedFailure";
    }
    return "Unknown";
}

std::string StatusTagForEvent(const bean::log::MythicEvent& event)
{
    using bean::log::MythicEventType;
    switch (event.type) {
    case MythicEventType::RunStarted:
        return "recording-started";
    case MythicEventType::RunEndedSuccess:
        return "challenge-ended";
    case MythicEventType::RunEndedFailure:
        return "challenge-failed";
    }
    return "unknown";
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path inputPath = (argc >= 2)
        ? std::filesystem::path(argv[1])
        : std::filesystem::path("tests") / "WoWCombatLog-061926_232002.txt";
    const std::filesystem::path outputPath = (argc >= 3)
        ? std::filesystem::path(argv[2])
        : std::filesystem::path("tests") / "WoWCombatLog-061926_232002.detected-statuses.txt";

    std::ifstream input(inputPath);
    if (!input.is_open()) {
        std::cerr << "Failed to open combat log input: " << inputPath.string() << '\n';
        return 1;
    }

    std::ofstream output(outputPath, std::ios::trunc);
    if (!output.is_open()) {
        std::cerr << "Failed to open output file: " << outputPath.string() << '\n';
        return 1;
    }

    bean::log::MythicRunDetector detector;
    std::string line;
    std::size_t lineNumber = 0;
    std::size_t eventCount = 0;

    output << "Input: " << inputPath.string() << "\n";
    output << "Output: " << outputPath.string() << "\n";
    output << "Detected events:\n\n";

    while (std::getline(input, line)) {
        ++lineNumber;
        const auto event = detector.ProcessLine(line);
        if (!event.has_value()) {
            continue;
        }

        ++eventCount;
        output << "[" << ExtractTimestamp(line) << "] "
               << "status=" << StatusTagForEvent(*event)
               << " event=" << EventTypeToString(event->type)
               << " (line " << lineNumber << ")";

        if (event->challengeMapId.has_value()) {
            output << " challengeMapId=" << *event->challengeMapId;
        }
        if (event->keystoneLevel.has_value()) {
            output << " keystoneLevel=" << *event->keystoneLevel;
        }
        if (event->mapId.has_value()) {
            output << " mapId=" << *event->mapId;
        }
        if (event->mapName.has_value()) {
            output << " mapName=\"" << *event->mapName << "\"";
        }
        output << '\n';
    }

    output << "\nTotal detected events: " << eventCount << '\n';

    if (!output.good()) {
        std::cerr << "Failed while writing output file: " << outputPath.string() << '\n';
        return 1;
    }

    std::cout << "Wrote replay output to: " << outputPath.string() << '\n';
    std::cout << "Detected events: " << eventCount << '\n';
    return 0;
}
