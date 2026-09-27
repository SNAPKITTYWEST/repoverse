-- Level scene: the platformer. All gameplay lives here; UNIFY owns the lifecycle and calls
-- init/load/update/render/on_contact/on_timer/on_saved/on_loaded at the right scheduler stage.
-- Everything that must survive a save state lives in self.state.
local L = {}

local RUN_SPEED, GROUND_ACCEL, AIR_ACCEL = 7.5, 70, 35
local JUMP_SPEED = 13.5
local COYOTE_TICKS, BUFFER_TICKS = 6, 6   -- grace windows, in simulation ticks
local PARTICLES = 64

local function approach(v, target, delta)
    if v < target then return math.min(v + delta, target) end
    return math.max(v - delta, target)
end

function L:init()
    input:bind("jump", "space", "w", "up", "pad_a")
    input:bind_axis("move", "left", "right", "pad_left_x")
    input:bind_axis("move_alt", "a", "d")
    input:bind("save", "f5")
    input:bind("load", "f9")
    input:bind("menu", "escape")

    local tex = "textures/sprites"
    animation.define{ name = "player_idle", texture = tex, mode = "loop", frames = { { "player_idle_0", 0.5 }, { "player_idle_1", 0.5 } } }
    animation.define{ name = "player_run", texture = tex, mode = "loop",
        frames = { { "player_run_0", 0.08 }, { "player_run_1", 0.08 }, { "player_run_2", 0.08 }, { "player_run_3", 0.08 } } }
    -- Squash and stretch are keyframed tracks blended on top of the sprite frames.
    animation.define{ name = "player_jump", texture = tex, mode = "once", frames = { { "player_jump", 0.4 } },
        tracks = { scale_y = { { 0, 1.2 }, { 0.15, 1 } }, scale_x = { { 0, 0.8 }, { 0.15, 1 } } } }
    animation.define{ name = "player_fall", texture = tex, mode = "loop", frames = { { "player_fall", 1 } } }
    animation.define{ name = "player_land", texture = tex, mode = "once", frames = { { "player_idle_0", 0.12 } },
        tracks = { scale_y = { { 0, 0.75 }, { 0.12, 1 } }, scale_x = { { 0, 1.25 }, { 0.12, 1 } } } }
    animation.define{ name = "coin_spin", texture = tex, mode = "loop",
        frames = { { "coin_0", 0.1 }, { "coin_1", 0.1 }, { "coin_2", 0.1 }, { "coin_3", 0.1 } } }
    animation.define{ name = "flag_wave", texture = tex, mode = "pingpong", frames = { { "flag_0", 0.3 }, { "flag_1", 0.3 } } }
end

