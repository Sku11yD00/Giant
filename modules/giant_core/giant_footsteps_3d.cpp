/**************************************************************************/
/*  giant_footsteps_3d.cpp                                                */
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

#include "giant_footsteps_3d.h"

#include "giant_body.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/skeleton_3d.h"

Skeleton3D *GiantFootsteps3D::_get_skeleton() const {
	if (skeleton_path.is_empty() || !is_inside_tree()) {
		return nullptr;
	}
	return Object::cast_to<Skeleton3D>(get_node_or_null(skeleton_path));
}

static Transform3D _ground_frame(Skeleton3D *p_skeleton) {
	// The giant's origin sits on the ground between her feet.
	GiantBody *body = GiantBody::find_for_node(p_skeleton);
	return body ? body->get_global_transform() : p_skeleton->get_global_transform();
}

void GiantFootsteps3D::_rebuild_feet(Skeleton3D *p_skeleton) {
	feet.clear();
	cached_skeleton = p_skeleton ? p_skeleton->get_instance_id() : ObjectID();
	if (!p_skeleton) {
		return;
	}
	Transform3D to_ground = _ground_frame(p_skeleton).affine_inverse() * p_skeleton->get_global_transform();
	for (const String &name : foot_bones) {
		Foot foot;
		foot.bone = p_skeleton->find_bone(name);
		if (foot.bone < 0) {
			WARN_PRINT(vformat("GiantFootsteps3D: bone \"%s\" not found in the skeleton.", name));
			continue;
		}
		foot.rest_height = to_ground.xform(p_skeleton->get_bone_global_rest(foot.bone).origin).y;
		feet.push_back(foot);
	}
}

void GiantFootsteps3D::update_feet() {
	Skeleton3D *skeleton = _get_skeleton();
	if (!skeleton) {
		return;
	}
	if (skeleton->get_instance_id() != cached_skeleton) {
		_rebuild_feet(skeleton);
	}
	Transform3D ground = _ground_frame(skeleton);
	Transform3D to_ground = ground.affine_inverse() * skeleton->get_global_transform();
	GiantBody *body = GiantBody::find_for_node(skeleton);
	real_t giant_height = body ? body->get_height() : skeleton->get_global_transform().basis.get_scale().y;

	for (Foot &foot : feet) {
		Vector3 local = to_ground.xform(skeleton->get_bone_global_pose(foot.bone).origin);
		real_t lift = local.y - foot.rest_height;
		if (foot.down && lift > lift_height) {
			foot.down = false;
		} else if (!foot.down && lift < contact_height) {
			foot.down = true;
			Vector3 position = ground.xform(Vector3(local.x, 0, local.z));
			emit_signal(SNAME("footstep"), StringName(skeleton->get_bone_name(foot.bone)), position, giant_height);
		}
	}
}

void GiantFootsteps3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_process_internal(!Engine::get_singleton()->is_editor_hint());
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			update_feet();
		} break;
	}
}

void GiantFootsteps3D::set_skeleton_path(const NodePath &p_path) {
	skeleton_path = p_path;
	cached_skeleton = ObjectID();
}

NodePath GiantFootsteps3D::get_skeleton_path() const {
	return skeleton_path;
}

void GiantFootsteps3D::set_foot_bones(const PackedStringArray &p_bones) {
	foot_bones = p_bones;
	cached_skeleton = ObjectID();
}

PackedStringArray GiantFootsteps3D::get_foot_bones() const {
	return foot_bones;
}

void GiantFootsteps3D::set_contact_height(real_t p_height) {
	contact_height = p_height;
}

real_t GiantFootsteps3D::get_contact_height() const {
	return contact_height;
}

void GiantFootsteps3D::set_lift_height(real_t p_height) {
	lift_height = p_height;
}

real_t GiantFootsteps3D::get_lift_height() const {
	return lift_height;
}

void GiantFootsteps3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_skeleton_path", "path"), &GiantFootsteps3D::set_skeleton_path);
	ClassDB::bind_method(D_METHOD("get_skeleton_path"), &GiantFootsteps3D::get_skeleton_path);
	ClassDB::bind_method(D_METHOD("set_foot_bones", "bones"), &GiantFootsteps3D::set_foot_bones);
	ClassDB::bind_method(D_METHOD("get_foot_bones"), &GiantFootsteps3D::get_foot_bones);
	ClassDB::bind_method(D_METHOD("set_contact_height", "height"), &GiantFootsteps3D::set_contact_height);
	ClassDB::bind_method(D_METHOD("get_contact_height"), &GiantFootsteps3D::get_contact_height);
	ClassDB::bind_method(D_METHOD("set_lift_height", "height"), &GiantFootsteps3D::set_lift_height);
	ClassDB::bind_method(D_METHOD("get_lift_height"), &GiantFootsteps3D::get_lift_height);
	ClassDB::bind_method(D_METHOD("update_feet"), &GiantFootsteps3D::update_feet);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "skeleton_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Skeleton3D"), "set_skeleton_path", "get_skeleton_path");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "foot_bones"), "set_foot_bones", "get_foot_bones");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "contact_height", PROPERTY_HINT_RANGE, "0,0.5,0.001,suffix:body m"), "set_contact_height", "get_contact_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lift_height", PROPERTY_HINT_RANGE, "0,0.5,0.001,suffix:body m"), "set_lift_height", "get_lift_height");

	ADD_SIGNAL(MethodInfo("footstep", PropertyInfo(Variant::STRING_NAME, "foot"), PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::FLOAT, "giant_height")));
}

GiantFootsteps3D::GiantFootsteps3D() {
	foot_bones.push_back("LeftFoot");
	foot_bones.push_back("RightFoot");
}
