/**************************************************************************/
/*  giant_footsteps_3d.h                                                  */
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

#include "scene/main/node.h"

class Skeleton3D;

// Detects when a giant's feet touch the ground and emits footstep, so games can
// shake the camera, throw up dust and damage what is underfoot. Heights are
// measured in body lengths, so the same settings work at every size.
class GiantFootsteps3D : public Node {
	GDCLASS(GiantFootsteps3D, Node);

	struct Foot {
		int bone = -1;
		real_t rest_height = 0.0; // Ankle height above the giant's origin, in body units.
		bool down = true;
	};

	NodePath skeleton_path;
	PackedStringArray foot_bones;
	real_t contact_height = 0.03;
	real_t lift_height = 0.08;
	LocalVector<Foot> feet;
	ObjectID cached_skeleton;

	Skeleton3D *_get_skeleton() const;
	void _rebuild_feet(Skeleton3D *p_skeleton);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_skeleton_path(const NodePath &p_path);
	NodePath get_skeleton_path() const;
	void set_foot_bones(const PackedStringArray &p_bones);
	PackedStringArray get_foot_bones() const;
	void set_contact_height(real_t p_height);
	real_t get_contact_height() const;
	void set_lift_height(real_t p_height);
	real_t get_lift_height() const;

	// Checks the feet once. Called every frame while the game runs; tests call it directly.
	void update_feet();

	GiantFootsteps3D();
};
