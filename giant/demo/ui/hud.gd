class_name Hud
extends CanvasLayer
## On-screen controls for the prototype.

const MIN_HEIGHT := 1.8
const MAX_HEIGHT := 1500.0
const GROW_SECONDS := 6.0

var main: Node

var _height_label: Label
var _height_slider: HSlider
var _feel_label: Label
var _info_label: Label
var _syncing := false


func _ready() -> void:
	var panel := PanelContainer.new()
	panel.position = Vector2(12, 12)
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.06, 0.07, 0.09, 0.82)
	style.set_corner_radius_all(8)
	style.set_content_margin_all(12)
	panel.add_theme_stylebox_override("panel", style)
	add_child(panel)

	var box := VBoxContainer.new()
	box.custom_minimum_size.x = 330
	box.add_theme_constant_override("separation", 6)
	panel.add_child(box)

	var title := Label.new()
	title.text = "Giant prototype"
	title.add_theme_font_size_override("font_size", 20)
	box.add_child(title)

	_height_label = Label.new()
	box.add_child(_height_label)
	_height_slider = HSlider.new()
	_height_slider.min_value = 0.0
	_height_slider.max_value = 1.0
	_height_slider.step = 0.001
	_height_slider.value_changed.connect(_on_height_slider)
	box.add_child(_height_slider)

	var grow_row := HBoxContainer.new()
	box.add_child(grow_row)
	for h in [1.8, 20.0, 100.0, 500.0, 1500.0]:
		var button := Button.new()
		button.text = _format_height(h)
		button.tooltip_text = "Grow smoothly to %s" % _format_height(h)
		button.pressed.connect(func(): main.giant.grow_to(h, GROW_SECONDS))
		button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		grow_row.add_child(button)

	_feel_label = Label.new()
	box.add_child(_feel_label)
	var feel := HSlider.new()
	feel.min_value = 0.0
	feel.max_value = 1.0
	feel.step = 0.01
	feel.value = main.giant.time_feel
	feel.value_changed.connect(func(v): main.giant.time_feel = v)
	box.add_child(feel)

	box.add_child(_toggle("Walk", main.walking, func(on): main.set_walking(on)))
	box.add_child(_toggle("Jiggle and hair physics", true, func(on): main.set_jiggle(on)))
	box.add_child(_toggle("Detail layers (no texture stretch)", true, func(on): main.set_detail_layers(on)))

	var cams := HBoxContainer.new()
	box.add_child(cams)
	for entry in [["1 Orbit", CameraRig.Mode.ORBIT], ["2 On foot", CameraRig.Mode.ON_FOOT], ["3 Fly", CameraRig.Mode.FLY]]:
		var button := Button.new()
		button.text = entry[0]
		button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		var cam_mode: int = entry[1]
		button.pressed.connect(func(): main.rig.set_mode(cam_mode))
		cams.add_child(button)

	var help := Label.new()
	help.text = "Orbit: right-drag to turn, wheel to zoom\nOn foot and fly: click to look, Esc to free the mouse\nWASD to move, Shift to run, Space to jump, Q/E down/up"
	help.add_theme_font_size_override("font_size", 12)
	help.modulate = Color(1, 1, 1, 0.7)
	box.add_child(help)

	_info_label = Label.new()
	_info_label.add_theme_font_size_override("font_size", 12)
	_info_label.modulate = Color(1, 1, 1, 0.7)
	box.add_child(_info_label)
	_no_focus(panel)


# Controls never take keyboard focus, so Space and WASD always reach the camera.
func _no_focus(node: Node) -> void:
	if node is Control:
		node.focus_mode = Control.FOCUS_NONE
	for child in node.get_children():
		_no_focus(child)


func _toggle(text: String, on: bool, callback: Callable) -> CheckBox:
	var check := CheckBox.new()
	check.text = text
	check.button_pressed = on
	check.toggled.connect(callback)
	return check


func _process(_delta: float) -> void:
	var giant: GiantBody = main.giant
	_height_label.text = "Height: %s" % _format_height(giant.height)
	_syncing = true
	_height_slider.value = log(giant.height / MIN_HEIGHT) / log(MAX_HEIGHT / MIN_HEIGHT)
	_syncing = false
	var feel := "in between"
	if giant.time_feel < 0.15:
		feel = "moves like a human"
	elif giant.time_feel > 0.85:
		feel = "real physics"
	_feel_label.text = "Time feel: %s (moves at %d%% speed)" % [feel, roundi(giant.get_time_scale() * 100.0)]
	_info_label.text = "%d FPS" % Engine.get_frames_per_second()


func _on_height_slider(value: float) -> void:
	if _syncing:
		return
	main.giant.stop_growing()
	main.giant.height = MIN_HEIGHT * pow(MAX_HEIGHT / MIN_HEIGHT, value)


static func _format_height(h: float) -> String:
	if h >= 1000.0:
		return "%.1f km" % (h / 1000.0)
	if h >= 10.0:
		return "%d m" % roundi(h)
	return "%.1f m" % h
