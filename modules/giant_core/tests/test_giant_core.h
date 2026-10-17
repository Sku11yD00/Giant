/**************************************************************************/
/*  test_giant_core.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "../giant_body.h"
#include "../giant_footsteps_3d.h"
#include "../giant_jiggle_3d.h"

#include "core/object/callable_mp.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "tests/test_macros.h"

namespace TestGiantCore {

TEST_CASE("[Giant] Time scale follows the time feel") {
	// Human size never slows down.
	CHECK(GiantBody::compute_time_scale(1.0, 1.0) == doctest::Approx(1.0));
	// Feel 0 moves like a human at every size.
	CHECK(GiantBody::compute_time_scale(100.0, 0.0) == doctest::Approx(1.0));
	// Feel 1 is real physics: 100 times bigger swings 10 times slower.
	CHECK(GiantBody::compute_time_scale(100.0, 1.0) == doctest::Approx(0.1));
	// The default sits in between.
	CHECK(GiantBody::compute_time_scale(100.0, 0.5) == doctest::Approx(Math::pow(100.0, -0.25)));
}

TEST_CASE("[Giant] Growth is steady in log space and lands on the target") {
	CHECK(GiantBody::compute_growth_height(2.0, 200.0, 0.0) == doctest::Approx(2.0));
	CHECK(GiantBody::compute_growth_height(2.0, 200.0, 0.5) == doctest::Approx(20.0));
	CHECK(GiantBody::compute_growth_height(2.0, 200.0, 1.0) == doctest::Approx(200.0));
	CHECK(GiantBody::compute_growth_height(200.0, 2.0, 1.0) == doctest::Approx(2.0));
}

TEST_CASE("[SceneTree][Giant] GiantBody scales with height and grows over time") {
	GiantBody *body = memnew(GiantBody);
	SceneTree::get_singleton()->get_root()->add_child(body);

	body->set_rest_height(2.0);
	body->set_height(100.0);
	CHECK(body->get_scale_factor() == doctest::Approx(50.0));
	CHECK(body->get_scale().x == doctest::Approx(50.0));

	body->grow_to(1000.0, 4.0);
	CHECK(body->is_growing());
	for (int i = 0; i < 8; i++) {
		body->advance(0.25);
	}
	CHECK(body->get_height() == doctest::Approx(Math::sqrt(100.0 * 1000.0))); // Halfway in log space.
	for (int i = 0; i < 8; i++) {
		body->advance(0.25);
	}
	CHECK_FALSE(body->is_growing());
	CHECK(body->get_height() == doctest::Approx(1000.0));

	memdelete(body);
}

// A giant with a sideways tail of three bones, for the jiggle tests.
struct JiggleRig {
	GiantBody *body = nullptr;
	Skeleton3D *skeleton = nullptr;
	GiantJiggle3D *jiggle = nullptr;

	JiggleRig(real_t p_height) {
		body = memnew(GiantBody);
		body->set_rest_height(1.8);
		body->set_height(p_height);
		body->set_time_feel(1.0);

		skeleton = memnew(Skeleton3D);
		skeleton->add_bone("Root");
		skeleton->add_bone("Tail0");
		skeleton->add_bone("Tail1");
		skeleton->add_bone("Tail2");
		skeleton->add_bone("TailEnd");
		skeleton->set_bone_parent(1, 0);
		skeleton->set_bone_parent(2, 1);
		skeleton->set_bone_parent(3, 2);
		skeleton->set_bone_parent(4, 3);
		skeleton->set_bone_rest(0, Transform3D());
		skeleton->set_bone_rest(1, Transform3D(Basis(), Vector3(0, 1.0, 0)));
		for (int i = 2; i < 5; i++) {
			skeleton->set_bone_rest(i, Transform3D(Basis(), Vector3(0.3, 0, 0)));
		}
		skeleton->reset_bone_poses();

		jiggle = memnew(GiantJiggle3D);
		Ref<GiantJiggleChain> chain;
		chain.instantiate();
		chain->set_root_bone("Tail0");
		chain->set_bone_count(3);
		chain->set_frequency(1.5);
		chain->set_damping_ratio(0.2);
		chain->set_gravity(9.8);
		TypedArray<GiantJiggleChain> chains;
		chains.push_back(chain);
		jiggle->set_chains(chains);

		body->add_child(skeleton);
		skeleton->add_child(jiggle);
		SceneTree::get_singleton()->get_root()->add_child(body);
	}

	~JiggleRig() {
		memdelete(body);
	}

	// One frame: the animation resets the pose, then the jiggle runs on top.
	void step(double p_real_delta) {
		skeleton->reset_bone_poses();
		jiggle->process_modification(p_real_delta);
	}

	// Moves the giant sideways by p_body_offset body lengths.
	void place(real_t p_body_offset) {
		body->set_position(Vector3(p_body_offset * body->get_scale_factor(), 0, 0));
	}
};

// Sways the giant from side to side for p_body_seconds of body time, then
// returns where the tail tip ended up, in body space.
static Vector3 _run_sway(JiggleRig &p_rig, double p_body_seconds, int p_frames) {
	double body_dt = p_body_seconds / p_frames;
	double real_dt = body_dt / p_rig.body->get_time_scale();
	for (int i = 0; i <= p_frames; i++) {
		p_rig.place(0.4 * Math::sin(Math::TAU * 0.8 * body_dt * i));
		p_rig.step(i == 0 ? 0.0 : real_dt);
	}
	return p_rig.jiggle->get_joint_position(0, 2);
}

TEST_CASE("[SceneTree][Giant] Jiggle has the same shape at human size and at 1 km") {
	JiggleRig human(1.8);
	JiggleRig giant(1800.0);
	CHECK(giant.body->get_time_scale() == doctest::Approx(Math::pow(1000.0, -0.5)));

	Vector3 human_tip = _run_sway(human, 2.0, 240);
	Vector3 giant_tip = _run_sway(giant, 2.0, 240);

	// The motion really did swing the tail away from where it hangs at rest...
	Vector3 rest_tip = human.skeleton->get_bone_global_rest(4).origin;
	CHECK(human_tip.distance_to(rest_tip) > 0.05);
	// ...and the giant's tail followed exactly the same path in body space.
	CHECK(giant_tip.distance_to(human_tip) < 1e-3);
}

TEST_CASE("[SceneTree][Giant] Jiggle barely depends on the frame rate") {
	JiggleRig slow(1.8);
	JiggleRig fast(1.8);
	Vector3 tip_30 = _run_sway(slow, 1.5, 45);
	Vector3 tip_144 = _run_sway(fast, 1.5, 216);
	CHECK(tip_30.distance_to(tip_144) < 0.03);
}

TEST_CASE("[SceneTree][Giant] Growing does not shake the jiggle") {
	// Two identical giants with the same clock; only one of them grows 1000 times taller.
	JiggleRig still(1.8);
	JiggleRig growing(1.8);
	still.jiggle->set_time_scale_override(1.0);
	growing.jiggle->set_time_scale_override(1.0);
	for (int i = 0; i < 120; i++) {
		growing.body->set_height(1.8 * Math::pow(1000.0, (i + 1) / 120.0));
		still.step(1.0 / 60.0);
		growing.step(1.0 / 60.0);
	}
	CHECK(growing.jiggle->get_joint_position(0, 2).distance_to(still.jiggle->get_joint_position(0, 2)) < 1e-6);
}

static int footstep_count = 0;
static void _count_footstep(const StringName &p_foot, const Vector3 &p_position, real_t p_giant_height) {
	footstep_count++;
}

TEST_CASE("[SceneTree][Giant] Footsteps fire once per step at any size") {
	for (real_t height : { 1.8, 900.0 }) {
		GiantBody *body = memnew(GiantBody);
		body->set_rest_height(1.8);
		body->set_height(height);
		Skeleton3D *skeleton = memnew(Skeleton3D);
		skeleton->set_name("Skeleton");
		skeleton->add_bone("Hips");
		skeleton->add_bone("LeftFoot");
		skeleton->set_bone_parent(1, 0);
		skeleton->set_bone_rest(0, Transform3D(Basis(), Vector3(0, 0.9, 0)));
		skeleton->set_bone_rest(1, Transform3D(Basis(), Vector3(0.1, -0.82, 0)));
		skeleton->reset_bone_poses();
		GiantFootsteps3D *steps = memnew(GiantFootsteps3D);
		steps->set_foot_bones({ "LeftFoot" });
		body->add_child(skeleton);
		body->add_child(steps);
		steps->set_skeleton_path(NodePath("../Skeleton"));
		SceneTree::get_singleton()->get_root()->add_child(body);
		steps->connect("footstep", callable_mp_static(&_count_footstep));

		footstep_count = 0;
		for (int step = 0; step < 3; step++) {
			for (int f = 0; f <= 20; f++) {
				// Lift the foot 20 cm (in body lengths) and put it down again.
				real_t lift = 0.2 * Math::sin(Math::PI * f / 20.0);
				skeleton->set_bone_pose_position(1, Vector3(0.1, -0.82 + lift, 0));
				steps->update_feet();
			}
		}
		CHECK_MESSAGE(footstep_count == 3, vformat("height %f", height));
		memdelete(body);
	}
}

} // namespace TestGiantCore
