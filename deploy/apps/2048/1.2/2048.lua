-- 2048 — wada.* reference app
-- Swipe (or trackball mapped swipe) to move tiles.
-- Tap after win or game over to restart.

local ui, sys, store, timer = wada.ui, wada.sys, wada.store, wada.timer
local C = ui.colors

local app = {}

local cv, score_lbl

local CELL = 38

local grid = {}

local history = {
    grids = {},
    maxsize = 5
}

local px_w, px_h
local score = 0
local hiscore = 0

local over = false
local won = false


--------------------------------------------------
-- History
--------------------------------------------------

function history:save()

    local old = {
        grid = {},
        score = score,
        over = over,
        won = won
    }

    for x = 1, 4 do

        old.grid[x] = {}

        for y = 1, 4 do
            old.grid[x][y] = grid[x][y]
        end

    end

    table.insert(self.grids, old)

    if #self.grids > self.maxsize then
        table.remove(self.grids, 1)
    end

end


function history:revert()

    if #self.grids > 0 then

        local old = table.remove(self.grids)

        grid = old.grid
        score = old.score
        over = old.over
        won = old.won

    end

end


--------------------------------------------------
-- Game logic
--------------------------------------------------

local function clear_grid()

    grid = {}

    for x = 1, 4 do

        grid[x] = {}

        for y = 1, 4 do
            grid[x][y] = 0
        end

    end

end


--------------------------------------------------
-- Add random tile
--------------------------------------------------

