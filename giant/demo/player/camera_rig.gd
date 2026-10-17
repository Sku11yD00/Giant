class_name CameraRig
extends Node3D
## Three ways to look at the giant:
## 1 Orbit around her (right mouse drag to turn, wheel to zoom),
## 2 On foot as a normal-sized person (WASD, mouse, Shift to run, Space to jump),
## 3 Fly freely (WASD, Q and E for down and up, Shift for speed).

enum Mode { ORBIT, ON_FOOT, FLY }

const EYE_HEIGHT := 1.65
const MOUSE_SENSITIVITY := 0.0025

var mode := Mode.ORBIT
var giant: GiantBody

var camera: Camera3D
var walker: CharacterBody3D

var yaw := 0.6
var pitch := -0.15
var orbit_zoom := 2.4
var fly_position := Vector3(40, 30, -60)

var _trauma := 0.0
var _shake_time := 0.0


func _ready() -> void:
	camera = Camera3D.new()
	camera.name = "Camera"
	camera.near = 0.05
	camera.far = 400000.0
	camera.fov = 70.0
	add_child(camera)
	camera.make_current()

	walker = CharacterBody3D.new()
	walker.name = "Walker"
	var shape := CollisionShape3D.new()
	var capsule := CapsuleShape3D.new()
	capsule.radius = 0.3
	capsule.height = 1.8
	shape.shape = capsule
	shape.position.y = 0.9
	walker.add_child(shape)
	add_child(walker)
	walker.global_position = Vector3(5, 0, 14)


## Shakes the camera; 1.0 is a heavy jolt.
func add_shake(amount: float) -> void:
	_trauma = clampf(_trauma + amount, 0.0, 1.0)


func set_mode(new_mode: Mode) -> void:
	mode = new_mode
	if mode == Mode.ON_FOOT:
		# Face the giant from where we stand.
		var to_giant := giant.global_position - walker.global_position
		yaw = atan2(-to_giant.x, -to_giant.z)
		pitch = 0.25
	elif mode == Mode.FLY:
		fly_position = camera.global_position
	if mode != Mode.ON_FOOT and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_1: set_mode(Mode.ORBIT)
			KEY_2: set_mode(Mode.ON_FOOT)
			KEY_3: set_mode(Mode.FLY)
			KEY_ESCAPE: Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	if event is InputEventMouseButton and event.pressed:
		if event.button_index == MOUSE_BUTTON_WHEEL_UP:
			orbit_zoom = maxf(orbit_zoom / 1.15, 0.35)
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			orbit_zoom = minf(orbit_zoom * 1.15, 40.0)
		elif event.button_index == MOUSE_BUTTON_LEFT and mode != Mode.ORBIT:
			Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	if event is InputEventMouseMotion:
		var looking: bool = Input.mouse_mode == Input.MOUSE_MODE_CAPTURED or Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)
		if looking:
			yaw -= event.relative.x * MOUSE_SENSITIVITY
			pitch = clampf(pitch - event.relative.y * MOUSE_SENSITIVITY, -1.5, 1.5)


func _physics_process(delta: float) -> void:
	if mode != Mode.ON_FOOT:
		return
	var input := _move_input()
	var speed := 6.0 if Input.is_key_pressed(KEY_SHIFT) else 2.6
	var forward := Basis(Vector3.UP, yaw)
	var velocity := forward * Vector3(input.x, 0, input.y) * speed
	walker.velocity.x = velocity.x
	walker.velocity.z = velocity.z
	if walker.is_on_floor():
		if Input.is_key_pressed(KEY_SPACE):
			walker.velocity.y = 4.5
	else:
		walker.velocity.y -= 9.8 * delta
	walker.move_and_slide()


func _process(delta: float) -> void:
	var view := Transform3D()
	match mode:
		Mode.ORBIT:
			var focus := giant.global_position + Vector3.UP * giant.height * 0.55
			var distance := giant.height * orbit_zoom
			var dir := Basis(Vector3.UP, yaw) * Basis(Vector3.RIGHT, pitch) * Vector3.BACK
			var eye := focus + dir * distance
			eye.y = maxf(eye.y, 1.0)
			view = Transform3D(Basis(), eye).looking_at(focus, Vector3.UP)
		Mode.ON_FOOT:
			view = Transform3D(Basis(Vector3.UP, yaw) * Basis(Vector3.RIGHT, pitch), walker.global_position + Vector3.UP * EYE_HEIGHT)
		Mode.FLY:
			var input := _move_input()
			var rise := float(Input.is_key_pressed(KEY_E)) - float(Input.is_key_pressed(KEY_Q))
			# Fly faster the higher you are, so crossing a 1 km giant doesn't take forever.
			var speed := maxf(6.0, fly_position.y * 0.8) * (4.0 if Input.is_key_pressed(KEY_SHIFT) else 1.0)
			var basis := Basis(Vector3.UP, yaw) * Basis(Vector3.RIGHT, pitch)
			fly_position += (basis * Vector3(input.x, 0, input.y) + Vector3.UP * rise) * speed * delta
			fly_position.y = maxf(fly_position.y, 0.5)
			view = Transform3D(basis, fly_position)

	# Camera shake: a quick, decaying wobble.
	_shake_time += delta
	_trauma = maxf(_trauma - delta * 0.9, 0.0)
	var shake := _trauma * _trauma
	if shake > 0.0:
		var wobble := Vector3(sin(_shake_time * 37.0) + 0.5 * sin(_shake_time * 71.0), sin(_shake_time * 43.0 + 1.0), 0.5 * sin(_shake_time * 29.0 + 2.0))
		view.basis = view.basis * Basis.from_euler(wobble * shake * 0.05)
		if mode == Mode.ON_FOOT:
			view.origin.y += sin(_shake_time * 55.0) * shake * 0.15
	camera.global_transform = view


func _move_input() -> Vector2:
	var v := Vector2(
		float(Input.is_key_pressed(KEY_D)) - float(Input.is_key_pressed(KEY_A)),
		float(Input.is_key_pressed(KEY_S)) - float(Input.is_key_pressed(KEY_W)))
	return v.normalized() if v.length() > 1.0 else v
