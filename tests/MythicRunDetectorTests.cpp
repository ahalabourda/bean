#include "log/MythicRunDetector.h"

#include <algorithm>
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

void TestStartAndSuccess()
{
    bean::log::MythicRunDetector detector;

    auto e1 = detector.ProcessLine("6/20/2026 00:00:00.000-7  CHALLENGE_MODE_START,\"Theater of Pain\",382,200,12,[117,152,10]");
    Expect(e1.has_value(), "Expected start event.");
    Expect(e1 && e1->type == bean::log::MythicEventType::RunStarted, "Expected RunStarted type.");

    auto e2 = detector.ProcessLine("6/20/2026 00:32:10.000-7  CHALLENGE_MODE_END,200,1,12,1925000.000000,55.000000,1980.000000");
    Expect(e2.has_value(), "Expected success event.");
    Expect(e2 && e2->type == bean::log::MythicEventType::RunEndedSuccess, "Expected RunEndedSuccess type.");
}

void TestStartAndEndEventSuccess()
{
    bean::log::MythicRunDetector detector;

    auto start = detector.ProcessLine("6/19/2026 21:00:00.000-7  CHALLENGE_MODE_START,402,10");
    Expect(start && start->type == bean::log::MythicEventType::RunStarted, "Run should start for END-event test.");

    auto ended = detector.ProcessLine("6/19/2026 21:30:00.000-7  CHALLENGE_MODE_END,402,1,10,1800000.000000,32.000000,1830.000000");
    Expect(ended.has_value(), "CHALLENGE_MODE_END should end the run.");
    Expect(ended && ended->type == bean::log::MythicEventType::RunEndedSuccess, "CHALLENGE_MODE_END should map to RunEndedSuccess.");
}

void TestEndEventOvertimeMapsToFailure()
{
    bean::log::MythicRunDetector detector;

    auto start = detector.ProcessLine("6/19/2026 21:00:00.000-7  CHALLENGE_MODE_START,402,10");
    Expect(start && start->type == bean::log::MythicEventType::RunStarted, "Run should start for overtime END-event test.");

    auto ended = detector.ProcessLine("6/19/2026 21:30:00.000-7  CHALLENGE_MODE_END,402,1,10,1935000.000000,-105.000000,1830.000000");
    Expect(ended.has_value(), "Overtime CHALLENGE_MODE_END should end the run.");
    Expect(ended && ended->type == bean::log::MythicEventType::RunEndedFailure, "Negative on-time delta should map CHALLENGE_MODE_END to RunEndedFailure.");
}

void TestEndEventAllZeroPayloadMapsToFailure()
{
    bean::log::MythicRunDetector detector;

    auto start = detector.ProcessLine("6/19/2026 21:00:00.000-7  CHALLENGE_MODE_START,402,10");
    Expect(start && start->type == bean::log::MythicEventType::RunStarted, "Run should start for zero-payload END-event test.");

    auto ended = detector.ProcessLine("6/26/2026 20:29:42.910-7  CHALLENGE_MODE_END,2915,0,0,0.000000,0.000000,0.000000");
    Expect(ended.has_value(), "Zero-payload CHALLENGE_MODE_END should end the run.");
    Expect(ended && ended->type == bean::log::MythicEventType::RunEndedFailure, "Zero-payload CHALLENGE_MODE_END should map to RunEndedFailure.");
}

void TestStartAndFailure()
{
    bean::log::MythicRunDetector detector;

    auto e1 = detector.ProcessLine("CHALLENGE_MODE_START");
    Expect(e1.has_value(), "Expected start event.");

    auto e2 = detector.ProcessLine("CHALLENGE_MODE_RESET");
    Expect(e2.has_value(), "Expected failure event.");
    Expect(e2 && e2->type == bean::log::MythicEventType::RunEndedFailure, "Expected RunEndedFailure type.");
}

void TestDuplicateStartForcesRestart()
{
    bean::log::MythicRunDetector detector;

    auto e1 = detector.ProcessLine("CHALLENGE_MODE_START");
    auto e2 = detector.ProcessLine("CHALLENGE_MODE_START");

    Expect(e1.has_value(), "First start should emit.");
    Expect(e2.has_value(), "Second start should emit to force restart.");
    Expect(e2 && e2->type == bean::log::MythicEventType::RunStarted, "Second start should still be RunStarted.");
}

