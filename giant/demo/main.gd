extends Node3D
## Milestone 1 test level: one giantess who can grow from human size to 1.5 km,
## walk through a town, jiggle, and shake the ground.
##
## Run with `--fixed-fps 30 -- --screenshots=<folder>` to save a set of test pictures and quit.

var giant: GiantBody
var rig: CameraRig
var hud: Hud
var walking := false
var skin_material: ShaderMaterial

var _animation: AnimationPlayer
var _jiggle: GiantJiggle3D
var _heading := 0.0
var _sun: DirectionalLight3D


func _ready() -> void:
	WorldBuilder.build(self)
	_sun = $Sun

	skin_material = ShaderMaterial.new()
	skin_material.shader = load("res://giant/giant_skin.gdshader")
	skin_material.set_shader_parameter("detail_albedo", _noise_texture(false))
	skin_material.set_shader_parameter("detail_normal", _noise_texture(true))

	giant = GiantMannequin.build(skin_material)
	add_child(giant)
	giant.height = 1.8
	_animation = giant.get_node("AnimationPlayer")
	_jiggle = giant.get_node("Skeleton/Jiggle")
	_animation.play("idle")
	giant.get_node("Footsteps").footstep.connect(_on_footstep)

	rig = CameraRig.new()
	rig.name = "CameraRig"
	rig.giant = giant
	add_child(rig)

	hud = Hud.new()
	hud.main = self
	add_child(hud)

	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--screenshots="):
			_take_screenshots.call_deferred(arg.trim_prefix("--screenshots="))


func _noise_texture(normal_map: bool) -> NoiseTexture2D:
	var noise := FastNoiseLite.new()
	noise.noise_type = FastNoiseLite.TYPE_CELLULAR if normal_map else FastNoiseLite.TYPE_SIMPLEX_SMOOTH
	noise.frequency = 0.025
	noise.fractal_octaves = 3
	var tex := NoiseTexture2D.new()
	tex.width = 256
	tex.height = 256
	tex.seamless = true
	tex.as_normal_map = normal_map
	tex.bump_strength = 6.0
	tex.generate_mipmaps = true
	tex.noise = noise
	return tex


func set_walking(on: bool) -> void:
	walking = on
	_animation.play("walk" if on else "idle", 0.4)


func set_jiggle(on: bool) -> void:
	_jiggle.active = on


func set_detail_layers(on: bool) -> void:
	skin_material.set_shader_parameter("use_octaves", on)


func _process(delta: float) -> void:
	if walking:
		_walk(delta)
	# Shadows reach as far as the giant is big, so a 1 km giant still casts one.
	var view_distance := rig.camera.global_position.distance_to(giant.global_position)
	_sun.directional_shadow_max_distance = clampf(view_distance * 1.5 + giant.height * 2.0, 60.0, 20000.0)


# Walks forward at the speed the animation's feet travel, wandering but staying
# within reach of the town.
func _walk(delta: float) -> void:
	var body_seconds := delta * giant.get_time_scale()
	var to_home := -giant.global_position
	to_home.y = 0
	var roam := maxf(250.0, giant.height * 6.0)
	if to_home.length() > roam:
		var want := atan2(to_home.x, to_home.z)
		_heading = rotate_toward(_heading, want, 0.35 * body_seconds)
	else:
		_heading += 0.05 * body_seconds * sin(Time.get_ticks_msec() * 0.0001)
	giant.rotation.y = _heading
	var forward := Vector3(sin(_heading), 0, cos(_heading))
	giant.global_position += forward * GiantMannequin.walk_speed * giant.get_scale_factor() * body_seconds


func _on_footstep(_foot: StringName, where: Vector3, giant_height: float) -> void:
	# The bigger the giant and the closer you are, the harder the ground shakes.
	var distance := rig.camera.global_position.distance_to(where)
	var reach := giant_height * 6.0
	var shake := clampf((giant_height / 40.0) * (1.0 - distance / reach), 0.0, 0.9)
	rig.add_shake(shake)
	if giant_height > 4.0:
		_spawn_dust(where, giant_height)