function L:load()
    physics:set_gravity(0, -30)
    local s = {
        score = 0, coins = 0, coins_total = 0, jumps = 0, landings = 0, deaths = 0, collisions = 0,
        time = 0, won = false, grounded = false, coyote = 0, fall_speed = 0,
        msg = "", msg_timer = 0, kernel_runs = 0, particles_alive = 0,
        spawn = { 2, 3 }, platforms = {}, clouds = {}, sparks = {}, cam = { 10, 6 },
    }
    self.state = s

    -- Build the level from the tile map. Horizontal runs of ground become one static body each,
    -- so the player never catches on seams between tiles.
    local m = map.load("maps/level1")
    for r = 1, m.height do
        local row, y = m.rows[r], m.height - r
        local run_start = nil
        for x = 0, m.width do
            local ch = row:sub(x + 1, x + 1)
            if ch == "#" then
                run_start = run_start or x
                local above = r > 1 and m.rows[r - 1]:sub(x + 1, x + 1) or " "
                world:spawn(above == "#" and "prefabs/tile" or "prefabs/tile_top", x + 0.5, y + 0.5)
            else
                if run_start then
                    local w = x - run_start
                    physics:add_static_box(run_start + w / 2, y + 0.5, w, 1)
                    run_start = nil
                end
                if ch == "P" then s.spawn = { x + 0.5, y + 0.8 }
                elseif ch == "C" then world:spawn("prefabs/coin", x + 0.5, y + 0.5); s.coins_total = s.coins_total + 1
                elseif ch == "B" then world:spawn("prefabs/crate", x + 0.5, y + 0.45)
                elseif ch == "O" then world:spawn("prefabs/ball", x + 0.5, y + 0.5)
                elseif ch == "F" then world:spawn("prefabs/flag", x + 0.5, y + 1)
                elseif ch == "M" then
                    s.platforms[#s.platforms + 1] = { e = world:spawn("prefabs/platform", x + 1.5, y + 0.5), phase = #s.platforms }
                end
            end
        end
    end
    s.player = world:spawn("prefabs/player", s.spawn[1], s.spawn[2])
    s.sky = world:spawn("prefabs/sky", 0, 0)
    for i = 1, 6 do s.clouds[i] = { e = world:spawn("prefabs/cloud", i * 13, 9), x = i * 13, y = 8 + (i % 3) * 1.3 } end

    -- Sparkle particles are integrated by a WASM kernel compiled from CUDA-like source.
    s.kernel = kernel.load(kernel.compile("kernels/particles"))
    s.px, s.py = buffer.create(PARTICLES), buffer.create(PARTICLES)
    s.vx, s.vy = buffer.create(PARTICLES), buffer.create(PARTICLES)
    s.life = buffer.create(PARTICLES)
    for i = 1, PARTICLES do
        local e = world:spawn("prefabs/spark", -100, -100)
        e:set_visible(false)
        s.sparks[i] = e
    end

    audio:stream("music/theme", 0.45, true, 1.0)
    s.cam = { s.spawn[1], 6 }
    camera:set(s.cam[1], s.cam[2], 32)
end

function L:burst(x, y, count)
    local s = self.state
    local life = buffer.download(s.life)
    local px, py = buffer.download(s.px), buffer.download(s.py)
    local vx, vy = buffer.download(s.vx), buffer.download(s.vy)
    local spawned = 0
    for i = 1, PARTICLES do
        if spawned >= count then break end
        if life[i] <= 0 then
            local angle = math.random() * 2 * math.pi
            local speed = 2 + math.random() * 4
            px[i], py[i] = x, y
            vx[i], vy[i] = math.cos(angle) * speed, math.sin(angle) * speed + 3
            life[i] = 0.6 + math.random() * 0.4
            spawned = spawned + 1
        end
    end
    buffer.upload(s.px, px); buffer.upload(s.py, py)
    buffer.upload(s.vx, vx); buffer.upload(s.vy, vy)
    buffer.upload(s.life, life)
    s.particles_alive = s.particles_alive + spawned
end

function L:update_particles(dt)
    local s = self.state
    if s.particles_alive <= 0 then return end
    kernel.dispatch(s.kernel, 1, PARTICLES, s.px, s.py, s.vx, s.vy, s.life, PARTICLES, dt, -20.0)
    s.kernel_runs = s.kernel_runs + 1
    local px, py, life = buffer.download(s.px), buffer.download(s.py), buffer.download(s.life)
    local alive = 0
    for i = 1, PARTICLES do
        local e = s.sparks[i]
        if life[i] > 0 then
            alive = alive + 1
            e:set_position(px[i], py[i], true)
            e:set_visible(true)
        else
            e:set_visible(false)
        end
    end
    s.particles_alive = alive
end

function L:respawn()
    local s = self.state
    s.player:set_position(s.spawn[1], s.spawn[2], true)
    s.player:set_velocity(0, 0)
    s.deaths = s.deaths + 1
    s.msg, s.msg_timer = "OOPS", 45
end

function L:update_player(dt)
    local s, p = self.state, self.state.player
    local x, y = p:position()
    local vx, vy = p:velocity()

    -- Ground probe: two short rays below the feet, ignoring the player's own body.
    local grounded = false
    for _, ox in ipairs({ -0.33, 0.33 }) do
        local hit, _, _, _, ny = physics:raycast(x + ox, y - 0.6, x + ox, y - 0.85, 65535, p)
        if hit and ny > 0.5 then grounded = true end
    end
    if grounded and not s.grounded and s.fall_speed > 6 then
        audio:play("audio/land", 0.6)
        s.landings = s.landings + 1
        p:set_animation("player_land", 0, true)
    end
    if grounded then s.coyote = COYOTE_TICKS else s.coyote = math.max(0, s.coyote - 1) end
    if grounded then s.fall_speed = 0 else s.fall_speed = math.max(s.fall_speed, -vy) end
    s.grounded = grounded

    local move = math.max(-1, math.min(1, input:axis("move") + input:axis("move_alt")))
    vx = approach(vx, move * RUN_SPEED, (grounded and GROUND_ACCEL or AIR_ACCEL) * dt)

    -- Buffered jump + coyote time: a press up to BUFFER_TICKS before landing, or a jump up to
    -- COYOTE_TICKS after walking off a ledge, still counts.
    if input:buffered("jump", BUFFER_TICKS) and s.coyote > 0 then
        input:consume("jump")
        vy = JUMP_SPEED
        s.coyote = 0
        grounded = false  -- airborne from this tick on: don't let the ground logic below override the jump
        s.jumps = s.jumps + 1
        audio:play("audio/jump", 0.7, 0, 0.95 + math.random() * 0.1)
        p:set_animation("player_jump", 0, true)
    elseif input:is_released("jump") and vy > 0 then
        vy = vy * 0.5  -- short hop when the button is released early
    end
    p:set_velocity(vx, vy)

    local anim, finished = p:animation()
    if grounded then
        if anim ~= "player_land" or finished then
            p:set_animation(math.abs(vx) > 0.5 and "player_run" or "player_idle", 0.05)
        end
    elseif (anim == "player_jump" and finished) or (anim ~= "player_jump" and vy < -1) then
        p:set_animation("player_fall")
    end
    if move ~= 0 then p:set_flip(move < 0) end
    if y < -6 then self:respawn() end
end

function L:update_platforms()
    local s = self.state
    for _, pl in ipairs(s.platforms) do
        pl.e:set_velocity(2.5 * math.cos(s.time * 0.9 + pl.phase), 0)
    end
end

function L:update_camera()
    local s = self.state
    local x, y = s.player:position()
    s.cam[1] = s.cam[1] + (x - s.cam[1]) * 0.12
    s.cam[2] = s.cam[2] + (math.max(y + 1.5, 5.6) - s.cam[2]) * 0.08
    s.cam[1] = math.max(10, math.min(64, s.cam[1]))
    camera:set(s.cam[1], s.cam[2])
    s.sky:set_position(s.cam[1], s.cam[2], true)
    for i, c in ipairs(s.clouds) do
        local cx = c.x - s.cam[1] * 0.4
        cx = cx - math.floor(cx / 78) * 78 + s.cam[1] - 30  -- wrap across the view with parallax
        c.e:set_position(cx, c.y + s.cam[2] * 0.3, true)
    end
end

function L:update(dt)
    local s = self.state
    s.time = s.time + dt
    if s.msg_timer > 0 then s.msg_timer = s.msg_timer - 1 end
    if input:is_pressed("save") then save_state(); save_state("slot1") end
    if input:is_pressed("load") then load_state() end
    if input:is_pressed("menu") then scene.transition("title"); return end
    self:update_player(dt)
    self:update_platforms()
    self:update_particles(dt)
    self:update_camera()
end

function L:on_contact(a, b, kind, nx, ny, impulse)
    local s = self.state
    if a == nil or b == nil then return end
    local other = (a == s.player and b) or (b == s.player and a) or nil
    if kind == "trigger_begin" and other then
        local name = other:name()
        if name == "coin" then
            local x, y = other:position()
            other:destroy()
            s.coins, s.score = s.coins + 1, s.score + 100
            audio:play("audio/coin", 0.7)
            self:burst(x, y, 16)
        elseif name == "flag" and not s.won then
            local x, y = other:position()
            s.won, s.score = true, s.score + 1000
            audio:play("audio/win", 0.8)
            self:burst(x, y + 0.5, 40)
            timer.after(4, "back_to_title")
        end
    elseif kind == "begin" then
        s.collisions = s.collisions + 1
        local ball = (a:name() == "ball") or (b:name() == "ball")
        if ball and impulse > 1.0 then audio:play("audio/bounce", 0.5, 0, 0.9 + math.random() * 0.2) end
    end
end

function L:on_timer(name)
    if name == "back_to_title" then scene.transition("title") end
end

function L:on_saved(ok, slot)
    if slot == "" then self.state.msg, self.state.msg_timer = ok and "STATE SAVED" or "SAVE FAILED", 90 end
end

function L:on_loaded(ok, slot)
    self.state.msg, self.state.msg_timer = ok and "STATE RESTORED" or "NO SAVE", 90
end

local function centered(text, y, scale, r, g, b)
    local w = ui:screen()
    ui:text(text, (w - ui:width(text, scale)) / 2, y, scale, r, g, b)
end

function L:render()
    local s = self.state
    local w, h = ui:screen()
    ui:rect(0, 0, w, 22, 0, 0, 0, 150)
    ui:text("SCORE " .. s.score, 8, 5, 2)
    ui:text("COINS " .. s.coins .. "/" .. s.coins_total, 160, 5, 2, 255, 220, 90)
    ui:text("JUMPS " .. s.jumps, 330, 5, 2, 150, 210, 255)
    ui:text(string.format("%.1fS", s.time), 560, 5, 2, 200, 200, 200)
    local st = engine.stats()
    ui:rect(0, h - 30, w, 30, 0, 0, 0, 150)
    ui:text("ARROWS/AD MOVE  SPACE JUMP  F5 SAVE  F9 LOAD  ESC MENU", 8, h - 26, 1, 210, 210, 210)
    ui:text(string.format("ENT %d  BATCHES %d  AWAKE %d  LUA %dK  WASM RUNS %d  PARTICLES %d",
        st.entities, st.batches, st.awake_bodies, st.lua_bytes // 1024, s.kernel_runs, s.particles_alive), 8, h - 12, 1, 150, 170, 200)
    if s.msg_timer > 0 then centered(s.msg, 80, 3, 255, 255, 160) end
    if s.won then centered("LEVEL COMPLETE", 130, 4, 140, 255, 160) end
end

return L
