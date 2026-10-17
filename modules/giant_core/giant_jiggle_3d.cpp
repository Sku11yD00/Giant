/**************************************************************************/
/*  giant_jiggle_3d.cpp                                                   */
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

#include "giant_jiggle_3d.h"

#include "giant_body.h"

#include "core/object/class_db.h"

// Body-time step the simulation aims for. Each frame is split into equal substeps
// no longer than this, so results barely depend on the frame rate.
static const double TARGET_STEP = 1.0 / 120.0;
static const int MAX_SUBSTEPS = 8;
// A jump of more than this many body lengths in one frame is a teleport, not motion.
static const real_t TELEPORT_DISTANCE = 1.0;

void GiantJiggleChain::set_root_bone(const StringName &p_bone) {
	root_bone = p_bone;
	emit_changed();
}

StringName GiantJiggleChain::get_root_bone() const {
	return root_bone;
}

void GiantJiggleChain::set_bone_count(int p_count) {
	bone_count = MAX(p_count, 1);
	emit_changed();
}

int GiantJiggleChain::get_bone_count() const {
	return bone_count;
}

void GiantJiggleChain::set_frequency(real_t p_frequency) {
	frequency = MAX(p_frequency, 0.0);
	emit_changed();
}

real_t GiantJiggleChain::get_frequency() const {
	return frequency;
}

void GiantJiggleChain::set_damping_ratio(real_t p_ratio) {
	damping_ratio = MAX(p_ratio, 0.0);
	emit_changed();
}

real_t GiantJiggleChain::get_damping_ratio() const {
	return damping_ratio;
}

void GiantJiggleChain::set_gravity(real_t p_gravity) {
	gravity = p_gravity;
	emit_changed();
}

real_t GiantJiggleChain::get_gravity() const {
	return gravity;
}

void GiantJiggleChain::set_max_angle(real_t p_angle) {
	max_angle = CLAMP(p_angle, 0.0, Math::PI);
	emit_changed();
}

real_t GiantJiggleChain::get_max_angle() const {
	return max_angle;
}

void GiantJiggleChain::set_tip_length(real_t p_length) {
	tip_length = MAX(p_length, 0.0);
	emit_changed();
}

real_t GiantJiggleChain::get_tip_length() const {
	return tip_length;
}

void GiantJiggleChain::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_root_bone", "bone"), &GiantJiggleChain::set_root_bone);
	ClassDB::bind_method(D_METHOD("get_root_bone"), &GiantJiggleChain::get_root_bone);
	ClassDB::bind_method(D_METHOD("set_bone_count", "count"), &GiantJiggleChain::set_bone_count);
	ClassDB::bind_method(D_METHOD("get_bone_count"), &GiantJiggleChain::get_bone_count);
	ClassDB::bind_method(D_METHOD("set_frequency", "frequency"), &GiantJiggleChain::set_frequency);
	ClassDB::bind_method(D_METHOD("get_frequency"), &GiantJiggleChain::get_frequency);
	ClassDB::bind_method(D_METHOD("set_damping_ratio", "ratio"), &GiantJiggleChain::set_damping_ratio);
	ClassDB::bind_method(D_METHOD("get_damping_ratio"), &GiantJiggleChain::get_damping_ratio);
	ClassDB::bind_method(D_METHOD("set_gravity", "gravity"), &GiantJiggleChain::set_gravity);
	ClassDB::bind_method(D_METHOD("get_gravity"), &GiantJiggleChain::get_gravity);
	ClassDB::bind_method(D_METHOD("set_max_angle", "angle"), &GiantJiggleChain::set_max_angle);
	ClassDB::bind_method(D_METHOD("get_max_angle"), &GiantJiggleChain::get_max_angle);
	ClassDB::bind_method(D_METHOD("set_tip_length", "length"), &GiantJiggleChain::set_tip_length);
	ClassDB::bind_method(D_METHOD("get_tip_length"), &GiantJiggleChain::get_tip_length);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "root_bone"), "set_root_bone", "get_root_bone");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bone_count", PROPERTY_HINT_RANGE, "1,64,1"), "set_bone_count", "get_bone_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "frequency", PROPERTY_HINT_RANGE, "0,20,0.01,or_greater,suffix:Hz"), "set_frequency", "get_frequency");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "damping_ratio", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_damping_ratio", "get_damping_ratio");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gravity", PROPERTY_HINT_RANGE, "-20,20,0.01,or_less,or_greater,suffix:m/s²"), "set_gravity", "get_gravity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_angle", PROPERTY_HINT_RANGE, "0,180,0.1,radians_as_degrees"), "set_max_angle", "get_max_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tip_length", PROPERTY_HINT_RANGE, "0,10,0.001,or_greater,suffix:m"), "set_tip_length", "get_tip_length");
}