func _spawn_dust(where: Vector3, giant_height: float) -> void:
	var dust := GPUParticles3D.new()
	dust.one_shot = true
	dust.amount = 32
	dust.lifetime = 2.6
	dust.explosiveness = 0.9
	dust.local_coords = true
	# Dust is part of the giant's world: it billows at her size and her slower tempo.
	dust.speed_scale = giant.get_time_scale()
	var process := ParticleProcessMaterial.new()
	process.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_RING
	process.emission_ring_radius = 0.16
	process.emission_ring_inner_radius = 0.08
	process.emission_ring_height = 0.01
	process.emission_ring_axis = Vector3.UP
	process.direction = Vector3(0, 0.4, 0)
	process.spread = 80.0
	process.initial_velocity_min = 0.25
	process.initial_velocity_max = 0.6
	process.gravity = Vector3(0, -0.05, 0)
	process.damping_min = 0.4
	process.damping_max = 0.6
	process.scale_min = 0.6
	process.scale_max = 1.2
	var grow := Curve.new()
	grow.add_point(Vector2(0, 0.4))
	grow.add_point(Vector2(1, 1.6))
	var grow_tex := CurveTexture.new()
	grow_tex.curve = grow
	process.scale_curve = grow_tex
	var fade := Gradient.new()
	fade.set_color(0, Color(0.6, 0.53, 0.42, 0.75))
	fade.set_color(1, Color(0.55, 0.48, 0.38, 0.0))
	var fade_tex := GradientTexture1D.new()
	fade_tex.gradient = fade
	process.color_ramp = fade_tex
	dust.process_material = process
	var quad := QuadMesh.new()
	quad.size = Vector2(0.3, 0.3)
	var mat := StandardMaterial3D.new()
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.billboard_mode = BaseMaterial3D.BILLBOARD_PARTICLES
	mat.vertex_color_use_as_albedo = true
	mat.albedo_texture = _dust_texture()
	quad.material = mat
	dust.draw_pass_1 = quad
	add_child(dust)
	dust.global_position = where
	dust.scale = Vector3.ONE * (giant_height / GiantMannequin.REST_HEIGHT)
	dust.emitting = true
	get_tree().create_timer(dust.lifetime / dust.speed_scale + 0.5).timeout.connect(dust.queue_free)


var _dust_tex: GradientTexture2D

func _dust_texture() -> GradientTexture2D:
	if _dust_tex == null:
		var g := Gradient.new()
		g.set_color(0, Color(1, 1, 1, 1))
		g.set_color(1, Color(1, 1, 1, 0))
		_dust_tex = GradientTexture2D.new()
		_dust_tex.gradient = g
		_dust_tex.fill = GradientTexture2D.FILL_RADIAL
		_dust_tex.fill_from = Vector2(0.5, 0.5)
		_dust_tex.fill_to = Vector2(1.0, 0.5)
	return _dust_tex


# Test pictures for checking the prototype without a person at the screen.
func _take_screenshots(folder: String) -> void:
	DirAccess.make_dir_recursive_absolute(folder)
	hud.visible = false
	var shots := [
		["01_human_size_orbit", 1.8, CameraRig.Mode.ORBIT, true],
		["02_50m_orbit", 50.0, CameraRig.Mode.ORBIT, true],
		["03_500m_orbit", 500.0, CameraRig.Mode.ORBIT, true],
		["04_500m_from_the_ground", 500.0, CameraRig.Mode.ON_FOOT, true],
		["05_1500m_far", 1500.0, CameraRig.Mode.ORBIT, true],
	]
	for shot in shots:
		giant.height = shot[1]
		# On foot, stand out in front of her where the whole giant is in view.
		rig.walker.global_position = Vector3(30, 0, shot[1] * 0.45)
		rig.orbit_zoom = 2.4 if shot[1] < 1000.0 else 4.0
		rig.set_mode(shot[2])
		await _settle(30)
		_save(folder, shot[0])

	# Close-up of the giant's shin at 500 m, with and without detail layers.
	giant.height = 500.0
	rig.set_mode(CameraRig.Mode.FLY)
	# A point on the front of the left shin, then step back a little under 1 m.
	var shin := giant.global_transform * Vector3(0.1, 0.25, 0.052)
	rig.fly_position = shin + Vector3(0.25, 0.2, 0.9)
	rig.yaw = atan2(-(shin - rig.fly_position).x, -(shin - rig.fly_position).z)
	rig.pitch = -0.15
	for on in [true, false]:
		set_detail_layers(on)
		await _settle(10)
		_save(folder, "06_500m_shin_closeup_detail_%s" % ("on" if on else "off"))
	set_detail_layers(true)

	# Walking through town at 50 m, seen from the street.
	giant.height = 50.0
	set_walking(true)
	rig.walker.global_position = Vector3(12, 0, 60)
	rig.set_mode(CameraRig.Mode.ON_FOOT)
	await _settle(90)
	_save(folder, "07_50m_walking_from_the_street")
	print("Screenshots saved to ", folder)
	get_tree().quit()


func _settle(frames: int) -> void:
	for i in frames:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw


func _save(folder: String, name: String) -> void:
	get_viewport().get_texture().get_image().save_png(folder.path_join(name + ".png"))
