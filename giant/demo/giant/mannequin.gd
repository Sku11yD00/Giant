class_name GiantMannequin
extends RefCounted
## Builds a stand-in giantess from code: skeleton, skinned mesh, walk and idle
## animations, jiggle and hair. Real art replaces this later; every Giant system
## works the same on an imported character.

const REST_HEIGHT := 1.81

# name, parent, rest offset from the parent bone (body metres).
const BONES := [
	["Hips", "", Vector3(0, 0.95, 0)],
	["Spine", "Hips", Vector3(0, 0.12, 0)],
	["Chest", "Spine", Vector3(0, 0.2, 0)],
	["Neck", "Chest", Vector3(0, 0.22, 0)],
	["Head", "Neck", Vector3(0, 0.1, 0)],
	["HeadTop", "Head", Vector3(0, 0.22, 0)],
	["Breast.L", "Chest", Vector3(0.085, 0.04, 0.07)],
	["Breast.L.Tip", "Breast.L", Vector3(0, 0, 0.09)],
	["Breast.R", "Chest", Vector3(-0.085, 0.04, 0.07)],
	["Breast.R.Tip", "Breast.R", Vector3(0, 0, 0.09)],
	["Ponytail0", "Head", Vector3(0, 0.16, -0.1)],
	["Ponytail1", "Ponytail0", Vector3(0, -0.1, -0.03)],
	["Ponytail2", "Ponytail1", Vector3(0, -0.1, 0)],
	["Ponytail3", "Ponytail2", Vector3(0, -0.1, 0)],
	["Ponytail4", "Ponytail3", Vector3(0, -0.1, 0)],
	["Ponytail5", "Ponytail4", Vector3(0, -0.09, 0)],
	["UpperArm.L", "Chest", Vector3(0.19, 0.16, 0)],
	["LowerArm.L", "UpperArm.L", Vector3(0, -0.28, 0)],
	["Hand.L", "LowerArm.L", Vector3(0, -0.25, 0)],
	["HandEnd.L", "Hand.L", Vector3(0, -0.16, 0)],
	["UpperArm.R", "Chest", Vector3(-0.19, 0.16, 0)],
	["LowerArm.R", "UpperArm.R", Vector3(0, -0.28, 0)],
	["Hand.R", "LowerArm.R", Vector3(0, -0.25, 0)],
	["HandEnd.R", "Hand.R", Vector3(0, -0.16, 0)],
	["UpperLeg.L", "Hips", Vector3(0.1, -0.05, 0)],
	["LowerLeg.L", "UpperLeg.L", Vector3(0, -0.42, 0)],
	["LeftFoot", "LowerLeg.L", Vector3(0, -0.42, 0)],
	["Toe.L", "LeftFoot", Vector3(0, -0.04, 0.15)],
	["UpperLeg.R", "Hips", Vector3(-0.1, -0.05, 0)],
	["LowerLeg.R", "UpperLeg.R", Vector3(0, -0.42, 0)],
	["RightFoot", "LowerLeg.R", Vector3(0, -0.42, 0)],
	["Toe.R", "RightFoot", Vector3(0, -0.04, 0.15)],
]

const SKIN := Color(0.85, 0.64, 0.52)
const TOP := Color(0.16, 0.32, 0.55)
const SHORTS := Color(0.22, 0.22, 0.26)
const HAIR := Color(0.3, 0.17, 0.09)
const SHOES := Color(0.12, 0.1, 0.09)

