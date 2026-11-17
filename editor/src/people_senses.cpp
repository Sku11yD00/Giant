#include "people_senses.h"

#include "people_travel.h"

#include "giant/world/people.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace giant::editor {

namespace {
constexpr double k_pi_d = 3.14159265358979323846;
}

const char* PeopleSenses::to_string(Kind kind) {
    switch (kind) {
    case Kind::shot: return "shot";
    case Kind::impact: return "impact";
    case Kind::blast: return "blast";
    case Kind::splash: return "splash";
    case Kind::voice: return "voice";
    case Kind::other: return "other";
    case Kind::sight: return "sight";
    }
    return "other";
}

bool PeopleSenses::from_string(std::string_view text, Kind& out) {
    for (const Kind kind : {Kind::shot, Kind::impact, Kind::blast, Kind::splash, Kind::voice, Kind::other, Kind::sight}) {
        if (text == to_string(kind)) {
            out = kind;
            return true;
        }
    }
    return false;
}

float PeopleSenses::blast_loudness(float impulse) {
    if (!std::isfinite(impulse) || impulse <= 0.0f) return 0.0f;
    return std::clamp(10.0f * std::sqrt(impulse), 5.0f, 400.0f); // (a push of 100 N s: as a rifle's report)
}

void PeopleSenses::clear() {
    m_noises.clear();
    for (Slot& slot : m_slots) slot = Slot{slot.id};
    m_cursor = k_done;
    m_watched.clear();
    player.reset();
    m_look = 0;
    m_look_thing = 0;
    m_holding = 0;
    m_stats = {};
}

void PeopleSenses::silence() {
    m_noises.clear();
    m_watched.clear();
    m_look_thing = 0;
    m_stats.noises = 0;
}

bool PeopleSenses::watch(std::string_view name, const DVec3& at, float clearance) {
    if (!settings.on || !m_running || name.empty() || name.size() > k_max_name) return false;
    if (!(clearance >= 0.0f)) clearance = 0.0f;
    clearance = std::min(clearance, 50.0f);
    if (!(std::fabs(at.x) <= 1e8 && std::fabs(at.y) <= 1e8 && std::fabs(at.z) <= 1e8)) return false; // (or not numbers)
    for (Watched& w : m_watched) {
        if (w.name == name) {
            w.at = at;
            w.clearance = clearance;
            return true;
        }
    }
    if (m_watched.size() >= k_max_watched) return false;
    m_watched.push_back({std::string(name), at, clearance});
    return true;
}

bool PeopleSenses::unwatch(std::string_view name) {
    const auto it = std::find_if(m_watched.begin(), m_watched.end(), [&](const Watched& w) { return w.name == name; });
    if (it == m_watched.end()) return false;
    m_watched.erase(it);
    m_look_thing = 0; // (a look that ran out of rays part way through a person begins that person again)
    return true;
}

bool PeopleSenses::noise(const DVec3& at, float loudness, Kind kind, double* reach) {
    if (!settings.on || !m_running) return false;
    if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z) || std::fabs(at.x) > 1e8 ||
        std::fabs(at.y) > 1e8 || std::fabs(at.z) > 1e8 || !std::isfinite(loudness)) {
        return false;
    }
    const float quietest = std::clamp(settings.hear_loudness, 0.01f, 100.0f);
    if (!(loudness > quietest)) return false; // (nobody a metre off would notice)
    Noise n;
    n.at = at;
    n.loudness = std::min(loudness, k_max_loudness);
    n.kind = kind;
    n.made = m_seconds;
    n.reach = std::min(static_cast<double>(n.loudness) / static_cast<double>(quietest), k_max_reach);
    if (reach != nullptr) *reach = n.reach;
    if (m_noises.size() >= k_max_noises) {
        // The quietest gives way to a louder one (those it had still to reach do not hear it).
        const auto quiet = std::min_element(m_noises.begin(), m_noises.end(),
                                            [](const Noise& a, const Noise& b) { return a.loudness < b.loudness; });
        ++m_stats.dropped;
        if (quiet->loudness >= n.loudness) return false;
        *quiet = n;
    } else {
        m_noises.push_back(n);
    }
    ++m_stats.made;
    m_stats.noises = static_cast<u32>(m_noises.size());
    return true;
}

