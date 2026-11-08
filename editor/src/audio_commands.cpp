// Sound commands: a sound played somewhere in the scene, heard at the
// camera as the world shapes it (audio_state.h); recorded to a WAV file;
// the mixer's state. View state, not undoable.
#include "audio_state.h"
#include "editor_commands.h"
#include "physics_state.h"

#include "giant/audio/sounds.h"
#include "giant/core/assert.h"

#include <cmath>
#include <string>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "audio command could not be registered");
}

Json point_schema(const char* description) {
    return {{"type", "array"},
            {"items", {{"type", "number"}, {"minimum", -1e8}, {"maximum", 1e8}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description", description}};
}

DVec3 point_of(const Json& p) { return {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()}; }
Json vec(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }
Json dvec(const DVec3& v) { return Json::array({v.x, v.y, v.z}); }

Json heard_json(const audio::Heard& h, const DVec3& at, const DVec3& listener) {
    Json taps = Json::array();
    for (const audio::Tap& t : h.taps)
        taps.push_back({{"delay", t.delay}, {"gain", t.gain}, {"lowpass", t.lowpass}, {"direction", vec(t.direction)}});
    const DVec3 d = listener - at;
    return {{"distance", std::sqrt(dot(d, d))},
            {"taps", std::move(taps)},
            {"occluded", h.occluded},
            {"openness", h.openness},
            {"mean_free_path", h.mean_free_path},
            {"hardness", h.hardness},
            {"reverb_send", h.reverb_send},
            {"reverb_seconds", h.reverb_seconds},
            {"casts", h.casts}};
}

Json play_properties() {
    return {{"sound", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64},
                       {"description", "A sound's name (get_audio lists them). Default gunshot."}}},
            {"at", point_schema("Where it is made, world metres [x, y, z].")},
            {"loudness", {{"type", "number"}, {"minimum", 0}, {"maximum", 10000},
                          {"description", "Its level at 1 m (1: the sound as made; a rifle about 100). Default 1."}}},
            {"rays", {{"type", "integer"}, {"minimum", 0}, {"maximum", 256},
                      {"description", "Echo rays cast from it (default 64; 0: the direct way only)."}}},
            {"max_distance", {{"type", "number"}, {"minimum", 1}, {"maximum", 20000},
                              {"description", "Metres an echoing surface may be from it (default 3000)."}}}};
}

struct PlayArgs {
    audio::SoundId sound = audio::k_no_sound;
    DVec3 at{};
    float loudness = 1.0f;
    audio::EchoSettings settings;
};

std::optional<PlayArgs> play_args(const Json& args, const AudioState& audio, std::string* error) {
    PlayArgs p;
    const std::string name = args.value("sound", std::string("gunshot"));
    const std::optional<audio::SoundId> id = audio.find(name);
    if (!id) {
        *error = "no sound called " + name + " (get_audio lists them)";
        return std::nullopt;
    }
    p.sound = *id;
    p.at = point_of(args["at"]);
    p.loudness = args.value("loudness", 1.0f);
    p.settings.rays = args.value("rays", 64u);
    p.settings.max_distance = args.value("max_distance", 3000.0);
    return p;
}

const char* k_heard_doc =
    "Returns how it is heard: `distance`, `taps` (the direct way first, then echoes, loudest first: `delay` "
    "seconds after it was made, `gain` against its level at 1 m, `lowpass` Hz, `direction` from the "
    "listener towards where it is heard from), `occluded` (something stands on the direct way: quieter, "
    "duller), `openness` (the share of rays that met nothing), `mean_free_path` (metres to what they met), "
    "`hardness`, `reverb_send` and `reverb_seconds` (the tail's level and length) and `casts`.";

} // namespace

