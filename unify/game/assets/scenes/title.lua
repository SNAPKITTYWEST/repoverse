-- Title scene: runs the WASM compute self-test (C[i] = A[i] + B[i]) and starts the music stream.
local S = {}

function S:init()
    input:bind("start", "enter", "space", "pad_start", "pad_a")
    input:bind("quit", "escape")
end

function S:load()
    local st = { t = 0, kernel_ok = false, result = {} }
    self.state = st

    -- kernel.compile: CUDA-like source -> AST -> IR -> WASM; kernel.load: instantiate in the WASM runtime.
    local compiled = kernel.compile("kernels/vadd")
    assert(compiled.ok, compiled.error)
    local k = kernel.load(compiled)
    local n = 8
    local A, B, C = buffer.create(n), buffer.create(n), buffer.create(n)
    local a, b = {}, {}
    for i = 1, n do a[i] = i; b[i] = i * 10 end
    buffer.upload(A, a)
    buffer.upload(B, b)
    st.kernel_instructions = kernel.dispatch(k, 1, n, A, B, C, n)
    local c = buffer.download(C)
    local ok = true
    for i = 1, n do
        if c[i] ~= a[i] + b[i] then ok = false end
    end
    st.kernel_ok = ok
    st.result = c
    st.wasm_bytes = compiled.wasm_bytes
    log(string.format("vadd kernel: %s (%d wasm bytes, %d instructions)", ok and "OK" or "FAIL", compiled.wasm_bytes, st.kernel_instructions))

    audio:stream("music/theme", 0.5, true, 1.5)
    camera:set(0, 0, 32)
end

function S:update(dt)
    self.state.t = self.state.t + dt
    if input:is_pressed("start") then scene.transition("level") end
    if input:is_pressed("quit") then engine.quit() end
end

local function centered(text, y, scale, r, g, b)
    local w = ui:screen()
    ui:text(text, (w - ui:width(text, scale)) / 2, y, scale, r, g, b)
end

function S:render()
    local st = self.state
    local w, h = ui:screen()
    ui:rect(0, 0, w, h, 22, 26, 44, 255)
    ui:rect(0, h - 90, w, 90, 30, 38, 64, 255)
    centered("UNIFY", 60, 10, 110, 190, 255)
    centered("2D ENGINE RUNTIME", 150, 2, 200, 210, 230)
    if math.floor(st.t * 2) % 2 == 0 then centered("PRESS ENTER OR SPACE", 200, 2, 255, 230, 120) end
    local nums = {}
    for i = 1, #st.result do nums[i] = string.format("%g", st.result[i]) end
    ui:text("WASM KERNEL  C[i] = A[i] + B[i] : " .. (st.kernel_ok and "OK" or "FAIL"), 16, h - 76, 1.5,
        st.kernel_ok and 140 or 255, st.kernel_ok and 230 or 90, 140)
    ui:text("C = " .. table.concat(nums, " "), 16, h - 56, 1.5, 180, 190, 210)
    ui:text(string.format("%d BYTES OF WASM, %d INSTRUCTIONS INTERPRETED", st.wasm_bytes or 0, st.kernel_instructions or 0), 16, h - 36, 1.5, 150, 160, 190)
end

return S