// The slots put by the people as they are now, each keeping its own (both
// lists are in the order of the ids): people made or removed, an edit's
// cost. The one to look at next stays the same person (or the next after it).
void PeopleSenses::match_slots(const std::vector<world::Person>& people) {
    const bool done = m_cursor >= m_slots.size();
    const world::PersonId next = done ? 0 : m_slots[m_cursor].id;
    const bool look_done = m_look >= m_slots.size();
    const world::PersonId look_next = look_done ? 0 : m_slots[m_look].id;
    m_slots_before.swap(m_slots);
    m_slots.clear();
    m_slots.reserve(people.size());
    usize had = 0;
    for (const world::Person& person : people) {
        while (had < m_slots_before.size() && m_slots_before[had].id < person.id) ++had;
        Slot slot;
        if (had < m_slots_before.size() && m_slots_before[had].id == person.id) slot = m_slots_before[had];
        slot.id = person.id;
        m_slots.push_back(slot);
    }
    const auto place = [&](world::PersonId wanted) {
        const auto it = std::lower_bound(m_slots.begin(), m_slots.end(), wanted,
                                         [](const Slot& slot, world::PersonId id) { return slot.id < id; });
        return static_cast<usize>(it - m_slots.begin());
    };
    m_cursor = done ? k_done : place(next);
    m_look = look_done ? 0 : place(look_next);
    if (look_done || m_look >= m_slots.size() || m_slots[m_look].id != look_next) m_look_thing = 0; // (another's turn now)
    m_holding = 0;
    for (const Slot& slot : m_slots) m_holding += slot.held > 0.0f ? 1 : 0;
}

float PeopleSenses::eased(const Slot& slot) const { return std::max(eased_alone(slot), slot.held); }

float PeopleSenses::eased_alone(const Slot& slot) const {
    if (slot.alarm <= 0.0f) return 0.0f;
    const double calm = std::clamp(static_cast<double>(settings.calm_seconds), 1.0, 3600.0);
    const double left = static_cast<double>(slot.alarm) - std::max(m_seconds - slot.at, 0.0) / calm;
    return left > 0.0 ? static_cast<float>(left) : 0.0f;
}

void PeopleSenses::sync(const world::Scene& scene, const PeopleTravel& travel) {
    // Another scene, another run, or the simulation stopped: nobody has heard anything.
    if (scene.identity() != m_scene || travel.resets() != m_resets || travel.running() != m_running) {
        m_scene = scene.identity();
        m_resets = travel.resets();
        m_running = travel.running();
        clear();
        m_heard_from = m_heard_to = travel.seconds();
    }
    m_seconds = travel.seconds();
}

void PeopleSenses::update(const world::Scene& scene, const PeopleTravel& travel) {
    sync(scene, travel);
    const std::vector<world::Person>& people = scene.people;
    if (m_slots.size() != people.size()) match_slots(people); // (what each heard stays its own while it is quiet too)
    hear(people, travel);
    look(scene, travel);
    m_stats.noises = static_cast<u32>(m_noises.size());
}

