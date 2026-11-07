#include "play_state.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

namespace {

using platform::InputEvent;
using platform::Key;

constexpr float k_degrees_per_point = 0.15f;
constexpr float k_degrees_to_radians = 0.017453292f;
// How fast the eyes catch up a step up (1/s).
constexpr double k_eye_ease = 14.0;

} // namespace

PlayState::PlayState(PhysicsState& physics, const PlayerSettings& settings) : m_physics(physics), m_settings(settings) {}

bool PlayState::start(world::Scene& scene, EditorCamera& camera, u64 scene_generation) {
    if (playing()) return true;
    m_physics.flush(scene); // everything there to stand on
    sim::CharacterDesc desc = m_settings.body;
    // On what is under the camera (within a kilometre), else from where it is.
    const DVec3 eye = camera.position;
    const auto hit = m_physics.world().raycast(eye, {0.0f, -1.0f, 0.0f}, 1000.0);
    desc.position = hit ? hit->point + DVec3(0.0, 0.02, 0.0)
                        : eye - DVec3(0.0, static_cast<double>(m_settings.eye_height), 0.0);
    m_player = m_physics.world().add_character(desc);
    if (m_player == sim::k_no_character) return false;
    m_generation = scene_generation;
    m_saved_camera = camera;
    m_was_simulating = m_physics.simulating();
    m_physics.set_simulating(true);
    m_input = {};
    m_keys = {};
    m_shift = m_space = m_stop = m_from_keys = false;
    m_waiting = false;
    m_yaw = camera.yaw_degrees;
    m_pitch = camera.pitch_degrees;
    m_eye = desc.position.y + m_settings.eye_height;
    after_step(camera, 0.0);
    return true;
}

void PlayState::stop(EditorCamera& camera) {
    if (!playing()) return;
    m_physics.world().remove_character(m_player);
    m_player = sim::k_no_character;
    m_physics.set_simulating(m_was_simulating);
    camera = m_saved_camera;
}

void PlayState::handle_input(const std::vector<InputEvent>& events, bool ui_has_keyboard) {
    if (!playing()) return;
    for (const InputEvent& e : events) {
        switch (e.type) {
        case InputEvent::Type::mouse_move:
            m_yaw = std::remainder(m_yaw - e.dx * k_degrees_per_point, 360.0f);
            m_pitch = std::clamp(m_pitch - e.dy * k_degrees_per_point, -89.0f, 89.0f);
            break;
        case InputEvent::Type::key: {
            m_shift = (e.modifiers & platform::modifier::shift) != 0;
            // Releases always count, so a key let go over a panel never sticks.
            if (ui_has_keyboard && e.down) break;
            const auto set = [&](usize i) {
                m_keys[i] = e.down;
                m_from_keys = true;
            };
            switch (e.key) {
            case Key::w: set(0); break;
            case Key::s: set(1); break;
            case Key::a: set(2); break;
            case Key::d: set(3); break;
            case Key::left_shift:
            case Key::right_shift: m_shift = e.down; break;
            case Key::space:
                if (e.down && !m_space) jump(); // a press, not the key repeating
                m_space = e.down;
                break;
            case Key::escape:
                if (e.down) m_stop = true;
                break;
            default: break;
            }
            break;
        }
        case InputEvent::Type::focus:
            if (!e.down) { // focus lost: no release will arrive
                m_keys = {};
                m_shift = m_space = false;
            }
            break;
        default: break;
        }
    }
    if (m_from_keys) {
        m_input.forward = (m_keys[0] ? 1.0f : 0.0f) - (m_keys[1] ? 1.0f : 0.0f);
        m_input.right = (m_keys[3] ? 1.0f : 0.0f) - (m_keys[2] ? 1.0f : 0.0f);
        m_input.sprint = m_shift;
    }
}

void PlayState::set_input(const PlayerInput& input) {
    m_input.forward = std::isfinite(input.forward) ? std::clamp(input.forward, -1.0f, 1.0f) : 0.0f;
    m_input.right = std::isfinite(input.right) ? std::clamp(input.right, -1.0f, 1.0f) : 0.0f;
    m_input.sprint = input.sprint;
    m_from_keys = false; // until a key is pressed
}

void PlayState::jump() {
    if (playing()) m_physics.world().jump(m_player, m_settings.jump_speed);
}

void PlayState::look(float yaw_degrees, float pitch_degrees) {
    if (std::isfinite(yaw_degrees)) m_yaw = std::remainder(yaw_degrees, 360.0f);
    if (std::isfinite(pitch_degrees)) m_pitch = std::clamp(pitch_degrees, -89.0f, 89.0f);
}

void PlayState::apply_input() {
    // Along where it looks, level: yaw 0 looks down -z, 90 down -x.
    const float yaw = m_yaw * k_degrees_to_radians;
    const Vec3 forward{-std::sin(yaw), 0.0f, -std::cos(yaw)};
    const Vec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};
    Vec3 move = forward * m_input.forward + right * m_input.right;
    const float length = std::sqrt(move.x * move.x + move.z * move.z);
    if (length > 1.0f) move = move * (1.0f / length); // diagonals no faster
    sim::CharacterInput in;
    in.velocity = move * (m_input.sprint ? m_settings.sprint_speed : m_settings.walk_speed);
    m_physics.world().set_character_input(m_player, in);
}

bool PlayState::before_step(const world::Scene& scene, EditorCamera& camera, u64 scene_generation) {
    if (!playing()) return false;
    if (m_stop || scene_generation != m_generation) {
        stop(camera);
        return false;
    }
    apply_input();
    // Ground on its way under it (streamed in, just edited): held where it
    // is until it is there, rather than falling through.
    const std::optional<sim::CharacterState> s = m_physics.world().character_state(m_player);
    const bool waiting = s && m_physics.bodies().ground_waiting(scene.terrain, s->position);
    if (waiting && !m_waiting) m_held = s->position;
    if (waiting) m_physics.world().set_character_position(m_player, m_held);
    m_waiting = waiting;
    return true;
}

void PlayState::after_step(EditorCamera& camera, double seconds) {
    const std::optional<sim::CharacterState> s = player();
    if (!s) return;
    const double eye = s->position.y + static_cast<double>(m_settings.eye_height);
    // Up a step (a small rise while on the ground): eased. Anything else
    // (falling, jumping, down a step) followed at once.
    const double rise = eye - m_eye;
    if (s->on_ground && rise > 0.0 && rise <= static_cast<double>(m_settings.body.step_height) + 0.05) {
        m_eye += rise * (1.0 - std::exp(-k_eye_ease * std::max(seconds, 0.0)));
    } else {
        m_eye = eye;
    }
    camera.position = {s->position.x, m_eye, s->position.z};
    camera.yaw_degrees = m_yaw;
    camera.pitch_degrees = m_pitch;
}

std::optional<sim::CharacterState> PlayState::player() const {
    if (!playing()) return std::nullopt;
    return m_physics.world().character_state(m_player);
}

sim::BodyId PlayState::player_body() const {
    return playing() ? m_physics.world().character_body(m_player) : sim::k_no_body;
}

} // namespace giant::editor
