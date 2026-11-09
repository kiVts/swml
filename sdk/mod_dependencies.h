#pragma once
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "semver.h"

namespace swml_dependencies {
struct Mod {
    std::string id; bool enabled = true; std::vector<std::string> required, optional;
    std::string version;
    std::vector<std::string> conflicts, before, after;
    std::string problem;
};
struct Plan { std::vector<std::string> order; std::map<std::string, std::string> blocked; };
inline std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> out; size_t begin = 0;
    for (size_t i = 0; i <= text.size(); ++i) if (i == text.size() || text[i] == ',' || text[i] == ';' || text[i] == ' ' || text[i] == '\t') {
        if (i > begin) out.push_back(text.substr(begin, i - begin)); begin = i + 1;
    }
    std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end()); return out;
}
inline Plan plan(const std::vector<Mod>& input) {
    Plan result; std::map<std::string, Mod> all;
    for (const auto& mod : input) {
        if (all.contains(mod.id)) result.blocked[mod.id] = "duplicate mod id";
        all[mod.id] = mod;
        if (mod.enabled && !mod.problem.empty()) result.blocked[mod.id] = mod.problem;
    }
    for (const auto& [id, mod] : all) if (mod.enabled)
        for (const auto& conflict : mod.conflicts) {
            auto [other, range] = swml_version::requirement(conflict); auto it = all.find(other);
            if (it != all.end() && it->second.enabled && swml_version::satisfies(it->second.version, range)) {
                result.blocked[id] = "conflicts with enabled mod: " + other;
                result.blocked[other] = "conflicts with enabled mod: " + id;
            }
        }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [id, mod] : all) if (mod.enabled && !result.blocked.contains(id)) {
            for (const auto& requirement : mod.required) {
                auto [dependency, range] = swml_version::requirement(requirement);
                auto found = all.find(dependency);
                if (found == all.end() || !found->second.enabled || result.blocked.contains(dependency)) {
                    result.blocked[id] = "required dependency unavailable: " + dependency; changed = true; break;
                }
                if (!swml_version::satisfies(found->second.version, range)) {
                    result.blocked[id] = "dependency version mismatch: " + requirement; changed = true; break;
                }
            }
        }
    }
    std::set<std::string> remaining, done;
    for (const auto& [id, mod] : all) if (mod.enabled && !result.blocked.contains(id)) remaining.insert(id);
    while (!remaining.empty()) {
        std::vector<std::string> ready;
        for (const auto& id : remaining) {
            bool ordered = true;
            for (const auto& d : all[id].required) ordered &= done.contains(swml_version::requirement(d).first);
            for (const auto& d : all[id].after) if (remaining.contains(d)) ordered = false;
            for (const auto& other : remaining) if (other != id)
                if (std::find(all[other].before.begin(), all[other].before.end(), id) != all[other].before.end()) ordered = false;
            if (ordered) ready.push_back(id);
        }
        if (ready.empty()) {
            for (const auto& id : remaining) result.blocked[id] = "dependency cycle or dependency blocked by a cycle";
            break;
        }
        // Optional dependencies influence ordering without making optional cycles fatal.
        auto unmet = [&](const std::string& id) {
            size_t n = 0; for (const auto& d : all[id].optional) {
                auto [other, range] = swml_version::requirement(d);
                if (remaining.contains(other) && swml_version::satisfies(all[other].version, range)) ++n;
            } return n;
        };
        auto selected = *std::min_element(ready.begin(), ready.end(), [&](const auto& a, const auto& b) {
            return unmet(a) != unmet(b) ? unmet(a) < unmet(b) : a < b;
        });
        result.order.push_back(selected); done.insert(selected); remaining.erase(selected);
    }
    return result;
}
}
