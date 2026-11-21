#include "../runtime/ui/prime_boss_progress.h"
#include <cassert>
#include <set>

using namespace prime_achievements;
using local_achievements::Tracker;
constexpr size_t count = sizeof rules / sizeof *rules;

int main(int argc, char **argv) {
    assert(argc == 2);
    std::set<std::string> ids;
    static_assert(count == 71);
    for (const auto &r : rules) {
        assert(r.metric < Progress{}.size() && r.target > 0 && ids.insert(r.id).second);
    }
    IntroProgress intro;
    intro.observe("Tallon Overworld / 01_Over_mainplaza");
    assert(!intro.reactor_clear && !intro.escaped);
    intro.observe("Frigate Orpheon / 07_Intro_Reactor");
    assert(!intro.reactor_clear);
    intro.observe("Frigate Orpheon / 00g_intro_begin");
    assert(intro.reactor_clear && !intro.escaped);
    intro.observe("Tallon Overworld / 01_Over_mainplaza");
    assert(intro.escaped);

    std::array<int32_t, Items * 2> items{};
    BossProgress bosses;
    for (unsigned boss = 0; boss < BossCount; ++boss) {
        BossSample before{HiveTotem}, after{HiveTotem};
        before.state[boss] = Pending;
        after.state[boss] = Defeated;
        if (boss == MetaRidley) {
            before.room = TempleRoom;
            after.room = CraterEntryRoom;
        } else if (boss == PrimeShell) {
            before.room = ShellRoomPassage;
            after.room = CoreRoom;
        }
        bosses.rebase();
        assert(bosses.observe(after).none()); // Loaded victory.
        assert(bosses.observe(before).none());
        assert(bosses.observe({}).none()); // Briefly missing room during a transition.
        const auto won = bosses.observe(after);
        assert(won.count() == 1 && won[boss]);
        assert(bosses.observe(after).none());
    }
    std::array<uint8_t, 1024> guest{};
    const auto set = [&](size_t at, const auto &value) {
        memcpy(guest.data() + at, &value, sizeof(value));
    };
    const auto read = [&](uint64_t at, auto &value) {
        if (at > guest.size() - sizeof(value)) return false;
        memcpy(&value, guest.data() + at, sizeof(value));
        return true;
    };
    set(32, AssetId{1, 2});
    set(32 + 0x10, uint32_t{1});
    set(32 + 0x18, uint64_t{128});
    set(128, AssetId{1, 2});
    set(128 + 0x10, ReactorCore);
    set(128 + 0x20, uint64_t{256});
    set(256 + 8, uint64_t{320});
    bosses.rebase();
    assert(bosses.observe(read_bosses(32, items.data(), read)).none());
    set(256, uint32_t{1});
    set(320, QueenClear);
    assert(bosses.observe(read_bosses(32, items.data(), read))[ParasiteQueen]);

    set(128 + 0x10, RuinedShrine);
    assert(bosses.observe(read_bosses(32, items.data(), read)).none());
    items[2 * 16 + 1] = 1;
    assert(bosses.observe(read_bosses(32, items.data(), read))[PlatedBeetle]);
    items[2 * 16 + 1] = 0;

    Tracker tracker(rules, count);
    assert(tracker.open(argv[1]));
    assert(tracker.sample(read_capacities(items.data()).data()).empty());

    items[2 * 4] = 5;  // Ammo itself is not a capacity upgrade.
    assert(tracker.sample(read_capacities(items.data()).data()).empty());
    items[2 * 4 + 1] = 10;
    auto got = tracker.sample(read_capacities(items.data()).data());
    assert(got.size() == 2);
    assert(strcmp(rules[got[0]].id, "PRIME-MISSILES-5") == 0);
    assert(strcmp(rules[got[1]].id, "PRIME-MISSILES-10") == 0);

    items[2 * 29 + 1] = 3;  // Count distinct artifact slots, not the value in a slot.
    assert(read_capacities(items.data())[ArtifactCount] == 1);
    for (unsigned i = 29; i < Items; ++i) items[2 * i + 1] = 1;
    tracker.rebase();
    assert(tracker.sample(read_capacities(items.data()).data()).empty());

    assert(tracker.award(0));
    assert(!tracker.award(0));

    Tracker restart(rules, count);
    assert(restart.open(argv[1]));
    assert(!restart.award(0));
    Progress empty{};
    restart.sample(empty.data());
    Progress repeated{};
    repeated[4] = 10;
    assert(restart.sample(repeated.data()).empty());
    puts("PASS: boss transitions, capacity milestones, artifact counting, load baseline and saved unlocks");
}
