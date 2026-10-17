class_name WorldBuilder
extends RefCounted
## Builds the test world: ground, a town, forest, people for scale, far mountains,
## sky and sun. Everything is real size in metres.

const TOWN_CENTER := Vector3(0, 0, 160)


static func build(root: Node3D, rng_seed: int = 7) -> void:
	var rng := RandomNumberGenerator.new()
	rng.seed = rng_seed
	_add_environment(root)
	_add_ground(root)
	_add_town(root, rng)
	_add_forest(root, rng)
	_add_people(root, rng)
	_add_mountains(root, rng)


static func _add_environment(root: Node3D) -> void:
	var sky_material := ProceduralSkyMaterial.new()
	sky_material.sky_top_color = Color(0.32, 0.5, 0.78)
	sky_material.sky_horizon_color = Color(0.7, 0.78, 0.86)
	sky_material.ground_horizon_color = Color(0.7, 0.78, 0.86)
	sky_material.ground_bottom_color = Color(0.3, 0.33, 0.3)
	var sky := Sky.new()
	sky.sky_material = sky_material

	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	# Aerial perspective: far things fade into the sky colour, which sells distance.
	env.fog_enabled = true
	env.fog_mode = Environment.FOG_MODE_EXPONENTIAL
	env.fog_density = 0.00006
	env.fog_aerial_perspective = 0.85
	env.fog_sky_affect = 0.0
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = env
	root.add_child(world_env)

	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.rotation_degrees = Vector3(-38, -140, 0)
	sun.light_energy = 1.2
	sun.shadow_enabled = true
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
	sun.directional_shadow_max_distance = 200.0
	root.add_child(sun)


static func _add_ground(root: Node3D) -> void:
	var plane := PlaneMesh.new()
	plane.size = Vector2(400000, 400000)
	var material := ShaderMaterial.new()
	material.shader = load("res://world/ground.gdshader")
	plane.material = material
	var ground := MeshInstance3D.new()
	ground.name = "Ground"
	ground.mesh = plane
	ground.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	root.add_child(ground)

	var body := StaticBody3D.new()
	body.name = "GroundCollision"
	var shape := CollisionShape3D.new()
	shape.shape = WorldBoundaryShape3D.new()
	body.add_child(shape)
	root.add_child(body)


static func _material(color: Color, roughness: float = 0.85) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = color
	m.roughness = roughness
	m.vertex_color_use_as_albedo = true
	m.vertex_color_is_srgb = true
	return m


static func _multimesh(root: Node3D, name: String, mesh: Mesh, transforms: Array, colors: Array) -> MultiMeshInstance3D:
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	mm.mesh = mesh
	mm.instance_count = transforms.size()
	for i in transforms.size():
		mm.set_instance_transform(i, transforms[i])
		mm.set_instance_color(i, colors[i])
	var node := MultiMeshInstance3D.new()
	node.name = name
	node.multimesh = mm
	root.add_child(node)
	return node


# A small town of houses along streets, with a few taller blocks in the middle.
static func _add_town(root: Node3D, rng: RandomNumberGenerator) -> void:
	var walls := []
	var wall_colors := []
	var roofs := []
	var roof_colors := []
	var collision := StaticBody3D.new()
	collision.name = "TownCollision"
	root.add_child(collision)

	for gx in range(-6, 7):
		for gz in range(-6, 7):
			if gx == 0 or gz == 0 or rng.randf() < 0.12:
				continue # Streets and a few empty plots.
			var lot := TOWN_CENTER + Vector3(gx * 22.0, 0, gz * 22.0)
			var central: bool = abs(gx) <= 2 and abs(gz) <= 2
			var size := Vector3(rng.randf_range(8, 13), rng.randf_range(5, 8), rng.randf_range(8, 12))
			if central and rng.randf() < 0.5:
				size.y = rng.randf_range(18, 45)
			var pos := lot + Vector3(rng.randf_range(-3, 3), size.y * 0.5, rng.randf_range(-3, 3))
			var basis := Basis(Vector3.UP, rng.randf_range(-0.08, 0.08))
			walls.append(Transform3D(basis * Basis.from_scale(size), pos))
			wall_colors.append(Color.from_hsv(rng.randf_range(0.05, 0.12), rng.randf_range(0.1, 0.35), rng.randf_range(0.6, 0.9)))
			if size.y < 10:
				var roof_size := Vector3(size.x * 1.1, size.y * 0.45, size.z * 1.05)
				roofs.append(Transform3D(basis * Basis.from_scale(roof_size), pos + Vector3(0, size.y * 0.5 + roof_size.y * 0.5, 0)))
				roof_colors.append(Color.from_hsv(rng.randf_range(0.0, 0.05), 0.55, rng.randf_range(0.35, 0.55)))
			var shape := CollisionShape3D.new()
			var box := BoxShape3D.new()
			box.size = size
			shape.shape = box
			shape.transform = Transform3D(basis, pos)
			collision.add_child(shape)

	var box_mesh := BoxMesh.new()
	box_mesh.material = _material(Color.WHITE)
	_multimesh(root, "Houses", box_mesh, walls, wall_colors)
	var roof_mesh := PrismMesh.new()
	roof_mesh.material = _material(Color.WHITE, 0.7)
	_multimesh(root, "Roofs", roof_mesh, roofs, roof_colors)

	# Streets.
	var street := MeshInstance3D.new()
	street.name = "Streets"
	var street_mesh := PlaneMesh.new()
	street_mesh.size = Vector2(8, 290)
	var asphalt := StandardMaterial3D.new()
	asphalt.albedo_color = Color(0.22, 0.22, 0.23)
	asphalt.roughness = 0.95
	street_mesh.material = asphalt
	street.mesh = street_mesh
	street.position = TOWN_CENTER + Vector3(0, 0.03, 0)
	root.add_child(street)
	var cross := street.duplicate()
	cross.name = "CrossStreet"
	cross.rotation.y = PI * 0.5
	root.add_child(cross)