# bone, start offset, end offset (both in the bone's rest space), start radius, end radius, colour.
# Radii can be a Vector2 (side, front) to flatten the shape.
const PARTS := [
	["Hips", Vector3(0, -0.06, 0), Vector3(0, 0.1, 0), Vector2(0.17, 0.12), Vector2(0.15, 0.1), SHORTS],
	["Spine", Vector3(0, 0.0, 0), Vector3(0, 0.2, 0), Vector2(0.13, 0.095), Vector2(0.15, 0.1), TOP],
	["Chest", Vector3(0, 0.0, 0), Vector3(0, 0.2, 0), Vector2(0.155, 0.1), Vector2(0.16, 0.095), TOP],
	["Neck", Vector3(0, -0.02, 0), Vector3(0, 0.12, 0), 0.05, 0.048, SKIN],
	["Head", Vector3(0, 0.02, 0.01), Vector3(0, 0.2, 0.0), Vector2(0.088, 0.1), Vector2(0.085, 0.095), SKIN],
	["Head", Vector3(0, 0.07, -0.025), Vector3(0, 0.21, -0.02), Vector2(0.095, 0.1), Vector2(0.088, 0.092), HAIR],
	["Breast.L", Vector3(0, 0, -0.01), Vector3(0, 0, 0.035), 0.075, 0.06, TOP],
	["Breast.R", Vector3(0, 0, -0.01), Vector3(0, 0, 0.035), 0.075, 0.06, TOP],
	["Ponytail0", Vector3(0, 0, 0), Vector3(0, -0.1, -0.03), 0.04, 0.045, HAIR],
	["Ponytail1", Vector3(0, 0, 0), Vector3(0, -0.1, 0), 0.045, 0.042, HAIR],
	["Ponytail2", Vector3(0, 0, 0), Vector3(0, -0.1, 0), 0.042, 0.036, HAIR],
	["Ponytail3", Vector3(0, 0, 0), Vector3(0, -0.1, 0), 0.036, 0.028, HAIR],
	["Ponytail4", Vector3(0, 0, 0), Vector3(0, -0.09, 0), 0.028, 0.016, HAIR],
	["UpperArm.L", Vector3(0, 0.02, 0), Vector3(0, -0.28, 0), 0.05, 0.04, SKIN],
	["LowerArm.L", Vector3(0, 0, 0), Vector3(0, -0.25, 0), 0.04, 0.03, SKIN],
	["Hand.L", Vector3(0, 0, 0), Vector3(0, -0.15, 0.01), Vector2(0.035, 0.018), Vector2(0.03, 0.014), SKIN],
	["UpperArm.R", Vector3(0, 0.02, 0), Vector3(0, -0.28, 0), 0.05, 0.04, SKIN],
	["LowerArm.R", Vector3(0, 0, 0), Vector3(0, -0.25, 0), 0.04, 0.03, SKIN],
	["Hand.R", Vector3(0, 0, 0), Vector3(0, -0.15, 0.01), Vector2(0.035, 0.018), Vector2(0.03, 0.014), SKIN],
	["UpperLeg.L", Vector3(0, 0.04, 0), Vector3(0, -0.42, 0), 0.085, 0.055, SHORTS],
	["UpperLeg.L", Vector3(0, -0.2, 0), Vector3(0, -0.42, 0), 0.07, 0.055, SKIN],
	["LowerLeg.L", Vector3(0, 0, 0), Vector3(0, -0.42, 0), 0.055, 0.038, SKIN],
	["LeftFoot", Vector3(0, -0.02, -0.03), Vector3(0, -0.045, 0.15), Vector2(0.04, 0.035), Vector2(0.038, 0.02), SHOES],
	["UpperLeg.R", Vector3(0, 0.04, 0), Vector3(0, -0.42, 0), 0.085, 0.055, SHORTS],
	["UpperLeg.R", Vector3(0, -0.2, 0), Vector3(0, -0.42, 0), 0.07, 0.055, SKIN],
	["LowerLeg.R", Vector3(0, 0, 0), Vector3(0, -0.42, 0), 0.055, 0.038, SKIN],
	["RightFoot", Vector3(0, -0.02, -0.03), Vector3(0, -0.045, 0.15), Vector2(0.04, 0.035), Vector2(0.038, 0.02), SHOES],
]

## Matches detail_size in giant_skin.gdshader.
const DETAIL_SIZE := 0.25

const WALK_CYCLE := 1.1 # Seconds per two steps, at human size.
const STRIDE_ANGLE := deg_to_rad(24.0)
const LEG_LENGTH := 0.84
## Body metres per second the walk animation covers at human size.
static var walk_speed := 2.0 * 2.0 * LEG_LENGTH * sin(STRIDE_ANGLE) / WALK_CYCLE