void TestIgnoreEndWhenNotInRun()
{
    bean::log::MythicRunDetector detector;
    auto e = detector.ProcessLine("CHALLENGE_MODE_END");
    Expect(!e.has_value(), "End event should be ignored if run not started.");
}

void TestRetailChallengeStartParsesMapAndLevel()
{
    bean::log::MythicRunDetector detector;
    auto start = detector.ProcessLine("6/20/2026 00:10:00.000-7  CHALLENGE_MODE_START,\"Brackenhide Hollow\",2520,405,18,[10,11,124]");
    Expect(start.has_value(), "Retail challenge start should emit event.");
    Expect(start && start->challengeMapId.has_value() && *start->challengeMapId == 405, "Retail challenge start should parse challenge map id.");
    Expect(start && start->keystoneLevel.has_value() && *start->keystoneLevel == 18, "Retail challenge start should parse keystone level.");
}

void TestParticipantsAreCapturedFromCombatLog()
{
    bean::log::MythicRunDetector detector;
    auto start = detector.ProcessLine("6/20/2026 00:10:00.000-7  CHALLENGE_MODE_START,\"Brackenhide Hollow\",2520,405,18,[10,11,124]");
    Expect(start && start->type == bean::log::MythicEventType::RunStarted, "Run should start for participant test.");

    detector.ProcessLine("6/20/2026 00:10:01.000-7  COMBATANT_INFO,Player-120-0A8344F2,1,191,1953,5749,474,0,0,0,1093,1093,1093,85,30,208,208,208,0,352,1083,1083,1083,1134,1,268,(1),(1),[1],1,0,0,0");
    detector.ProcessLine("6/20/2026 00:10:02.000-7  SPELL_AURA_APPLIED,Player-120-0A8344F2,\"Monkibo-Darkspear\",0x511,0x0,Player-11-0E35390D,\"Augmenter-Tichondrius\",0x512,0x0,373113,\"Bounty: Haste\",0x1,BUFF");

    const auto participants = detector.GetParticipants();
    Expect(participants.size() == 2, "Expected two participants after combatant and aura lines.");

    const auto first = std::find_if(participants.begin(), participants.end(), [](const bean::log::MythicParticipant& p) {
        return p.guid == "Player-120-0A8344F2";
    });
    Expect(first != participants.end(), "Primary participant should be present.");
    if (first != participants.end()) {
        Expect(first->name.has_value() && *first->name == "Monkibo", "Participant should parse player name.");
        Expect(first->realm.has_value() && *first->realm == "Darkspear", "Participant should parse realm.");
        Expect(first->specId.has_value() && *first->specId == 268, "Participant should parse spec id from COMBATANT_INFO.");
        Expect(first->className.has_value() && *first->className == "Monk", "Participant should infer class name from spec.");
        Expect(first->specName.has_value() && *first->specName == "Brewmaster", "Participant should infer spec name.");
    }
}

void TestParticipantsCaptureNewExpansionSpec()
{
    bean::log::MythicRunDetector detector;
    auto start = detector.ProcessLine("6/20/2026 00:10:00.000-7  CHALLENGE_MODE_START,\"Skyreach\",3120,1209,18,[10,11,124]");
    Expect(start && start->type == bean::log::MythicEventType::RunStarted, "Run should start for new spec test.");

    detector.ProcessLine("6/20/2026 00:10:01.000-7  COMBATANT_INFO,Player-61-0FDD494B,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1480");
    detector.ProcessLine("6/20/2026 00:10:02.000-7  SPELL_AURA_APPLIED,Player-61-0FDD494B,\"Jabourer-Zul'jin\",0x511,0x0,Creature-0-0-0-0-1-0000000000,\"Target\",0x10a48,0x0,373113,\"Bounty: Haste\",0x1,BUFF");

    const auto participants = detector.GetParticipants();
    const auto jabourer = std::find_if(participants.begin(), participants.end(), [](const bean::log::MythicParticipant& p) {
        return p.guid == "Player-61-0FDD494B";
    });

    Expect(jabourer != participants.end(), "Jabourer participant should be present.");
    if (jabourer != participants.end()) {
        Expect(jabourer->specId.has_value() && *jabourer->specId == 1480, "New expansion spec id should be captured.");
        Expect(jabourer->specName.has_value() && *jabourer->specName == "Devourer", "Spec id 1480 should map to Devourer.");
        Expect(jabourer->className.has_value() && *jabourer->className == "Demon Hunter", "Spec id 1480 should map to Demon Hunter.");
    }
}

