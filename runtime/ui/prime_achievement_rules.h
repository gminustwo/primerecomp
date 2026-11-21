#pragma once
#include "local_achievements.h"
#include <array>
#include <cstdint>

namespace prime_achievements {
using local_achievements::Rule;
constexpr unsigned Items = 41;
constexpr unsigned ArtifactCount = Items;
constexpr unsigned ReactorClear = ArtifactCount + 1;
constexpr unsigned FrigateEscape = ReactorClear + 1;

enum Boss : unsigned {
    ParasiteQueen,
    HiveMecha,
    PlatedBeetle,
    IncineratorDrone,
    Flaahgra,
    Sheegoth,
    Thardus,
    ElitePirate,
    CloakedDrone,
    PhazonElite,
    OmegaPirate,
    MetaRidley,
    PrimeShell,
    PrimeCore,
    BossCount,
};
constexpr unsigned FirstBoss = FrigateEscape + 1;
using Progress = std::array<unsigned, FirstBoss + BossCount>;
inline Progress read_capacities(const int32_t *items) {
    Progress result{};
    for (unsigned i = 0; i < Items; ++i) {
        const int32_t capacity = items[2 * i + 1];
        result[i] = capacity > 0 ? (unsigned)capacity : 0;
    }
    for (unsigned i = 29; i < Items; ++i) result[ArtifactCount] += result[i] > 0;
    return result;
}

// The reactor's escape passage is reached after the Parasite Queen encounter.
// Loading directly on Tallon IV cannot establish the preceding frigate route.
struct IntroProgress {
    bool reactor_seen = false;
    bool reactor_clear = false;
    bool escaped = false;

