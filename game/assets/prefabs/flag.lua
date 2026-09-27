return {
    name = "flag",
    sprite = { texture = "textures/sprites", region = "flag_0", w = 1, h = 2, layer = 4 },
    body = { type = "static", shape = "aabb", w = 0.6, h = 2, sensor = true, category = 4 },
    animation = "flag_wave",
}