local function add_random_tile()

    local free = {}

    for x = 1, 4 do

        for y = 1, 4 do

            if grid[x][y] == 0 then

                table.insert(
                    free,
                    {
                        x = x,
                        y = y
                    }
                )

            end

        end

    end


    -- Board is full.
    if #free == 0 then
        return false
    end


    local p


    -- If there is only one empty cell,
    -- use it directly.
    if #free == 1 then

        p = free[1]

    else

        local index = sys.random(1, #free)

        if index == nil then
            return false
        end

        p = free[index]

        if p == nil then
            return false
        end

    end


    -- 10% chance for 4,
    -- 90% chance for 2.
    if sys.random(0, 9) == 0 then
        grid[p.x][p.y] = 4
    else
        grid[p.x][p.y] = 2
    end

    return true

end


--------------------------------------------------
-- Reset
--------------------------------------------------

local function reset()

    clear_grid()

    score = 0
    over = false
    won = false

    history.grids = {}

    add_random_tile()
    add_random_tile()

end


--------------------------------------------------
-- Compress line
--------------------------------------------------

local function compress(line)

    local r = {}

    for i = 1, 4 do

        if line[i] ~= 0 then
            table.insert(r, line[i])
        end

    end

    while #r < 4 do
        table.insert(r, 0)
    end

    return r

end


--------------------------------------------------
-- Merge line
--------------------------------------------------

local function merge(line)

    local r = {}
    local i = 1

    while i <= 4 do

        if i < 4
            and line[i] ~= 0
            and line[i] == line[i + 1]
        then

            local v = line[i] * 2

            table.insert(r, v)

            score = score + v


            -- Permanent high score.
            if score > hiscore then

                hiscore = score

                store.set(
                    "2048_best",
                    hiscore
                )

            end


            -- Reaching 2048 means victory.
            if v >= 2048 then
                won = true
            end


            i = i + 2

        else

            table.insert(r, line[i])

            i = i + 1

        end

    end


    while #r < 4 do
        table.insert(r, 0)
    end

    return r

end


--------------------------------------------------
-- Move left
--------------------------------------------------

local function move_left()

    local changed = false

    for y = 1, 4 do

        local old = {}
        local line = {}

        for x = 1, 4 do

            old[x] = grid[x][y]
            line[x] = grid[x][y]

        end


        line = compress(line)
        line = merge(line)


        for x = 1, 4 do

            grid[x][y] = line[x]

            if old[x] ~= line[x] then
                changed = true
            end

        end

    end

    return changed

end


--------------------------------------------------
-- Rotate
--------------------------------------------------

local function rotate()

    local n = {}

    for x = 1, 4 do
        n[x] = {}
    end


    for x = 1, 4 do

        for y = 1, 4 do

            n[y][5 - x] = grid[x][y]

        end

    end


    grid = n

end


--------------------------------------------------
-- Game over
--------------------------------------------------

local function game_over()

    -- Any empty cell means the game can continue.
    for x = 1, 4 do

        for y = 1, 4 do

            if grid[x][y] == 0 then
                return false
            end

        end

    end


    -- Check horizontal and vertical merges.
    for x = 1, 4 do

        for y = 1, 4 do

            if x < 4
                and grid[x][y] == grid[x + 1][y]
            then
                return false
            end


            if y < 4
                and grid[x][y] == grid[x][y + 1]
            then
                return false
            end

        end

    end


    -- Board is full and no merge is possible.
    return true

end


--------------------------------------------------
-- Move
--------------------------------------------------

local function move(dir)

    -- Do nothing after victory or defeat.
    if over or won then
        return
    end


    history:save()

    local changed = false


    if dir == "left" then

        changed = move_left()


    elseif dir == "right" then

        rotate()
        rotate()

        changed = move_left()

        rotate()
        rotate()


    elseif dir == "up" then

        rotate()

        changed = move_left()

        rotate()
        rotate()
        rotate()


    elseif dir == "down" then

        rotate()
        rotate()
        rotate()

        changed = move_left()

        rotate()

    end


    --------------------------------------------------
    -- Nothing moved.
    --------------------------------------------------

    if not changed then

        history:revert()

        return

    end


    --------------------------------------------------
    -- Successful move.
    -- Exactly one new tile must be created.
    --------------------------------------------------

    local added = add_random_tile()


    --------------------------------------------------
    -- This should only happen if the board was already
    -- completely full. Normally a successful move on a
    -- full board creates space, so added should be true.
    --------------------------------------------------

    if not added then

        over = true

        return

    end


    --------------------------------------------------
    -- Victory.
    --
    -- merge() sets won when a 2048 tile is created.
    --------------------------------------------------

    if won then
        return
    end


    --------------------------------------------------
    -- IMPORTANT:
    --
    -- Game-over is checked AFTER the new random tile
    -- has been inserted.
    --------------------------------------------------

    if game_over() then
        over = true
    end

end


--------------------------------------------------
-- Tile colors
--------------------------------------------------

local function tile_color(v)

    if v == 0 then
        return 0xCDC1B4

    elseif v == 2 then
        return 0xEEE4DA

    elseif v == 4 then
        return 0xEDE0C8

    elseif v == 8 then
        return 0xF2B179

    elseif v == 16 then
        return 0xF59563

    elseif v == 32 then
        return 0xF67C5F

    elseif v == 64 then
        return 0xF65E3B

    elseif v == 128 then
        return 0xEDCF72

    elseif v == 256 then
        return 0xEDCC61

    elseif v == 512 then
        return 0xEDC850

    elseif v == 1024 then
        return 0xEDC53F

    elseif v == 2048 then
        return 0xEDC22E

    end

    -- Values above 2048.
    return 0x3C3A32

end


--------------------------------------------------
-- Text colors
--------------------------------------------------

local function text_color(v)

    if v <= 4 then
        return 0x776E65
    else
        return 0xFFFFFF
    end

end


--------------------------------------------------
-- Text size
--------------------------------------------------

local function text_size(v)

    if v < 100 then
        return 16

    elseif v < 1000 then
        return 14

    elseif v < 10000 then
        return 11

    else
        return 9
    end

end


--------------------------------------------------
-- Text positioning
--------------------------------------------------

local function text_x(px, v, size)

    local width

    if v < 100 then
        width = size * 1.1

    elseif v < 1000 then
        width = size * 1.7

    elseif v < 10000 then
        width = size * 2.3

    else
        width = size * 2.9
    end


    return px + math.floor(
        (CELL - width) / 2
    )

end


local function text_y(py, size)

    return py + math.floor(
        (CELL - size) / 2
    )

end


--------------------------------------------------
-- Score line
--------------------------------------------------

local function scoreline()

    local status = ""

    if won then

        status = "   - tap retry"

    elseif over then

        status = "   - tap retry"

    end


    score_lbl:set(
        string.format(
            "Score %d   Best %d%s",
            score,
            hiscore,
            status
        )
    )

end


--------------------------------------------------
-- Draw
--------------------------------------------------

local function draw()

    cv:fill(0x101418)


    for x = 1, 4 do

        for y = 1, 4 do

            local px = (x - 1) * CELL
            local py = (y - 1) * CELL

            local v = grid[x][y]


            cv:rect(
                px + 2,
                py + 2,
                CELL - 4,
                CELL - 4,
                tile_color(v),
                true,
                4
            )


            if v > 0 then

                local size = text_size(v)

                cv:text(
                    text_x(px, v, size),
                    text_y(py, size),
                    tostring(v),
                    text_color(v),
                    size
                )

            end

        end

    end


    --------------------------------------------------
    -- End messages
    --------------------------------------------------

    if won then

        cv:text(
            30,
            px_h / 2,
            "YOU WIN!",
            C.good,
            18
        )

    elseif over then

        cv:text(
            30,
            px_h / 2,
            "GAME OVER",
            C.bad,
            18
        )

    end

end


--------------------------------------------------
-- WADAMESH API
--------------------------------------------------

function app.on_open(w, h)

    hiscore = store.get(
        "2048_best",
        0
    )


    px_w = CELL * 4
    px_h = CELL * 4


    score_lbl = ui.label(
        "",
        4,
        4,
        14,
        C.text
    )


    cv = ui.canvas(
        px_w,
        px_h
    )


    cv:pos(
        math.floor(
            (w - px_w) / 2
        ),
        28
    )


    reset()

    scoreline()
    draw()


    timer.every(100)

end


function app.on_input(ev)

    if ev.type == "swipe" then

        move(ev.dir)

        scoreline()
        draw()


    elseif ev.type == "down"
        and (over or won)
    then

        reset()

        scoreline()
        draw()

    end

end


function app.on_tick(dt)

end


function app.on_close()

end


return app
