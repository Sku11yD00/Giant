/**************************************************************************/
/*  giant_body.h                                                          */
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

#include "scene/3d/node_3d.h"

class AnimationMixer;

// The root of a giant. Its origin sits between the feet, so growing scales
// the giant up from the ground. Height is in metres; the node's scale follows it.
//
// The time feel decides how much slower a big giant moves. At 0 she moves like
// a human at every size. At 1 she follows real physics: a pendulum (and so an arm,
// a stride or a jiggle) that is s times longer takes sqrt(s) times longer to swing.
// Values in between blend the two, which reads as huge without feeling sluggish.
class GiantBody : public Node3D {
	GDCLASS(GiantBody, Node3D);

	real_t rest_height = 1.8;
	real_t height = 1.8;
	real_t time_feel = 0.5;
	NodePath animation_mixer_path;

	bool growing = false;
	real_t grow_from = 1.8;
	real_t grow_to_height = 1.8;
	double grow_duration = 0.0;
	double grow_elapsed = 0.0;

	void _apply_scale();
	void _update_instance_parameters(Node *p_node, real_t p_scale);
	void _update_processing();
	AnimationMixer *_get_animation_mixer() const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static real_t compute_time_scale(real_t p_scale_factor, real_t p_time_feel);
	static real_t compute_growth_height(real_t p_from, real_t p_to, double p_progress);

	void set_rest_height(real_t p_height);
	real_t get_rest_height() const;

	void set_height(real_t p_height);
	real_t get_height() const;

	void set_time_feel(real_t p_time_feel);
	real_t get_time_feel() const;

	void set_animation_mixer(const NodePath &p_path);
	NodePath get_animation_mixer() const;

	real_t get_scale_factor() const;
	real_t get_time_scale() const;

	void grow_to(real_t p_height, double p_duration);
	void stop_growing();
	bool is_growing() const;

	// Advances growth and animation by a real-time step. Called every frame while
	// the game runs; tests call it directly.
	void advance(double p_delta);

	static GiantBody *find_for_node(const Node *p_node);

	GiantBody();
};