void register_audio_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"play_sound",
         std::string("Plays a sound made at `at`, heard at the camera as the world shapes it: late by its "
                     "distance over the speed of sound (343 m/s), quieter with distance, its highs taken by the "
                     "air (far shots rumble), muffled through what stands between, and echoed by what is round "
                     "it (rays from it meet fixed bodies, kits and the ground near through the physics, the "
                     "terrain far: a cliff answers seconds later from its side; rock and concrete echo more than "
                     "soil and leaves), with a reverb tail as long as the place is big and hard. ") +
             k_heard_doc,
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"at"})},
              {"properties", play_properties()}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.audio == nullptr) return CommandResult::failure("audio is not available");
             std::string error;
             const std::optional<PlayArgs> p = play_args(args, *state.audio, &error);
             if (!p) return CommandResult::failure(error);
             state.audio->update(state.camera, 0.0); // heard where the camera is now
             if (state.physics != nullptr) state.physics->flush(state.scene);
             const audio::Heard heard =
                 state.audio->play(state.scene, state.physics, p->sound, p->at, p->loudness, p->settings);
             Json out = heard_json(heard, p->at, state.audio->listener());
             out["device"] = state.audio->device();
             return CommandResult::success(std::move(out));
         }});

    Json record_props = play_properties();
    record_props["seconds"] = {{"type", "number"}, {"minimum", 0.01}, {"maximum", 60},
                               {"description", "Seconds recorded from when it is made (default 4)."}};
    record_props["path"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024},
                            {"description", "The .wav file to write (16-bit stereo, 48 kHz)."}};
    add(registry,
        {"record_audio",
         std::string("As play_sound, but the sound alone is rendered for `seconds` into a WAV file at `path` "
                     "instead of the speakers (to listen to, or check: what arrives when). Adds `path`, `peak` "
                     "(of the recording, 0 to 1) and `first_heard` (seconds until it is first heard, -1: never). ") +
             k_heard_doc,
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"at", "path"})},
              {"properties", record_props}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.audio == nullptr) return CommandResult::failure("audio is not available");
             std::string error;
             const std::optional<PlayArgs> p = play_args(args, *state.audio, &error);
             if (!p) return CommandResult::failure(error);
             state.audio->update(state.camera, 0.0); // heard where the camera is now
             if (state.physics != nullptr) state.physics->flush(state.scene);
             const AudioState::Recording r = state.audio->record(state.scene, state.physics, p->sound, p->at,
                                                                 p->loudness, p->settings, args.value("seconds", 4.0));
             const std::string path = args["path"].get<std::string>();
             if (!audio::save_wav(path, r.frames, 2)) return CommandResult::failure("could not write " + path);
             Json out = heard_json(r.heard, p->at, state.audio->listener());
             out["path"] = path;
             out["peak"] = r.peak;
             out["first_heard"] = r.first_heard;
             return CommandResult::success(std::move(out));
         }});

    add(registry,
        {"load_sound",
         "Loads a WAV file (16-bit PCM or 32-bit float, mono or stereo, any rate: mixed to mono at 48 kHz) as "
         "the sound `name`, for play_sound. A name, once given, is kept (256 sounds at most).",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"name", "path"})},
              {"properties",
               {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                {"path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.audio == nullptr) return CommandResult::failure("audio is not available");
             const std::string name = args["name"].get<std::string>();
             const std::string error = state.audio->load(name, args["path"].get<std::string>());
             if (!error.empty()) return CommandResult::failure(error);
             const audio::Sound* s = state.audio->mixer().sound(*state.audio->find(name));
             return CommandResult::success({{"name", name}, {"seconds", s != nullptr ? s->seconds() : 0.0}});
         }});

    add(registry,
        {"stop_sounds", "Stops every sound playing (and the reverb tail).",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}}, false,
         [&state](const Json&, CommandContext&) {
             if (state.audio == nullptr) return CommandResult::failure("audio is not available");
             state.audio->mixer().stop_all();
             return CommandResult::success({{"stopped", true}});
         }});

    add(registry,
        {"get_audio",
         "The sound: `device` (the speakers' driver; empty: none, the mix runs silent), the sounds by name, "
         "the listener (the camera), the mixer (`voices` playing, `taps` in them, `frames` rendered, "
         "`dropped` plays, voices `stolen` (cut short for a play no quieter than what they had left), `peak` of the last render before the limiter, `reverb_seconds`), sounds `played` "
         "and the last one's `heard` (as play_sound returns).",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}}, false,
         [&state](const Json&, CommandContext&) {
             if (state.audio == nullptr) return CommandResult::failure("audio is not available");
             const AudioState& a = *state.audio;
             const audio::MixerStats s = a.mixer().stats();
             Json out = {{"device", a.device_name()},
                         {"sounds", a.names()},
                         {"listener", dvec(a.listener())},
                         {"voices", s.voices},
                         {"taps", s.taps},
                         {"frames", s.frames},
                         {"dropped", s.dropped},
                         {"stolen", s.stolen},
                         {"peak", s.peak},
                         {"reverb_seconds", s.reverb_seconds},
                         {"played", a.played()},
                         {"speed_of_sound", audio::k_speed_of_sound}};
             if (a.last()) {
                 out["heard"] = heard_json(*a.last(), a.listener(), a.listener());
                 out["heard"].erase("distance"); // (where it was made is not kept)
             }
             return CommandResult::success(std::move(out));
         }});
}

} // namespace giant::editor
