#include "../runtime/ui/prime_achievement_rules.h"
#include <cassert>
#include <set>

using namespace prime_achievements;
using local_achievements::Tracker;
constexpr size_t count = sizeof rules / sizeof *rules;

int main(int argc, char **argv) {
    assert(argc == 2);
    std::set<std::string> ids;
    for (const auto &r : rules) {
        assert(r.metric <= FrigateEscape && r.target > 0 && ids.insert(r.id).second);
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
    puts("PASS: capacity milestones, artifact counting, load baseline and saved unlocks");
}
