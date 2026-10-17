/**************************************************************************/
/*  giant_body.cpp                                                        */
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

#include "giant_body.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/animation/animation_mixer.h"

static const real_t MIN_HEIGHT = 0.01;

real_t GiantBody::compute_time_scale(real_t p_scale_factor, real_t p_time_feel) {
	// Real physics slows motion by sqrt(scale); the time feel picks how much of that to keep.
	if (p_scale_factor <= 0) {
		return 1.0;
	}
	return Math::pow(p_scale_factor, -0.5f * p_time_feel);
}

real_t GiantBody::compute_growth_height(real_t p_from, real_t p_to, double p_progress) {
	// Grow at a steady rate in log space, so going from 2 m to 20 m takes as long as
	// 20 m to 200 m. Linear growth would look frantic at small sizes and stalled at huge ones.
	double t = CLAMP(p_progress, 0.0, 1.0);
	t = t * t * (3.0 - 2.0 * t); // Ease in and out.
	return p_from * Math::pow(double(p_to) / double(p_from), t);
}

void GiantBody::_apply_scale() {
	real_t s = get_scale_factor();
	set_scale(Vector3(s, s, s));
	_update_instance_parameters(this, s);
}

void GiantBody::_update_instance_parameters(Node *p_node, real_t p_scale) {
	// Shaders read giant_scale to add finer detail as the giant grows, see giant/README.md.
	GeometryInstance3D *gi = Object::cast_to<GeometryInstance3D>(p_node);
	if (gi) {
		gi->set_instance_shader_parameter(SNAME("giant_scale"), p_scale);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *child = p_node->get_child(i);
		if (Object::cast_to<GiantBody>(child)) {
			continue; // A giant inside a giant keeps its own scale.
		}
		_update_instance_parameters(child, p_scale);
	}
}

AnimationMixer *GiantBody::_get_animation_mixer() const {
	if (animation_mixer_path.is_empty() || !is_inside_tree()) {
		return nullptr;
	}
	return Object::cast_to<AnimationMixer>(get_node_or_null(animation_mixer_path));
}

void GiantBody::_update_processing() {
	set_process_internal(is_inside_tree() && !Engine::get_singleton()->is_editor_hint());
}

void GiantBody::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			_apply_scale();
			if (!Engine::get_singleton()->is_editor_hint()) {
				AnimationMixer *mixer = _get_animation_mixer();
				if (mixer) {
					// The giant drives the mixer so animation plays at the giant's tempo.
					mixer->set_callback_mode_process(AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL);
				}
			}
			_update_processing();
		} break;
		case NOTIFICATION_CHILD_ORDER_CHANGED: {
			if (is_inside_tree()) {
				_update_instance_parameters(this, get_scale_factor());
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			advance(get_process_delta_time());
		} break;
	}
}

void GiantBody::advance(double p_delta) {
	if (growing) {
		grow_elapsed += p_delta;
		double progress = grow_duration > 0 ? grow_elapsed / grow_duration : 1.0;
		set_height(compute_growth_height(grow_from, grow_to_height, progress));
		if (progress >= 1.0) {
			growing = false;
			emit_signal(SNAME("growth_finished"));
		}
	}
	AnimationMixer *mixer = _get_animation_mixer();
	if (mixer && mixer->get_callback_mode_process() == AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL) {
		mixer->advance(p_delta * get_time_scale());
	}
}

void GiantBody::set_rest_height(real_t p_height) {
	rest_height = MAX(p_height, MIN_HEIGHT);
	_apply_scale();
}

real_t GiantBody::get_rest_height() const {
	return rest_height;
}

void GiantBody::set_height(real_t p_height) {
	real_t new_height = MAX(p_height, MIN_HEIGHT);
	if (new_height == height) {
		return;
	}
	height = new_height;
	_apply_scale();
	emit_signal(SNAME("height_changed"), height);
}

real_t GiantBody::get_height() const {
	return height;
}

void GiantBody::set_time_feel(real_t p_time_feel) {
	time_feel = CLAMP(p_time_feel, 0.0, 1.0);
}

real_t GiantBody::get_time_feel() const {
	return time_feel;
}

void GiantBody::set_animation_mixer(const NodePath &p_path) {
	animation_mixer_path = p_path;
}

NodePath GiantBody::get_animation_mixer() const {
	return animation_mixer_path;
}

real_t GiantBody::get_scale_factor() const {
	return height / rest_height;
}

real_t GiantBody::get_time_scale() const {
	return compute_time_scale(get_scale_factor(), time_feel);
}

void GiantBody::grow_to(real_t p_height, double p_duration) {
	grow_from = height;
	grow_to_height = MAX(p_height, MIN_HEIGHT);
	grow_duration = MAX(p_duration, 0.0);
	grow_elapsed = 0.0;
	growing = true;
	if (grow_duration == 0.0) {
		advance(0.0);
	}
}

void GiantBody::stop_growing() {
	growing = false;
}

bool GiantBody::is_growing() const {
	return growing;
}

GiantBody *GiantBody::find_for_node(const Node *p_node) {
	for (const Node *n = p_node; n; n = n->get_parent()) {
		GiantBody *body = Object::cast_to<GiantBody>(const_cast<Node *>(n));
		if (body) {
			return body;
		}
	}
	return nullptr;
}

void GiantBody::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_rest_height", "height"), &GiantBody::set_rest_height);
	ClassDB::bind_method(D_METHOD("get_rest_height"), &GiantBody::get_rest_height);
	ClassDB::bind_method(D_METHOD("set_height", "height"), &GiantBody::set_height);
	ClassDB::bind_method(D_METHOD("get_height"), &GiantBody::get_height);
	ClassDB::bind_method(D_METHOD("set_time_feel", "time_feel"), &GiantBody::set_time_feel);
	ClassDB::bind_method(D_METHOD("get_time_feel"), &GiantBody::get_time_feel);
	ClassDB::bind_method(D_METHOD("set_animation_mixer", "path"), &GiantBody::set_animation_mixer);
	ClassDB::bind_method(D_METHOD("get_animation_mixer"), &GiantBody::get_animation_mixer);

	ClassDB::bind_method(D_METHOD("get_scale_factor"), &GiantBody::get_scale_factor);
	ClassDB::bind_method(D_METHOD("get_time_scale"), &GiantBody::get_time_scale);
	ClassDB::bind_method(D_METHOD("grow_to", "height", "duration"), &GiantBody::grow_to);
	ClassDB::bind_method(D_METHOD("stop_growing"), &GiantBody::stop_growing);
	ClassDB::bind_method(D_METHOD("is_growing"), &GiantBody::is_growing);
	ClassDB::bind_method(D_METHOD("advance", "delta"), &GiantBody::advance);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rest_height", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater,suffix:m"), "set_rest_height", "get_rest_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height", PROPERTY_HINT_RANGE, "0.01,10000,0.01,exp,or_greater,suffix:m"), "set_height", "get_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_feel", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_time_feel", "get_time_feel");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "animation_mixer", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "AnimationMixer"), "set_animation_mixer", "get_animation_mixer");

	ADD_SIGNAL(MethodInfo("height_changed", PropertyInfo(Variant::FLOAT, "height")));
	ADD_SIGNAL(MethodInfo("growth_finished"));
}

GiantBody::GiantBody() {
}