static func _add_forest(root: Node3D, rng: RandomNumberGenerator) -> void:
	var crowns := []
	var crown_colors := []
	var trunks := []
	var trunk_colors := []
	var placed := 0
	while placed < 4000:
		var distance := rng.randf_range(250, 4000)
		var angle := rng.randf() * TAU
		var pos := Vector3(cos(angle) * distance, 0, sin(angle) * distance)
		if pos.distance_to(TOWN_CENTER) < 190 or pos.length() < 120:
			continue
		var h := rng.randf_range(10, 24)
		var w := h * rng.randf_range(0.28, 0.4)
		crowns.append(Transform3D(Basis().scaled(Vector3(w, h * 0.8, w)), pos + Vector3(0, h * 0.6, 0)))
		crown_colors.append(Color.from_hsv(rng.randf_range(0.25, 0.33), rng.randf_range(0.45, 0.7), rng.randf_range(0.22, 0.38)))
		trunks.append(Transform3D(Basis().scaled(Vector3(w * 0.12, h * 0.3, w * 0.12)), pos + Vector3(0, h * 0.15, 0)))
		trunk_colors.append(Color(0.3, 0.22, 0.15))
		placed += 1
	var cone := CylinderMesh.new()
	cone.top_radius = 0.0
	cone.bottom_radius = 0.5
	cone.height = 1.0
	cone.radial_segments = 10
	cone.rings = 1
	cone.material = _material(Color.WHITE)
	var crowns_node := _multimesh(root, "Trees", cone, crowns, crown_colors)
	crowns_node.visibility_range_end = 9000.0
	var trunk := CylinderMesh.new()
	trunk.top_radius = 0.5
	trunk.bottom_radius = 0.5
	trunk.height = 1.0
	trunk.radial_segments = 6
	trunk.rings = 1
	trunk.material = _material(Color.WHITE)
	var trunks_node := _multimesh(root, "TreeTrunks", trunk, trunks, trunk_colors)
	trunks_node.visibility_range_end = 2500.0


# Human-sized figures around the town, so you can always read the giant's size.
static func _add_people(root: Node3D, rng: RandomNumberGenerator) -> void:
	var people := []
	var colors := []
	for i in 260:
		var pos := TOWN_CENTER + Vector3(rng.randf_range(-130, 130), 0.9, rng.randf_range(-130, 130))
		if i < 60:
			pos = Vector3(rng.randf_range(-30, 30), 0.9, rng.randf_range(8, 60))
		people.append(Transform3D(Basis(), pos))
		colors.append(Color.from_hsv(rng.randf(), 0.6, 0.8))
	var capsule := CapsuleMesh.new()
	capsule.radius = 0.25
	capsule.height = 1.8
	capsule.radial_segments = 8
	capsule.rings = 2
	capsule.material = _material(Color.WHITE, 0.6)
	var node := _multimesh(root, "People", capsule, people, colors)
	node.visibility_range_end = 1500.0


static func _add_mountains(root: Node3D, rng: RandomNumberGenerator) -> void:
	var peaks := []
	var colors := []
	for i in 70:
		var angle := rng.randf() * TAU
		var distance := rng.randf_range(18000, 60000)
		var h := rng.randf_range(1500, 4500)
		var w := h * rng.randf_range(2.2, 3.5)
		peaks.append(Transform3D(Basis(Vector3.UP, rng.randf() * TAU) * Basis.from_scale(Vector3(w, h, w)), Vector3(cos(angle) * distance, h * 0.5 - 50, sin(angle) * distance)))
		colors.append(Color.from_hsv(0.3, 0.12, rng.randf_range(0.3, 0.42)))
	var cone := CylinderMesh.new()
	cone.top_radius = 0.0
	cone.bottom_radius = 0.5
	cone.height = 1.0
	cone.radial_segments = 7
	cone.rings = 3
	var m := ShaderMaterial.new()
	m.shader = load("res://world/mountain.gdshader")
	cone.material = m
	var node := _multimesh(root, "Mountains", cone, peaks, colors)
	node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