## Returns a GiantBody holding the whole character. Its origin is on the ground between the feet.
static func build(material: Material) -> GiantBody:
	var body := GiantBody.new()
	body.name = "Giantess"
	body.rest_height = REST_HEIGHT

	var skeleton := Skeleton3D.new()
	skeleton.name = "Skeleton"
	body.add_child(skeleton)
	for b in BONES:
		var index := skeleton.add_bone(b[0])
		if b[1] != "":
			skeleton.set_bone_parent(index, skeleton.find_bone(b[1]))
		skeleton.set_bone_rest(index, Transform3D(Basis(), b[2]))
	skeleton.reset_bone_poses()

	var mesh_instance := MeshInstance3D.new()
	mesh_instance.name = "Mesh"
	mesh_instance.mesh = _build_mesh(skeleton, material)
	mesh_instance.skin = _build_skin(skeleton)
	skeleton.add_child(mesh_instance)
	mesh_instance.skeleton = NodePath("..")

	var jiggle := GiantJiggle3D.new()
	jiggle.name = "Jiggle"
	jiggle.chains = [
		_chain("Breast.L", 1, 2.4, 0.16, 0.0, 22.0),
		_chain("Breast.R", 1, 2.4, 0.16, 0.0, 22.0),
		_chain("Ponytail0", 5, 1.1, 0.25, 9.8, 75.0),
	]
	skeleton.add_child(jiggle)

	var player := AnimationPlayer.new()
	player.name = "AnimationPlayer"
	body.add_child(player)
	var library := AnimationLibrary.new()
	library.add_animation("walk", _build_walk())
	library.add_animation("idle", _build_idle())
	player.add_animation_library("", library)
	player.root_node = NodePath("..")
	body.animation_mixer = NodePath("AnimationPlayer")

	var steps := GiantFootsteps3D.new()
	steps.name = "Footsteps"
	body.add_child(steps)
	steps.skeleton_path = NodePath("../Skeleton")
	return body


static func _chain(bone: String, count: int, frequency: float, damping: float, gravity: float, max_angle_deg: float) -> GiantJiggleChain:
	var chain := GiantJiggleChain.new()
	chain.root_bone = bone
	chain.bone_count = count
	chain.frequency = frequency
	chain.damping_ratio = damping
	chain.gravity = gravity
	chain.max_angle = deg_to_rad(max_angle_deg)
	chain.tip_length = 0.08
	return chain


static func _build_skin(skeleton: Skeleton3D) -> Skin:
	var skin := Skin.new()
	for i in skeleton.get_bone_count():
		skin.add_bind(i, skeleton.get_bone_global_rest(i).affine_inverse())
	return skin


static func _build_mesh(skeleton: Skeleton3D, material: Material) -> ArrayMesh:
	var st := SurfaceTool.new()
	st.set_skin_weight_count(SurfaceTool.SKIN_4_WEIGHTS)
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	for part in PARTS:
		var bone := skeleton.find_bone(part[0])
		var rest := skeleton.get_bone_global_rest(bone)
		_add_capsule(st, bone, rest * part[1], rest * part[2], _radii(part[3]), _radii(part[4]), part[5])
	st.generate_tangents()
	var mesh := st.commit()
	mesh.surface_set_material(0, material)
	return mesh


static func _radii(r) -> Vector2:
	return r if r is Vector2 else Vector2(r, r)


