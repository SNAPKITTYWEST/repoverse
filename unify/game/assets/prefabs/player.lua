return {
    name = "player",
    sprite = { texture = "textures/sprites", region = "player_idle_0", w = 1, h = 1.5, layer = 10 },
    body = { type = "dynamic", shape = "aabb", w = 0.7, h = 1.4, friction = 0.2, fixed_rotation = true,
             category = 2, mask = 65535, can_sleep = false },
    animation = "player_idle",
}