void PeopleSenses::hear(const std::vector<world::Person>& people, const PeopleTravel& travel) {
    if (!settings.on || !m_running || m_noises.empty() || people.empty()) {
        if (!settings.on || !m_running) {
            m_noises.clear();
        } else { // (nobody to hear them: they go when they have spread as far as they are heard)
            const double now = m_seconds;
            std::erase_if(m_noises, [&](const Noise& n) { return n.made + n.reach / k_sound_speed <= now; });
        }
        // Quiet: nobody is looked at; the next noise is heard from now, the first time through the list.
        m_cursor = k_done;
        m_heard_from = m_heard_to = m_seconds;
        return;
    }
    const double quietest = std::clamp(static_cast<double>(settings.hear_loudness), 0.01, 100.0);
    const double full = std::max(quietest, std::min(static_cast<double>(settings.alarm_loudness), 1000.0));
    usize left = std::clamp<usize>(settings.looked_a_frame, 1, k_max_looked);
    left = std::min(left, std::max<usize>(k_tests_a_frame / m_noises.size(), 1));
    while (left > 0) {
        if (m_cursor >= people.size()) {
            // Through the list: the noises everyone in reach has heard go, and the next time through
            // hears what arrives from where this one heard to until now. It begins only once time has
            // passed (nothing new arrives before), so nobody is looked at for nothing.
            const double heard_to = m_heard_to;
            std::erase_if(m_noises, [&](const Noise& n) { return n.made + n.reach / k_sound_speed <= heard_to; });
            m_cursor = k_done;
            if (m_noises.empty() || m_seconds <= m_heard_to) break;
            m_cursor = 0;
            m_heard_from = m_heard_to;
            m_heard_to = m_seconds;
        }
        const world::Person& person = people[m_cursor];
        if (m_slots[m_cursor].id != person.id) { // (as many people as before, but others)
            match_slots(people);
            continue;
        }
        Slot& slot = m_slots[m_cursor];
        const DVec3 here = travel.where(person).position;
        for (const Noise& n : m_noises) {
            const double dx = here.x - n.at.x, dy = here.y - n.at.y, dz = here.z - n.at.z;
            const double square = dx * dx + dy * dy + dz * dz;
            if (square > n.reach * n.reach) continue;
            const double far_off = std::sqrt(square);
            // (Later than it was made, however near: one made where a person stands is heard the next time through.)
            const double arrives = std::max(n.made + far_off / k_sound_speed,
                                            std::nextafter(n.made, std::numeric_limits<double>::infinity()));
            if (arrives <= m_heard_from || arrives > m_heard_to) continue; // (heard the time before, or not here yet)
            const double level = static_cast<double>(n.loudness) / std::max(far_off, 1.0);
            if (level < quietest) continue;
            ++m_stats.heard;
            const float now = eased(slot);
            const float more = static_cast<float>(std::min(level / full, 1.0));
            // Each noise takes its share of what calm is left; the one that alarmed most is remembered.
            const float sum = 1.0f - (1.0f - now) * (1.0f - more);
            if (more >= now || now <= 0.0f) {
                slot.kind = n.kind;
                slot.from = n.at;
                slot.level = static_cast<float>(level);
                slot.heard_at = m_seconds;
            }
            slot.alarm = sum;
            slot.at = m_seconds;
        }
        ++m_cursor;
        --left;
    }
}

