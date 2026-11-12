// Play mode: walking the scene as the player, in the editor's viewport
// (Unreal's Play in Editor, without leaving the editor).
//
// Starting puts a player character (giant/sim) on the ground under the
// camera, turns simulating on and looks through the player's eyes: the
// mouse looks, W A S D walk, Shift runs, Space jumps, Esc stops. Walked
// into a wall too steep to walk (a cliff, a building's side), the player
// climbs it: W up, S down, A and D along it; at the top it pulls itself up
// onto the ledge, and Space pushes off. Something can carry the player (an
// entity it is held to, later a giant's hand): it goes where that goes
// until let go. Stopping
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
    bool climb = true; // climbs walls walked into
    float up = 0.0f;   // swimming: -1 (dive, C) to 1 (up to the surface, Space)
};

struct PlayerSettings {
    float walk_speed = 4.0f;   // metres a second
    float sprint_speed = 8.0f;
    float jump_speed = 5.0f;   // up, metres a second: about 1.3 m high
    float eye_height = 1.65f;  // above the feet
    sim::CharacterDesc body;   // height, radius, steps and slopes (its position is set on starting)
};

// Where a game save had the player (GameSaves): put back so, not on what
// is under the camera.
struct PlayerStart {
    DVec3 feet{};
    Vec3 velocity{};
    std::optional<Vec3> climbing; // on a wall: its normal (flat, out of it)
    std::optional<world::EntityId> carried_by;
    EditorCamera returns_to;      // where stopping puts the camera
    bool was_simulating = false;  // and simulating as it was
};

class PlayState {
public:
    explicit PlayState(PhysicsState& physics, const PlayerSettings& settings = {});

    bool playing() const { return m_player != sim::k_no_character; }
    // Starts: the player stands on what is under the camera (or falls from
    // there, if nothing is), looking where the camera looks. Bodies still
    // being made (a world just opened, entities just added) are waited for
    // without stalling a frame: the player is held at the camera until
    // they are in (at most five seconds), then put on what is under it.
    // False when it could not be put there (the physics world refused it).
    // `at`: as a game save had it (its feet, speed, wall and carrier; the
    // camera and simulating to go back to), once what it stands on is in.
    bool start(world::Scene& scene, EditorCamera& camera, u64 scene_generation, const PlayerStart* at = nullptr);
    // What stopping goes back to (a game save keeps them).
    const EditorCamera& returns_to() const { return m_saved_camera; }
    // A game save's start not put down yet (still placing), else none.
    const std::optional<PlayerStart>& pending_start() const { return m_start; }
    // Carried again from `feet` (its carrier just put back where a save had
    // it: held as the save had it against it, not where it was before).
    bool carry_from(const DVec3& feet, world::EntityId entity);
    bool was_simulating() const { return m_was_simulating; }
    // Stops: the camera back where it was, simulating as it was.
    void stop(EditorCamera& camera);
    // Esc: stops play (true), or is kept for the game's menus to take (false).
    bool escape_stops = true;
    // Esc pressed since last asked (only kept while escape_stops is false).
    bool take_escape();
    // Every key let go of (a menu opened over play: none stays held).
    void release_keys();
    // How far the mouse turns the view (1: 0.15 degrees a point).
    float look_sensitivity = 1.0f;

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
    bool before_step(const world::Scene& scene, EditorCamera& camera, u64 scene_generation, double seconds);
    // After the physics steps: the camera at the player's eyes, drawn
    // between the last two fixed steps (smooth at any frame rate), a step
    // up eased (`seconds` since the last call), not snapped.
    void after_step(EditorCamera& camera, double seconds);

    // Carried by `entity`'s body (a hand, a cart): held where it is against
    // it, going wherever it goes, its input left aside, until released (or
    // the body goes: the entity removed, or its body made again). False
    // when not playing, still being put down (waiting_for_ground just
    // after starting), or the entity has no body.
    bool carry(world::EntityId entity);
    void release();
    // What carries the player: an entity's id, or nothing.
    std::optional<world::EntityId> carried_by() const;

    std::optional<sim::CharacterState> player() const;
    sim::BodyId player_body() const;
    const PlayerInput& input() const { return m_input; }
    const PlayerSettings& settings() const { return m_settings; }
    // Held still: what it stands on is still being made.
    bool waiting_for_ground() const { return m_waiting || m_placing; }
    bool placing() const { return m_placing; }

private:
    void apply_input();
    // On what is under `eye` (within a kilometre), else hanging from it.
    DVec3 spawn_at(const DVec3& eye) const;
    // Starting waits for this: entities to make, or the ground under `eye`.
    bool placing_waits(const world::Scene& scene, const DVec3& eye) const;
    // Put down where it is to start: on what is under the eyes, or as a game save had it.
    void put_down();

    PhysicsState& m_physics;
    PlayerSettings m_settings;
    sim::CharacterId m_player = sim::k_no_character;
    u64 m_generation = 0;
    EditorCamera m_saved_camera;
    bool m_was_simulating = false;
    PlayerInput m_input;
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
    std::array<bool, 6> m_keys{}; // W S A D, Space C
    bool m_shift = false;
    bool m_space = false;
    bool m_escape = false; // Esc pressed, for the menus
    bool m_stop = false; // Esc pressed
    bool m_from_keys = false; // the keys, not set_input, give the input
    bool m_up_from_keys = false; // Space and C give `up` (pressed since set_input)
    double m_eye = 0.0; // the camera's height shown (eased up steps)
    bool m_waiting = false;
    bool m_placing = false;   // started, waiting for the bodies before standing
    double m_placing_for = 0.0; // seconds of it
    DVec3 m_place_eye{};        // where its eyes are to start (what placing waits under)
    std::optional<PlayerStart> m_start; // a game save's, until put down
};

} // namespace giant::editor