void GiantJiggle3D::_build_joints(Skeleton3D *p_skeleton, const Ref<GiantJiggleChain> &p_chain, LocalVector<Joint> &r_joints) const {
	r_joints.clear();
	if (p_chain.is_null()) {
		return;
	}
	int bone = p_skeleton->find_bone(p_chain->get_root_bone());
	for (int i = 0; i < p_chain->get_bone_count() && bone >= 0; i++) {
		Vector<int> children = p_skeleton->get_bone_children(bone);
		int next = children.is_empty() ? -1 : children[0];

		Joint joint;
		joint.bone = bone;
		if (next >= 0) {
			joint.tail_offset = p_skeleton->get_bone_rest(next).origin;
		} else {
			// No child bone: extend the bone along the direction it leaves its parent.
			Vector3 dir = p_skeleton->get_bone_rest(bone).origin;
			real_t length = p_chain->get_tip_length() > 0 ? p_chain->get_tip_length() : dir.length();
			joint.tail_offset = dir.is_zero_approx() ? Vector3(0, length, 0) : dir.normalized() * length;
		}
		if (joint.tail_offset.is_zero_approx()) {
			break;
		}
		r_joints.push_back(joint);
		bone = next;
	}
}

bool GiantJiggle3D::_joints_match(Skeleton3D *p_skeleton, const Ref<GiantJiggleChain> &p_chain, const LocalVector<Joint> &p_joints) const {
	LocalVector<Joint> fresh;
	_build_joints(p_skeleton, p_chain, fresh);
	if (fresh.size() != p_joints.size()) {
		return false;
	}
	for (uint32_t i = 0; i < fresh.size(); i++) {
		if (fresh[i].bone != p_joints[i].bone || !fresh[i].tail_offset.is_equal_approx(p_joints[i].tail_offset)) {
			return false;
		}
	}
	return true;
}

void GiantJiggle3D::_carry_with_frame(Skeleton3D *p_skeleton, double p_body_delta) {
	// Body space is skeleton space without the giant's scale. When the giant speeds up,
	// slows down or turns, the tails feel it as inertia, measured in body lengths and
	// body time so it matches at every size. Growing changes the scale only, so it
	// moves nothing.
	Transform3D frame = p_skeleton->get_global_transform();
	Basis rotation = frame.basis.orthonormalized();
	real_t scale = Math::pow(Math::abs(frame.basis.determinant()), (real_t)(1.0 / 3.0));
	frame_accel = Vector3();
	if (scale <= CMP_EPSILON) {
		return;
	}
	if (frame_valid) {
		Vector3 moved = (frame.origin - frame_origin) / scale;
		Basis inv = rotation.transposed();
		if (inv.xform(moved).length() > TELEPORT_DISTANCE) {
			reset();
		} else {
			// Turning: keep each tail where it was in the world, so it trails behind.
			Basis turn = inv * frame_rotation;
			for (LocalVector<Joint> &joints : chain_joints) {
				for (Joint &joint : joints) {
					joint.position = turn.xform(joint.position);
					joint.velocity = turn.xform(joint.velocity);
				}
			}
			// Moving: a change in the giant's speed pushes the tails the other way.
			if (p_body_delta > 0.0) {
				Vector3 velocity = moved / p_body_delta;
				if (frame_velocity_valid) {
					frame_accel = -inv.xform(velocity - frame_velocity) / p_body_delta;
				}
				frame_velocity = velocity;
				frame_velocity_valid = true;
			}
		}
	}
	if (!frame_valid) {
		// Assume the giant starts at rest.
		frame_velocity = Vector3();
		frame_velocity_valid = true;
	}
	frame_rotation = rotation;
	frame_origin = frame.origin;
	frame_valid = true;
}

void GiantJiggle3D::_simulate_chain(Skeleton3D *p_skeleton, const Ref<GiantJiggleChain> &p_chain, LocalVector<Joint> &p_joints, double p_body_delta, const Vector3 &p_down) {
	const real_t omega = Math::TAU * p_chain->get_frequency();
	const real_t stiffness = omega * omega;
	const real_t damping = 2.0 * p_chain->get_damping_ratio() * omega;
	const Vector3 gravity = p_down * p_chain->get_gravity() + frame_accel;
	const real_t max_angle = p_chain->get_max_angle();

	int steps = 0;
	double h = 0.0;
	if (p_body_delta > 0.0) {
		steps = CLAMP(int(Math::ceil(p_body_delta / TARGET_STEP)), 1, MAX_SUBSTEPS);
		h = p_body_delta / steps;
	}

	for (Joint &joint : p_joints) {
		// Parents earlier in the chain are already posed, so this is the pose to spring back to.
		Transform3D pose = p_skeleton->get_bone_global_pose(joint.bone);
		Vector3 head = pose.origin;
		Vector3 target = pose.xform(joint.tail_offset);
		Vector3 rest_dir = target - head;
		real_t length = rest_dir.length();
		if (length <= CMP_EPSILON) {
			continue;
		}
		rest_dir /= length;

		for (int i = 0; i < steps; i++) {
			Vector3 start = joint.position;
			Vector3 accel = (target - joint.position) * stiffness - joint.velocity * damping + gravity;
			joint.velocity += accel * h;
			Vector3 p = joint.position + joint.velocity * h;

			// Keep the bone length, and keep the bone within its swing limit.
			Vector3 dir = p - head;
			dir = dir.is_zero_approx() ? rest_dir : dir.normalized();
			real_t angle = rest_dir.angle_to(dir);
			if (angle > max_angle) {
				Vector3 axis = rest_dir.cross(dir);
				dir = axis.is_zero_approx() ? rest_dir : rest_dir.rotated(axis.normalized(), max_angle);
			}
			joint.position = head + dir * length;
			joint.velocity = (joint.position - start) / h;
		}
		if (steps == 0) {
			// No time passed: keep the current swing but follow the bone's new head.
			Vector3 dir = joint.position - head;
			joint.position = head + (dir.is_zero_approx() ? rest_dir : dir.normalized()) * length;
		}

		Vector3 sim_dir = (joint.position - head).normalized();
		Quaternion swing = Quaternion(rest_dir, sim_dir);
		Quaternion global_rotation = swing * pose.basis.get_rotation_quaternion();
		p_skeleton->set_bone_pose_rotation(joint.bone, get_local_pose_rotation(p_skeleton, joint.bone, global_rotation));
	}
}

