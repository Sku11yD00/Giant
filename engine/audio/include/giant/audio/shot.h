// What a shot sounds like where one stands. The report leaves the muzzle
// as any sound does (propagation.h). A bullet faster than sound also drags
// a shock wave behind it, a cone at the Mach angle: one it passes hears a
// crack from the point of the flight the cone left as it swept by them,
// earlier than the report (a rifle round at 300 m cracks past almost half
// a second before the bang arrives), and from upstream of where the bullet
// was when it passed (so the crack seems to come from off to the side, not
// from the shooter). One behind the shooter, outside the cone (far out to
// the side near the muzzle), or past where the bullet stopped before its
// cone reached them hears no crack; one ahead of where it slowed under
// the speed of sound hears the shock from about there.
//
//   const audio::Crack c = audio::trace_crack({muzzle, velocity, drag, stopped_at}, listener);
//   if (c.heard) mixer.play({crack, {Tap{c.delay, c.gain * loudness, ...}}});
//
// The flight is taken straight along its aim (gravity's drop is left out:
// a metre or so over the supersonic part of a rifle's flight), slowing by
// quadratic drag (v = v0 e^(-drag s)). Cost: a fixed 80-step search.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

namespace giant::audio {

struct Flight {
    DVec3 muzzle{};
    Vec3 velocity{};          // m/s
    float drag = 0.0007f;     // 1/m (sim::BallisticsSettings::drag)
    double range = 4000.0;    // metres flown before it stopped (or was spent)
};

struct Crack {
    bool heard = false;
    double delay = 0.0;       // seconds after the shot
    double miss = 0.0;        // metres from where its shock left the flight to the listener
    DVec3 from{};             // that point
    Vec3 direction{0, 0, 1};  // from the listener towards it
    double speed = 0.0;       // the bullet's there, m/s
    double distance = 0.0;    // metres down the flight there
    // Its level against the crack at 1 m (an N-wave falls as distance^-3/4, with what the air takes).
    float gain = 0.0f;
};

// Metres along the flight while it is faster than sound (0: never).
double supersonic_range(const Flight& flight);
Crack trace_crack(const Flight& flight, const DVec3& listener, float air = 1.0f);

} // namespace giant::audio
