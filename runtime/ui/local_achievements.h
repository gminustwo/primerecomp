// Tracker adapted from the local achievement prototype in LAS Recomp (MIT).
// A game supplies one value per rule and starts a new baseline after loading.
#pragma once
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <string>
#include <vector>

namespace local_achievements {
struct Rule {
    const char *id, *title, *description;
    unsigned metric, target;
    enum Badge { Gear, Beam, Suit, Missile, Tank, Bomb, Artifact, Escape, Boss } badge;
};

class Tracker {
public:
    Tracker(const Rule *rules, size_t count) : rules_(rules), earned_(count), seen_(count) {}

    bool open(const std::string &path) {
        path_ = path;
        FILE *file = fopen(path.c_str(), "rb");
        if (!file) return writable_ = errno == ENOENT;
        char line[256], id[32], extra;
        long long timestamp;
        while (fgets(line, sizeof line, file)) {
            if (!strchr(line, '\n') || sscanf(line, "%31s %lld %c", id, &timestamp, &extra) != 2 || timestamp <= 0)
                continue;
            for (size_t i = 0; i < earned_.size(); ++i)
                if (!strcmp(id, rules_[i].id)) earned_[i] = true;
        }
        bool ok = !ferror(file);
        if (fclose(file)) ok = false;
        return writable_ = ok;
    }

    bool unlocked(size_t index) const { return earned_[index]; }
    void rebase() { ready_ = false; }

    std::vector<size_t> sample(const unsigned *values) {
        std::vector<size_t> unlocked;
        if (!writable_) return unlocked;
        for (size_t i = 0; i < earned_.size(); ++i) {
            bool met = values[rules_[i].metric] >= rules_[i].target;
            if (!ready_) {
                seen_[i] = met;
            } else if (met && !seen_[i] && !earned_[i]) {
                if (award(i)) {
                    unlocked.push_back(i);
                    seen_[i] = true;
                }
            }
        }
        ready_ = true;
        return unlocked;
    }

    bool award(size_t index) {
        if (!writable_ || earned_[index]) return false;
        const bool ok = record(index);
        write_failed_ = !ok;
        return ok;
    }

    bool has_error() const { return !writable_ || write_failed_; }

private:
    bool record(size_t index) {
        FILE *file = fopen(path_.c_str(), "ab");
        if (!file) return false;
        bool ok = fprintf(file, "\n%s %lld\n", rules_[index].id, (long long)time(nullptr)) > 0;
        if (fflush(file)) ok = false;
        if (fclose(file)) ok = false;
        if (ok) earned_[index] = true;
        return ok;
    }

    const Rule *rules_;
    std::string path_;
    std::vector<bool> earned_, seen_;
    bool writable_ = false, ready_ = false, write_failed_ = false;
};
}