void GiantJiggle3D::_process_modification(double p_delta) {
	Skeleton3D *skeleton = get_skeleton();
	if (!skeleton) {
		return;
	}

	chain_joints.resize(chains.size());
	for (int i = 0; i < chains.size(); i++) {
		Ref<GiantJiggleChain> chain = chains[i];
		if (!_joints_match(skeleton, chain, chain_joints[i])) {
			_build_joints(skeleton, chain, chain_joints[i]);
			// Start at rest on the current pose.
			for (Joint &joint : chain_joints[i]) {
				joint.position = Vector3(NAN, NAN, NAN);
			}
		}
	}

	double body_delta = p_delta * get_effective_time_scale();
	_carry_with_frame(skeleton, body_delta);

	Basis world_to_body = frame_rotation.transposed();
	Vector3 down = world_to_body.xform(Vector3(0, -1, 0));

	for (int i = 0; i < chains.size(); i++) {
		LocalVector<Joint> &joints = chain_joints[i];
		for (Joint &joint : joints) {
			if (!joint.position.is_finite()) {
				// Joints are posed in order, so this sees the parents already in place.
				joint.position = skeleton->get_bone_global_pose(joint.bone).xform(joint.tail_offset);
				joint.velocity = Vector3();
			}
		}
		_simulate_chain(skeleton, chains[i], joints, body_delta, down);
	}
}

void GiantJiggle3D::_set_active(bool p_active) {
	if (p_active) {
		reset();
	}
}

void GiantJiggle3D::set_chains(const TypedArray<GiantJiggleChain> &p_chains) {
	chains = p_chains;
	reset();
}

TypedArray<GiantJiggleChain> GiantJiggle3D::get_chains() const {
	return chains;
}

void GiantJiggle3D::set_time_scale_override(real_t p_scale) {
	time_scale_override = p_scale;
}

real_t GiantJiggle3D::get_time_scale_override() const {
	return time_scale_override;
}

real_t GiantJiggle3D::get_effective_time_scale() const {
	if (time_scale_override >= 0.0) {
		return time_scale_override;
	}
	GiantBody *body = GiantBody::find_for_node(this);
	return body ? body->get_time_scale() : 1.0;
}

void GiantJiggle3D::reset() {
	for (LocalVector<Joint> &joints : chain_joints) {
		for (Joint &joint : joints) {
			joint.position = Vector3(NAN, NAN, NAN);
			joint.velocity = Vector3();
		}
	}
	frame_valid = false;
	frame_velocity_valid = false;
}

Vector3 GiantJiggle3D::get_joint_position(int p_chain, int p_joint) const {
	ERR_FAIL_INDEX_V(p_chain, (int)chain_joints.size(), Vector3());
	ERR_FAIL_INDEX_V(p_joint, (int)chain_joints[p_chain].size(), Vector3());
	return chain_joints[p_chain][p_joint].position;
}

void GiantJiggle3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_chains", "chains"), &GiantJiggle3D::set_chains);
	ClassDB::bind_method(D_METHOD("get_chains"), &GiantJiggle3D::get_chains);
	ClassDB::bind_method(D_METHOD("set_time_scale_override", "scale"), &GiantJiggle3D::set_time_scale_override);
	ClassDB::bind_method(D_METHOD("get_time_scale_override"), &GiantJiggle3D::get_time_scale_override);
	ClassDB::bind_method(D_METHOD("get_effective_time_scale"), &GiantJiggle3D::get_effective_time_scale);
	ClassDB::bind_method(D_METHOD("reset"), &GiantJiggle3D::reset);
	ClassDB::bind_method(D_METHOD("get_joint_position", "chain", "joint"), &GiantJiggle3D::get_joint_position);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "chains", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("GiantJiggleChain")), "set_chains", "get_chains");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_scale_override", PROPERTY_HINT_RANGE, "-1,10,0.001"), "set_time_scale_override", "get_time_scale_override");
}