## Adds a tapered capsule from a to b, rigidly skinned to one bone. Its UVs are laid
## out in body metres (around, along), so textures keep the same density everywhere.
static func _add_capsule(st: SurfaceTool, bone: int, a: Vector3, b: Vector3, ra: Vector2, rb: Vector2, color: Color) -> void:
	const SIDES := 20
	const RINGS := 10
	const CAP_RINGS := 6
	var axis := b - a
	var length := axis.length()
	var up := axis.normalized()
	var side := Vector3.RIGHT if abs(up.dot(Vector3.RIGHT)) < 0.9 else Vector3.FORWARD
	var front := up.cross(side).normalized()
	side = front.cross(up).normalized()

	# Rings of (centre, radii) from the bottom cap through the body to the top cap.
	var rings := []
	for i in range(CAP_RINGS, 0, -1):
		var t := float(i) / CAP_RINGS * PI * 0.5
		rings.append([a - up * sin(t) * ra.length() * 0.7071, ra * cos(t)])
	for i in RINGS + 1:
		var t := float(i) / RINGS
		rings.append([a + axis * t, ra.lerp(rb, t)])
	for i in range(1, CAP_RINGS + 1):
		var t := float(i) / CAP_RINGS * PI * 0.5
		rings.append([b + up * sin(t) * rb.length() * 0.7071, rb * cos(t)])

	# Wrap the texture a whole number of detail tiles around, so no seam shows at any octave.
	var widest := maxf(ra.x + ra.y, rb.x + rb.y) * 0.5
	var circumference := maxf(DETAIL_SIZE, roundf(TAU * widest / DETAIL_SIZE) * DETAIL_SIZE)
	var along := 0.0
	var verts := []
	for r in rings.size():
		var center: Vector3 = rings[r][0]
		var radius: Vector2 = rings[r][1]
		if r > 0:
			along += center.distance_to(rings[r - 1][0])
		var row := []
		for s in SIDES + 1:
			var angle := TAU * s / SIDES
			var dir: Vector3 = side * cos(angle) * radius.x + front * sin(angle) * radius.y
			var normal: Vector3 = dir.normalized() if dir.length() > 0.0001 else (up if r > rings.size() / 2 else -up)
			var around := float(s) / SIDES * circumference
			row.append([center + dir, normal, Vector2(around, along)])
		verts.append(row)

	for r in rings.size() - 1:
		for s in SIDES:
			var quad := [verts[r][s], verts[r + 1][s], verts[r + 1][s + 1], verts[r][s], verts[r + 1][s + 1], verts[r][s + 1]]
			for v in quad:
				st.set_color(color)
				st.set_normal(v[1])
				st.set_uv(v[2])
				st.set_bones(PackedInt32Array([bone, 0, 0, 0]))
				st.set_weights(PackedFloat32Array([1.0, 0.0, 0.0, 0.0]))
				st.add_vertex(v[0])


# Walk: each leg is planted for half the cycle while the hips ride over it.
static func _build_walk() -> Animation:
	var anim := Animation.new()
	anim.length = WALK_CYCLE
	anim.loop_mode = Animation.LOOP_LINEAR
	var keys := 32
	var tracks := {}
	for bone in ["Hips", "Spine", "Chest", "Neck", "UpperLeg.L", "LowerLeg.L", "LeftFoot", "UpperLeg.R", "LowerLeg.R", "RightFoot", "UpperArm.L", "LowerArm.L", "UpperArm.R", "LowerArm.R"]:
		tracks[bone] = _track(anim, Animation.TYPE_ROTATION_3D, bone)
	var hips_pos := _track(anim, Animation.TYPE_POSITION_3D, "Hips")

	for k in keys + 1:
		var phase := float(k) / keys
		var time := phase * WALK_CYCLE
		var left := _leg(phase)
		var right := _leg(fmod(phase + 0.5, 1.0))
		# The planted leg sets the hip height so the planted foot stays on the ground.
		var planted: Array = left if phase < 0.5 else right
		var hip_height := 0.95 - LEG_LENGTH * (1.0 - cos(planted[0]))
		var sway := 0.025 * sin(TAU * phase)
		anim.position_track_insert_key(hips_pos, time, Vector3(sway, hip_height, 0))
		var twist := 0.08 * sin(TAU * phase)
		anim.rotation_track_insert_key(tracks["Hips"], time, Quaternion(Vector3.UP, twist) * Quaternion(Vector3.FORWARD, 0.04 * sin(TAU * phase)))
		anim.rotation_track_insert_key(tracks["Spine"], time, Quaternion(Vector3.UP, -twist * 0.6))
		anim.rotation_track_insert_key(tracks["Chest"], time, Quaternion(Vector3.UP, -twist * 0.8) * Quaternion(Vector3.RIGHT, 0.05))
		anim.rotation_track_insert_key(tracks["Neck"], time, Quaternion(Vector3.UP, twist * 0.4))
		for side in [["L", "LeftFoot", left], ["R", "RightFoot", right]]:
			var leg: Array = side[2]
			# Negative rotation about X swings the leg forward (+Z).
			anim.rotation_track_insert_key(tracks["UpperLeg." + side[0]], time, Quaternion(Vector3.RIGHT, -leg[0]))
			anim.rotation_track_insert_key(tracks["LowerLeg." + side[0]], time, Quaternion(Vector3.RIGHT, leg[1]))
			anim.rotation_track_insert_key(tracks[side[1]], time, Quaternion(Vector3.RIGHT, leg[0] - leg[1] + leg[2]))
		# Arms swing against the leg on the same side.
		anim.rotation_track_insert_key(tracks["UpperArm.L"], time, Quaternion(Vector3.RIGHT, left[0] * 0.7) * Quaternion(Vector3.BACK, 0.1))
		anim.rotation_track_insert_key(tracks["UpperArm.R"], time, Quaternion(Vector3.RIGHT, right[0] * 0.7) * Quaternion(Vector3.BACK, -0.1))
		anim.rotation_track_insert_key(tracks["LowerArm.L"], time, Quaternion(Vector3.RIGHT, -0.25 - max(0.0, -left[0]) * 0.6))
		anim.rotation_track_insert_key(tracks["LowerArm.R"], time, Quaternion(Vector3.RIGHT, -0.25 - max(0.0, -right[0]) * 0.6))
	return anim


