// Play mode: walking the scene as the player, in the editor's viewport
// (Unreal's Play in Editor, without leaving the editor).
//
// Starting puts a player character (giant/sim) on the ground under the
// camera, turns simulating on and looks through the player's eyes: the
// mouse looks, W A S D walk, Shift runs, Space jumps, Esc stops. Stopping
// takes the player away, puts the camera back where it was and simulating
// back as it was. Like simulating, it is view state: the scene is never
// touched. A new scene stops it.
#pragma once

#include "editor_commands.h"
#include "physics_state.h"

#include "giant/platform/input.h"

#include <array>
#include <optional>
#include <vector>

namespace giant::editor {

// What the player is told, held until told otherwise (the keys, or
// set_player_input).
struct PlayerInput {
    float forward = 0.0f; // -1 (back) to 1, along where it looks
    float right = 0.0f;   // -1 (left) to 1
    bool sprint = false;
};

struct PlayerSettings {
    float walk_speed = 4.0f;   // metres a second
    float sprint_speed = 8.0f;
    float jump_speed = 5.0f;   // up, metres a second: about 1.3 m high
    float eye_height = 1.65f;  // above the feet
    sim::CharacterDesc body;   // height, radius, steps and slopes (its position is set on starting)
};

class PlayState {
public:
    explicit PlayState(PhysicsState& physics, const PlayerSettings& settings = {});

    bool playing() const { return m_player != sim::k_no_character; }
    // Starts: the player stands on what is under the camera (or falls from
    // there, if nothing is), looking where the camera looks. False when it
    // could not be put there (the physics world refused it).
    bool start(world::Scene& scene, EditorCamera& camera, u64 scene_generation);
    // Stops: the camera back where it was, simulating as it was.
    void stop(EditorCamera& camera);

    // The keys and the mouse, while playing (Esc stops on the next update).
    // `ui_has_keyboard`: a panel's text field has the keys.
    void handle_input(const std::vector<platform::InputEvent>& events, bool ui_has_keyboard);
    void set_input(const PlayerInput& input);
    void jump();
    // Where the player looks, in degrees (as EditorCamera's).
    void look(float yaw_degrees, float pitch_degrees);
    float yaw() const { return m_yaw; }
    float pitch() const { return m_pitch; }

    // Each frame, before the physics steps: the player's wanted motion. A
    // new scene (another generation) stops play; true while still playing.
    bool before_step(const world::Scene& scene, EditorCamera& camera, u64 scene_generation);
    // After the physics steps: the camera at the player's eyes (`seconds`
    // since the last: a step up is eased, not snapped).
    void after_step(EditorCamera& camera, double seconds);

    std::optional<sim::CharacterState> player() const;
    sim::BodyId player_body() const;
    const PlayerInput& input() const { return m_input; }
    const PlayerSettings& settings() const { return m_settings; }
    // Held still in the air: the ground under it is still being made.
    bool waiting_for_ground() const { return m_waiting; }

private:
    void apply_input();

    PhysicsState& m_physics;
    PlayerSettings m_settings;
    sim::CharacterId m_player = sim::k_no_character;
    u64 m_generation = 0;
    EditorCamera m_saved_camera;
    bool m_was_simulating = false;
    PlayerInput m_input;
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
    std::array<bool, 4> m_keys{}; // W S A D
    bool m_shift = false;
    bool m_space = false;
    bool m_stop = false; // Esc pressed
    bool m_from_keys = false; // the keys, not set_input, give the input
    double m_eye = 0.0; // the camera's height shown (eased up steps)
    bool m_waiting = false;
    DVec3 m_held{}; // where it is held while the ground is made
};

} // namespace giant::editor
