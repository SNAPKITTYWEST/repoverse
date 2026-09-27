return {
    name = "platform",
    sprite = { texture = "textures/sprites", region = "platform", w = 3, h = 0.75, layer = 3 },
    body = { type = "kinematic", shape = "aabb", w = 3, h = 0.5, friction = 1.0, category = 1 },
}