void TestParticipantCollectionShortCircuitsAfterFiveResolved()
{
    bean::log::MythicRunDetector detector;
    auto start = detector.ProcessLine("6/20/2026 00:10:00.000-7  CHALLENGE_MODE_START,\"Brackenhide Hollow\",2520,405,18,[10,11,124]");
    Expect(start && start->type == bean::log::MythicEventType::RunStarted, "Run should start for participant short-circuit test.");

    // Each pair resolves one player (spec via COMBATANT_INFO, name+realm via SPELL_AURA_APPLIED).
    detector.ProcessLine("6/20/2026 00:10:01.000-7  COMBATANT_INFO,Player-1-AAAA,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,268");
    detector.ProcessLine("6/20/2026 00:10:01.100-7  SPELL_AURA_APPLIED,Player-1-AAAA,\"Alpha-Area52\",0x511,0x0,Creature-0-0-0-0-1-0000000000,\"Target\",0x10a48,0x0,373113,\"Bounty: Haste\",0x1,BUFF");
    detector.ProcessLine("6/20/2026 00:10:02.000-7  COMBATANT_INFO,Player-2-BBBB,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,62");
    detector.ProcessLine("6/20/2026 00:10:02.100-7  SPELL_AURA_APPLIED,Player-2-BBBB,\"Bravo-Illidan\",0x511,0x0,Creature-0-0-0-0-1-0000000000,\"Target\",0x10a48,0x0,373113,\"Bounty: Haste\",0x1,BUFF");
    detector.ProcessLine("6/20/2026 00:10:03.000-7  COMBATANT_INFO,Player-3-CCCC,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,581");
    detector.ProcessLine("6/20/2026 00:10:03.100-7  SPELL_AURA_APPLIED,Player-3-CCCC,\"Charlie-Tichondrius\",0x511,0x0,Creature-0-0-0-0-1-0000000000,\"Target\",0x10a48,0x0,373113,\"Bounty: Haste\",0x1,BUFF");
    detector.ProcessLine("6/20/2026 00:10:04.000-7  COMBATANT_INFO,Player-4-DDDD,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,257");
    detector.ProcessLine("6/20/2026 00:10:04.100-7  SPELL_AURA_APPLIED,Player-4-DDDD,\"Delta-Stormrage\",0x511,0x0,Creature-0-0-0-0-1-0000000000,\"Target\",0x10a48,0x0,373113,\"Bounty: Haste\",0x1,BUFF");
    detector.ProcessLine("6/20/2026 00:10:05.000-7  COMBATANT_INFO,Player-5-EEEE,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,70");
    detector.ProcessLine("6/20/2026 00:10:05.100-7  SPELL_AURA_APPLIED,Player-5-EEEE,\"Echo-MalGanis\",0x511,0x0,Creature-0-0-0-0-1-0000000000,\"Target\",0x10a48,0x0,373113,\"Bounty: Haste\",0x1,BUFF");

    const auto resolvedFive = detector.GetParticipants();
    Expect(resolvedFive.size() == 5, "Expected five resolved participants before short-circuit check.");

    // These should be ignored because collection should be complete now.
    detector.ProcessLine("6/20/2026 00:10:06.000-7  COMBATANT_INFO,Player-6-FFFF,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,105");
    detector.ProcessLine("6/20/2026 00:10:06.100-7  SPELL_AURA_APPLIED,Player-6-FFFF,\"Foxtrot-Sargeras\",0x511,0x0,Creature-0-0-0-0-1-0000000000,\"Target\",0x10a48,0x0,373113,\"Bounty: Haste\",0x1,BUFF");

    const auto stillFive = detector.GetParticipants();
    Expect(stillFive.size() == 5, "Participant parsing should short-circuit after five resolved players.");
}

} // namespace

int main()
{
    TestStartAndSuccess();
    TestStartAndEndEventSuccess();
    TestStartAndFailure();
    TestDuplicateStartForcesRestart();
    TestIgnoreEndWhenNotInRun();
    TestEndEventOvertimeMapsToFailure();
    TestEndEventAllZeroPayloadMapsToFailure();
    TestRetailChallengeStartParsesMapAndLevel();
    TestParticipantsAreCapturedFromCombatLog();
    TestParticipantsCaptureNewExpansionSpec();
    TestParticipantCollectionShortCircuitsAfterFiveResolved();

    if (gFailures == 0) {
        std::cout << "All MythicRunDetector tests passed.\n";
        return 0;
    }

    std::cerr << gFailures << " test(s) failed.\n";
    return 1;
}