// The people's eyes looked through, a share a frame: who sees a thing shown. While one is seen its
// alarm is held (Slot::held: it does not ease under that); looked through again and not seeing it,
// the person lets go and eases from there.
void PeopleSenses::look(const world::Scene& scene, const PeopleTravel& travel) {
    const std::vector<world::Person>& people = scene.people;
    if (!settings.on || !m_running) {
        m_watched.clear();
        player.reset();
    }
    const bool any = settings.sight && (player.has_value() || !m_watched.empty());
    if (people.empty() || (!any && m_holding == 0)) { // (nothing to see, and nobody still holding what it saw)
        m_look_thing = 0;
        return;
    }
    const usize things = any ? m_watched.size() + 1 : 0; // (the player's place first, then those shown)
    // (What a look that ran out of rays had seen is of the things as they were: begun again when the player comes or goes.)
    if (!any || player.has_value() != m_look_player) m_look_thing = 0;
    m_look_player = player.has_value();
    const double reach = std::clamp(static_cast<double>(settings.see_distance), 1.0, k_max_see);
    const double across = std::clamp(static_cast<double>(settings.see_angle), 10.0, 360.0);
    const double cosine = across >= 360.0 ? -2.0 : std::cos(across * 0.5 * (k_pi_d / 180.0));
    u32 rays = std::clamp<u32>(settings.rays_a_frame, 1, k_max_rays);
    usize left = std::min<usize>(std::clamp<usize>(settings.seen_a_frame, 1, k_max_looked), people.size());
    while (left > 0) {
        if (m_look >= people.size()) m_look = 0;
        const world::Person& person = people[m_look];
        if (m_slots[m_look].id != person.id) { // (as many people as before, but others)
            match_slots(people);
            continue;
        }
        Slot& slot = m_slots[m_look];
        const PeopleTravel::Where here = travel.where(person);
        const float base = eased_alone(slot); // (what it is alarmed by but for what it sees)
        // Its eyes and the way it faces are worked out only for one with something near enough.
        bool have_eyes = false, have_facing = false;
        double eye_y = 0.0, fx = 0.0, fz = 0.0;
        float best = 0.0f;
        DVec3 best_at{};
        if (m_look_thing > 0) { // (the frame before ran out of rays part way through this one)
            best = m_look_best;
            best_at = m_look_at;
        }
        for (usize thing = m_look_thing; thing < things; ++thing) {
            if (thing == 0 && !player) continue;
            const DVec3 at = thing == 0 ? *player : m_watched[thing - 1].at;
            const double clearance = thing == 0 ? 0.0 : static_cast<double>(m_watched[thing - 1].clearance);
            const double dx = at.x - here.position.x, dz = at.z - here.position.z;
            if (dx * dx + dz * dz > reach * reach) continue;
            if (!have_eyes) {
                // On the ground where there is terrain (as its entity stands: a person's own height is not kept to it).
                const std::optional<double> ground = scene.terrain.height_at(here.position.x, here.position.z);
                eye_y = (ground ? *ground : here.position.y) + k_eye_height;
                have_eyes = true;
            }
            const double dy = at.y - eye_y;
            const double square = dx * dx + dy * dy + dz * dz;
            if (square > reach * reach) continue;
            const double far_off = std::sqrt(square);
            if (far_off > k_arms_reach) {
                if (!have_facing) {
                    const double turn = static_cast<double>(here.yaw) * (k_pi_d / 180.0);
                    fx = std::sin(turn); // (the way it faces: yaw 0 looks along +z)
                    fz = std::cos(turn);
                    have_facing = true;
                }
                if (dx * fx + dz * fz < cosine * far_off) continue; // (behind it, out to the side, or too far up or down)
            }
            // Fully alarming within a fifth of the distance, a tenth at its end.
            const float more = static_cast<float>(std::clamp(1.125 * (1.0 - far_off / reach) + 0.1, 0.1, 1.0));
            // Seen or not, it would change nothing: no ray is spent (one whose alarm is held by what it
            // sees is looked through all the same: whether it still sees is what holds it).
            if (more <= best || (more < base && slot.held <= 0.0f)) continue;
            const double open = far_off - clearance; // (the thing's own body, within its clearance, does not hide it)
            if (blocked && open > 0.1) {
                if (rays == 0) { // out of rays: this one is looked through from this thing on next frame
                    m_look_thing = thing;
                    m_look_best = best;
                    m_look_at = best_at;
                    return;
                }
                --rays;
                ++m_stats.rays;
                const double part = open / far_off;
                const DVec3 eyes{here.position.x, eye_y, here.position.z};
                if (blocked(eyes, {eyes.x + dx * part, eyes.y + dy * part, eyes.z + dz * part})) continue;
            }
            best = more;
            best_at = at;
        }
        if (best > 0.0f) {
            ++m_stats.seen;
            if (slot.held <= 0.0f) ++m_holding;
            float under = base;
            if (best < slot.held) { // what it sees now alarms less than what it saw: it eases from there, not at once
                slot.alarm = eased(slot);
                slot.at = m_seconds;
                under = slot.alarm;
            }
            slot.held = best;
            if (best > under || slot.kind == Kind::sight) { // (a noise as alarming stays what is remembered)
                slot.kind = Kind::sight;
                slot.from = best_at;
                slot.level = best;
                slot.heard_at = m_seconds;
            }
        } else if (slot.held > 0.0f) { // out of sight: it eases from here
            slot.alarm = eased(slot);
            slot.at = m_seconds;
            slot.held = 0.0f;
            --m_holding;
        }
        m_look_thing = 0;
        ++m_look;
        --left;
    }
}

bool PeopleSenses::heard(const world::Scene& scene, world::PersonId id, Heard& out) const {
    if (!m_running || scene.identity() != m_scene) return false;
    const world::Person* person = world::find_person(scene.people, id);
    if (person == nullptr) return false;
    // (By its id, not its place in the list: the slots are matched to the people at the next update.)
    const auto it = std::lower_bound(m_slots.begin(), m_slots.end(), id,
                                     [](const Slot& s, world::PersonId wanted) { return s.id < wanted; });
    if (it == m_slots.end() || it->id != id) return false;
    const Slot& slot = *it;
    const float now = eased(slot);
    if (now <= 0.0f) return false;
    out.alarm = now;
    out.kind = slot.kind;
    out.from = slot.from;
    out.level = slot.level;
    out.seconds = std::max(m_seconds - slot.heard_at, 0.0);
    return true;
}

float PeopleSenses::alarm_at(const world::Scene& scene, usize index) const {
    if (!m_running || scene.identity() != m_scene || index >= scene.people.size()) return 0.0f;
    const world::PersonId id = scene.people[index].id;
    if (index < m_slots.size() && m_slots[index].id == id) return eased(m_slots[index]);
    return alarm(scene, id);
}

float PeopleSenses::alarm(const world::Scene& scene, world::PersonId id) const {
    Heard h;
    return heard(scene, id, h) ? h.alarm : 0.0f;
}

} // namespace giant::editor