## Leg pose at a phase of the cycle: [thigh forward angle, knee bend, toe lift].
## 0 to 0.5 is planted (thigh moves front to back), 0.5 to 1 is the swing.
static func _leg(phase: float) -> Array:
	if phase < 0.5:
		var t := phase / 0.5
		return [lerp(STRIDE_ANGLE, -STRIDE_ANGLE, t), 0.0, 0.0]
	var t := (phase - 0.5) / 0.5
	var thigh: float = lerp(-STRIDE_ANGLE, STRIDE_ANGLE, 0.5 - 0.5 * cos(PI * t))
	var knee := sin(PI * min(t * 1.15, 1.0)) * deg_to_rad(62.0)
	var toe := sin(PI * t) * 0.25
	return [thigh, knee, toe]


static func _build_idle() -> Animation:
	var anim := Animation.new()
	anim.length = 4.0
	anim.loop_mode = Animation.LOOP_LINEAR
	var chest := _track(anim, Animation.TYPE_ROTATION_3D, "Chest")
	var hips := _track(anim, Animation.TYPE_ROTATION_3D, "Hips")
	var neck := _track(anim, Animation.TYPE_ROTATION_3D, "Neck")
	var arm_l := _track(anim, Animation.TYPE_ROTATION_3D, "UpperArm.L")
	var arm_r := _track(anim, Animation.TYPE_ROTATION_3D, "UpperArm.R")
	var elbow_l := _track(anim, Animation.TYPE_ROTATION_3D, "LowerArm.L")
	var elbow_r := _track(anim, Animation.TYPE_ROTATION_3D, "LowerArm.R")
	for k in 17:
		var time := k * 0.25
		var breath := sin(TAU * time / 4.0)
		anim.rotation_track_insert_key(chest, time, Quaternion(Vector3.RIGHT, -0.03 * breath))
		anim.rotation_track_insert_key(hips, time, Quaternion(Vector3.FORWARD, 0.03 * sin(TAU * time / 4.0 * 0.5 + 0.3)))
		anim.rotation_track_insert_key(neck, time, Quaternion(Vector3.UP, 0.15 * sin(TAU * time / 4.0 * 0.5)))
		anim.rotation_track_insert_key(arm_l, time, Quaternion(Vector3.BACK, 0.12 + 0.02 * breath))
		anim.rotation_track_insert_key(arm_r, time, Quaternion(Vector3.BACK, -0.12 - 0.02 * breath))
		anim.rotation_track_insert_key(elbow_l, time, Quaternion(Vector3.RIGHT, -0.2))
		anim.rotation_track_insert_key(elbow_r, time, Quaternion(Vector3.RIGHT, -0.2))
	return anim


static func _track(anim: Animation, type: int, bone: String) -> int:
	var track := anim.add_track(type)
	anim.track_set_path(track, NodePath("Skeleton:" + bone))
	return track