    void observe(const char *room) {
        if (!strcmp(room, "Frigate Orpheon / 07_Intro_Reactor")) reactor_seen = true;
        if (reactor_seen && !strcmp(room, "Frigate Orpheon / 00g_intro_begin")) reactor_clear = true;
        if (reactor_clear && !strcmp(room, "Tallon Overworld / 01_Over_mainplaza")) escaped = true;
    }
};

constexpr Rule rules[] = {
    {"PRIME-REACTOR-CLEAR", "Reactor Showdown", "Defeat the Parasite Queen", ReactorClear, 1,
     Rule::Boss},
    {"PRIME-FRIGATE-ESCAPE", "Escape from Orpheon", "Escape the frigate and land on Tallon IV", FrigateEscape, 1,
     Rule::Escape},
    {"PRIME-EQUIP-01", "Ice Beam", "Obtain the Ice Beam", 1, 1, Rule::Beam},
    {"PRIME-EQUIP-02", "Wave Beam", "Obtain the Wave Beam", 2, 1, Rule::Beam},
    {"PRIME-EQUIP-03", "Plasma Beam", "Obtain the Plasma Beam", 3, 1, Rule::Beam},
    {"PRIME-EQUIP-06", "Morph Ball Bombs", "Obtain the Morph Ball Bombs", 6, 1, Rule::Bomb},
    {"PRIME-EQUIP-08", "Flamethrower", "Obtain the Flamethrower", 8, 1, Rule::Beam},
    {"PRIME-EQUIP-09", "Thermal Visor", "Obtain the Thermal Visor", 9, 1, Rule::Gear},
    {"PRIME-EQUIP-10", "Charge Beam", "Obtain the Charge Beam", 10, 1, Rule::Beam},
    {"PRIME-EQUIP-11", "Super Missile", "Obtain the Super Missile", 11, 1, Rule::Missile},
    {"PRIME-EQUIP-12", "Grapple Beam", "Obtain the Grapple Beam", 12, 1, Rule::Gear},
    {"PRIME-EQUIP-13", "X-Ray Visor", "Obtain the X-Ray Visor", 13, 1, Rule::Gear},
    {"PRIME-EQUIP-14", "Ice Spreader", "Obtain the Ice Spreader", 14, 1, Rule::Missile},
    {"PRIME-EQUIP-15", "Space Jump Boots", "Obtain the Space Jump Boots", 15, 1, Rule::Gear},
    {"PRIME-EQUIP-16", "Morph Ball", "Obtain the Morph Ball", 16, 1, Rule::Gear},
    {"PRIME-EQUIP-18", "Boost Ball", "Obtain the Boost Ball", 18, 1, Rule::Gear},
    {"PRIME-EQUIP-19", "Spider Ball", "Obtain the Spider Ball", 19, 1, Rule::Gear},
    {"PRIME-EQUIP-21", "Gravity Suit", "Obtain the Gravity Suit", 21, 1, Rule::Suit},
    {"PRIME-EQUIP-22", "Varia Suit", "Obtain the Varia Suit", 22, 1, Rule::Suit},
    {"PRIME-EQUIP-23", "Phazon Suit", "Obtain the Phazon Suit", 23, 1, Rule::Suit},
    {"PRIME-EQUIP-28", "Wavebuster", "Obtain the Wavebuster", 28, 1, Rule::Beam},
    {"PRIME-MISSILES-5", "Missile Reserve: 5", "Reach 5 missile capacity", 4, 5, Rule::Missile},
    {"PRIME-MISSILES-10", "Missile Reserve: 10", "Reach 10 missile capacity", 4, 10, Rule::Missile},
    {"PRIME-MISSILES-25", "Missile Reserve: 25", "Reach 25 missile capacity", 4, 25, Rule::Missile},
    {"PRIME-MISSILES-50", "Missile Reserve: 50", "Reach 50 missile capacity", 4, 50, Rule::Missile},
    {"PRIME-MISSILES-100", "Missile Reserve: 100", "Reach 100 missile capacity", 4, 100, Rule::Missile},
    {"PRIME-MISSILES-150", "Missile Reserve: 150", "Reach 150 missile capacity", 4, 150, Rule::Missile},
    {"PRIME-MISSILES-200", "Missile Reserve: 200", "Reach 200 missile capacity", 4, 200, Rule::Missile},
    {"PRIME-MISSILES-250", "Missile Reserve: 250", "Reach 250 missile capacity", 4, 250, Rule::Missile},
    {"PRIME-POWER-BOMBS-4", "Power Bomb Reserve: 4", "Reach 4 Power Bomb capacity", 7, 4, Rule::Bomb},
    {"PRIME-POWER-BOMBS-5", "Power Bomb Reserve: 5", "Reach 5 Power Bomb capacity", 7, 5, Rule::Bomb},
    {"PRIME-POWER-BOMBS-6", "Power Bomb Reserve: 6", "Reach 6 Power Bomb capacity", 7, 6, Rule::Bomb},
    {"PRIME-POWER-BOMBS-7", "Power Bomb Reserve: 7", "Reach 7 Power Bomb capacity", 7, 7, Rule::Bomb},
    {"PRIME-POWER-BOMBS-8", "Power Bomb Reserve: 8", "Reach 8 Power Bomb capacity", 7, 8, Rule::Bomb},
    {"PRIME-TANKS-1", "Energy Reserve: 1", "Collect your first Energy Tank", 24, 1, Rule::Tank},
    {"PRIME-TANKS-2", "Energy Reserve: 2", "Collect 2 Energy Tanks", 24, 2, Rule::Tank},
    {"PRIME-TANKS-4", "Energy Reserve: 4", "Collect 4 Energy Tanks", 24, 4, Rule::Tank},
    {"PRIME-TANKS-6", "Energy Reserve: 6", "Collect 6 Energy Tanks", 24, 6, Rule::Tank},
    {"PRIME-TANKS-8", "Energy Reserve: 8", "Collect 8 Energy Tanks", 24, 8, Rule::Tank},
    {"PRIME-TANKS-10", "Energy Reserve: 10", "Collect 10 Energy Tanks", 24, 10, Rule::Tank},
    {"PRIME-TANKS-12", "Energy Reserve: 12", "Collect 12 Energy Tanks", 24, 12, Rule::Tank},
    {"PRIME-TANKS-14", "Energy Reserve: 14", "Collect 14 Energy Tanks", 24, 14, Rule::Tank},
    {"PRIME-ARTIFACTS-3", "Chozo Relics: 3", "Collect 3 Chozo Artifacts", 41, 3, Rule::Artifact},
    {"PRIME-ARTIFACTS-6", "Chozo Relics: 6", "Collect 6 Chozo Artifacts", 41, 6, Rule::Artifact},
    {"PRIME-ARTIFACTS-9", "Chozo Relics: 9", "Collect 9 Chozo Artifacts", 41, 9, Rule::Artifact},
    {"PRIME-ARTIFACTS-12", "Chozo Relics: 12", "Collect 12 Chozo Artifacts", 41, 12, Rule::Artifact},
    {"PRIME-ARTIFACT-29", "Artifact of Truth", "Obtain the Artifact of Truth", 29, 1, Rule::Artifact},
    {"PRIME-ARTIFACT-30", "Artifact of Strength", "Obtain the Artifact of Strength", 30, 1, Rule::Artifact},
    {"PRIME-ARTIFACT-31", "Artifact of Elder", "Obtain the Artifact of Elder", 31, 1, Rule::Artifact},
    {"PRIME-ARTIFACT-32", "Artifact of Wild", "Obtain the Artifact of Wild", 32, 1, Rule::Artifact},
    {"PRIME-ARTIFACT-33", "Artifact of Lifegiver", "Obtain the Artifact of Lifegiver", 33, 1, Rule::Artifact},
    {"PRIME-ARTIFACT-34", "Artifact of Warrior", "Obtain the Artifact of Warrior", 34, 1, Rule::Artifact},
    {"PRIME-ARTIFACT-35", "Artifact of Chozo", "Obtain the Artifact of Chozo", 35, 1, Rule::Artifact},
    {"PRIME-ARTIFACT-36", "Artifact of Nature", "Obtain the Artifact of Nature", 36, 1, Rule::Artifact},
    {"PRIME-ARTIFACT-37", "Artifact of Sun", "Obtain the Artifact of Sun", 37, 1, Rule::Artifact},
    {"PRIME-ARTIFACT-38", "Artifact of World", "Obtain the Artifact of World", 38, 1, Rule::Artifact},
    {"PRIME-ARTIFACT-39", "Artifact of Spirit", "Obtain the Artifact of Spirit", 39, 1, Rule::Artifact},
    {"PRIME-ARTIFACT-40", "Artifact of Newborn", "Obtain the Artifact of Newborn", 40, 1, Rule::Artifact},
    {"PRIME-BOSS-HIVE-MECHA", "Hive Mecha", "Defeat Hive Mecha",
     FirstBoss + HiveMecha, 1, Rule::Boss},
    {"PRIME-BOSS-PLATED-BEETLE", "Plated Beetle", "Defeat the Plated Beetle and recover the Morph Ball",
     FirstBoss + PlatedBeetle, 1, Rule::Boss},
    {"PRIME-BOSS-INCINERATOR-DRONE", "Incinerator Drone", "Defeat the Incinerator Drone",
     FirstBoss + IncineratorDrone, 1, Rule::Boss},
    {"PRIME-BOSS-FLAAHGRA", "Flaahgra", "Defeat Flaahgra",
     FirstBoss + Flaahgra, 1, Rule::Boss},
    {"PRIME-BOSS-SHEEGOTH", "Sheegoth", "Defeat Sheegoth and recover the Wave Beam",
     FirstBoss + Sheegoth, 1, Rule::Boss},
    {"PRIME-BOSS-THARDUS", "Thardus", "Defeat Thardus and recover the Spider Ball",
     FirstBoss + Thardus, 1, Rule::Boss},
    {"PRIME-BOSS-ELITE-PIRATE", "Elite Pirate", "Defeat the Elite Pirate in Omega Research",
     FirstBoss + ElitePirate, 1, Rule::Boss},
    {"PRIME-BOSS-CLOAKED-DRONE", "Cloaked Drone", "Defeat the Cloaked Drone and recover the Power Bombs",
     FirstBoss + CloakedDrone, 1, Rule::Boss},
    {"PRIME-BOSS-PHAZON-ELITE", "Phazon Elite", "Defeat the Phazon Elite in Elite Research",
     FirstBoss + PhazonElite, 1, Rule::Boss},
    {"PRIME-BOSS-OMEGA-PIRATE", "Omega Pirate", "Defeat the Omega Pirate",
     FirstBoss + OmegaPirate, 1, Rule::Boss},
    {"PRIME-BOSS-META-RIDLEY", "Meta Ridley", "Defeat Meta Ridley and enter the Impact Crater",
     FirstBoss + MetaRidley, 1, Rule::Boss},
    {"PRIME-BOSS-PRIME-SHELL", "Metroid Prime: Shell", "Defeat the shell and enter the final arena",
     FirstBoss + PrimeShell, 1, Rule::Boss},
    {"PRIME-BOSS-PRIME-CORE", "Metroid Prime: Core", "Defeat the final form of Metroid Prime",
     FirstBoss + PrimeCore, 1, Rule::Boss},
};
}
