return {
    name = "coin",
    sprite = { texture = "textures/sprites", region = "coin_0", w = 0.6, h = 0.6, layer = 6 },
    body = { type = "static", shape = "circle", r = 0.3, sensor = true, category = 4 },
    animation = "coin_spin",
}
