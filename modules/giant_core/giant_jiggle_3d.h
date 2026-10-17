/**************************************************************************/
/*  giant_jiggle_3d.h                                                     */
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

#include "core/io/resource.h"
#include "scene/3d/skeleton_modifier_3d.h"

// One jiggling bone, or a chain of bones such as a ponytail.
class GiantJiggleChain : public Resource {
	GDCLASS(GiantJiggleChain, Resource);

	StringName root_bone;
	int bone_count = 1;
	real_t frequency = 2.5;
	real_t damping_ratio = 0.25;
	real_t gravity = 0.0;
	real_t max_angle = Math::deg_to_rad(60.0);
	real_t tip_length = 0.0;

protected:
	static void _bind_methods();

public:
	void set_root_bone(const StringName &p_bone);
	StringName get_root_bone() const;
	void set_bone_count(int p_count);
	int get_bone_count() const;
	void set_frequency(real_t p_frequency);
	real_t get_frequency() const;
	void set_damping_ratio(real_t p_ratio);
	real_t get_damping_ratio() const;
	void set_gravity(real_t p_gravity);
	real_t get_gravity() const;
	void set_max_angle(real_t p_angle);
	real_t get_max_angle() const;
	void set_tip_length(real_t p_length);
	real_t get_tip_length() const;
};

// Jiggle and hair that behave the same at every size.
//
// The simulation runs in the giant's own body space (skeleton space, before the
// giant's scale) and in the giant's own time (see GiantBody.time_feel). So a
// 1 km giant's jiggle has exactly the shape it has at human size, just at her
// slower tempo. Moving or turning the whole giant adds inertia measured in body
// lengths, so walking, stopping or being knocked still makes things sway.
class GiantJiggle3D : public SkeletonModifier3D {
	GDCLASS(GiantJiggle3D, SkeletonModifier3D);

public:
	struct Joint {
		int bone = -1;
		Vector3 tail_offset; // Tail in the bone's local space.
		Vector3 position; // Simulated tail in body space.
		Vector3 velocity;
	};

private:
	TypedArray<GiantJiggleChain> chains;
	LocalVector<LocalVector<Joint>> chain_joints;

	bool frame_valid = false;
	bool frame_velocity_valid = false;
	Basis frame_rotation;
	Vector3 frame_origin;
	Vector3 frame_velocity; // World-oriented, in body lengths per body second.
	Vector3 frame_accel; // Body space, this frame.
	real_t time_scale_override = -1.0;

	void _build_joints(Skeleton3D *p_skeleton, const Ref<GiantJiggleChain> &p_chain, LocalVector<Joint> &r_joints) const;
	bool _joints_match(Skeleton3D *p_skeleton, const Ref<GiantJiggleChain> &p_chain, const LocalVector<Joint> &p_joints) const;
	void _carry_with_frame(Skeleton3D *p_skeleton, double p_body_delta);
	void _simulate_chain(Skeleton3D *p_skeleton, const Ref<GiantJiggleChain> &p_chain, LocalVector<Joint> &p_joints, double p_body_delta, const Vector3 &p_down);

protected:
	static void _bind_methods();

	virtual void _set_active(bool p_active) override;
	virtual void _process_modification(double p_delta) override;

public:
	virtual bool has_process() const override { return true; }

	void set_chains(const TypedArray<GiantJiggleChain> &p_chains);
	TypedArray<GiantJiggleChain> get_chains() const;

	// Seconds of body time that pass per real second. Normally taken from the
	// nearest GiantBody; a value of zero or more overrides it (used by tests).
	void set_time_scale_override(real_t p_scale);
	real_t get_time_scale_override() const;
	real_t get_effective_time_scale() const;

	void reset();

	// Simulated tail of a chain's joint in body space, for tests and debugging.
	Vector3 get_joint_position(int p_chain, int p_joint) const;
};
