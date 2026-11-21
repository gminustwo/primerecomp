#pragma once
#include "prime_achievement_rules.h"
#include <algorithm>
#include <bitset>
#include <optional>

namespace prime_achievements {
using AssetId = std::array<uint64_t, 2>;

// Room and persistent defeat relay identifiers from the v1.0.0 encounter data.
constexpr AssetId ReactorCore = {0x47bd28dbb4c87df0ull, 0x1cf4f6cf1403409bull};
constexpr AssetId HiveTotem = {0x49910f6b1585ccd2ull, 0x9594755124c11c97ull};
constexpr AssetId RuinedShrine = {0x47d39510762eb17bull, 0xf7505de1c18428b1ull};
constexpr AssetId BurnDome = {0x4ed83f6886e6497cull, 0x6904017a41311f98ull};
constexpr AssetId Sunchamber = {0x4a719df1c8589215ull, 0x72c10dbb8908b881ull};
constexpr AssetId ChapelOfTheElders = {0x4269807bb0414f85ull, 0x7abaa681af6bcebaull};
constexpr AssetId QuarantineCave = {0x4160a6f2169aaa49ull, 0xcbb1832b07345fa2ull};
constexpr AssetId OmegaResearch = {0x46c247ecfad3dc94ull, 0x8e791b136dc63496ull};
constexpr AssetId CentralDynamo = {0x445649372a5c08f3ull, 0xc7f08742f9bbac8aull};
constexpr AssetId EliteResearch = {0x4095e7a4601aac0full, 0x0ba2394bea7cf996ull};
constexpr AssetId EliteQuarters = {0x4fd3742cd216e17aull, 0xa04bbab9149150a3ull};
constexpr AssetId TempleRoom = {0x4ae3e9855722350aull, 0x3fb9222632366bb9ull};
constexpr AssetId CraterEntryRoom = {0x407cd47a6929c583ull, 0x5c3969776c581480ull};
constexpr AssetId CoreRoom = {0x428b0be0a103fb3cull, 0x3f881ba39fc6c683ull};
constexpr AssetId ShellRoomA = {0x43ade8f64f0d441cull, 0x3c90bab41b0f2b9dull};
constexpr AssetId ShellRoomB = {0x42209e636ed2b08full, 0x4d89f075bc72c28dull};
constexpr AssetId ShellRoomC = {0x4cdd3da279cba2e8ull, 0x2174807649ffcd8bull};
constexpr AssetId ShellRoomD = {0x4f23a6eae655de83ull, 0x9e6d628c1b733ba1ull};
constexpr AssetId ShellRoomE = {0x42ca0d946a6653a2ull, 0x34a102e4f13a2288ull};
constexpr AssetId ShellRoomPassage = {0x44b42fa643b3c4f8ull, 0x4131be281e4bd488ull};
constexpr AssetId EndingRoom = {0x4128c442ce44a797ull, 0x2237a82646697283ull};
constexpr AssetId QueenClear = {0x4b8776f0170c0e36ull, 0x3fcace07431c749aull};
constexpr AssetId HiveClear = {0x42eeea6b440f2807ull, 0xb06fcd2db98bb0b3ull};
constexpr AssetId IncineratorClear = {0x4b4390d6086d0575ull, 0xc61cd7917243f59cull};
constexpr AssetId FlaahgraClear = {0x435ce7b6d99af04aull, 0x9f1503de6c1b88b8ull};
constexpr AssetId EliteClear = {0x4ce94fe67dcec19eull, 0x873a2db43e6d58b3ull};
constexpr AssetId PhazonClear = {0x4143e93b792cf53aull, 0xf5c0282a344cca82ull};
constexpr AssetId OmegaClear = {0x4f1f2ebee1787809ull, 0x35208a2f16463085ull};


enum BossState { Unknown, Pending, Defeated };
struct BossSample {
    AssetId room{};
    std::array<BossState, BossCount> state{};
};

template<class Read>
BossSample read_bosses(uint64_t game, const int32_t *items, const Read &read) {
    struct WorldEntry {
        AssetId world, room;
        uint64_t mailbox;
        uint8_t remainder[0x28];
    };
    static_assert(sizeof(WorldEntry) == 0x50);
    AssetId world{};
    uint32_t count = 0;
    uint64_t worlds = 0;
    BossSample result;
    if (!game || !read(game, world) || world == AssetId{} || !read(game + 0x10, count) ||
        count > 64 || !read(game + 0x18, worlds) || !worlds) return result;

    WorldEntry area{};
    for (uint32_t i = 0; i < count; ++i) {
        if (!read(worlds + i * sizeof(area), area)) return result;
        if (area.world == world) break;
    }
    if (area.world != world || area.room == AssetId{~0ull, ~0ull}) return result;
    result.room = area.room;

    static constexpr struct { Boss boss; AssetId room, relay; } mailbox_rules[] = {
        {ParasiteQueen, ReactorCore, QueenClear}, {HiveMecha, HiveTotem, HiveClear},
        {IncineratorDrone, BurnDome, IncineratorClear}, {Flaahgra, Sunchamber, FlaahgraClear},
        {ElitePirate, OmegaResearch, EliteClear}, {PhazonElite, EliteResearch, PhazonClear},
        {OmegaPirate, EliteQuarters, OmegaClear},
    };
    for (const auto &rule : mailbox_rules) {
        if (rule.room != area.room) continue;
        uint32_t messages = 0;
        uint64_t entries = 0;
        if (!area.mailbox || !read(area.mailbox, messages) || messages > 4096 ||
            (messages && (!read(area.mailbox + 8, entries) || !entries))) break;
        auto status = Pending;
        for (uint32_t i = 0; i < messages; ++i) {
            AssetId relay{};
            if (!read(entries + i * 0x38, relay)) { status = Unknown; break; }
            if (relay == rule.relay) { status = Defeated; break; }
        }
        result.state[rule.boss] = status;
        break;
    }

    static constexpr struct { Boss boss; AssetId room; unsigned item; } reward_rules[] = {
        {PlatedBeetle, RuinedShrine, 16}, {Sheegoth, ChapelOfTheElders, 2},
        {Thardus, QuarantineCave, 19}, {CloakedDrone, CentralDynamo, 7},
    };
    for (const auto &rule : reward_rules)
        if (rule.room == area.room)
            result.state[rule.boss] = items[2 * rule.item + 1] > 0 ? Defeated : Pending;

    uint8_t complete = 0;
    if ((area.room == CoreRoom || area.room == EndingRoom) && read(game + 0x2d0, complete))
        result.state[PrimeCore] = complete & 1 ? Defeated : Pending;
    return result;
}

class BossProgress {
public:
    void rebase() { previous.reset(); }

    std::bitset<BossCount> observe(const BossSample &current) {
        std::bitset<BossCount> wins;
        if (current.room == AssetId{}) return wins;
        if (previous) {
            for (unsigned i = 0; i < BossCount; ++i)
                wins[i] = previous->state[i] == Pending && current.state[i] == Defeated;
            wins[MetaRidley] = previous->room == TempleRoom && current.room == CraterEntryRoom;
            static constexpr AssetId shell_rooms[] = {
                ShellRoomA, ShellRoomB, ShellRoomC, ShellRoomD, ShellRoomE, ShellRoomPassage,
            };
            wins[PrimeShell] = current.room == CoreRoom &&
                std::find(std::begin(shell_rooms), std::end(shell_rooms), previous->room) != std::end(shell_rooms);
        }
        previous = current;
        return wins;
    }

private:
    std::optional<BossSample> previous;
};
}
