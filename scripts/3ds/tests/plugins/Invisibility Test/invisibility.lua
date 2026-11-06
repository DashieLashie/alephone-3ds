-- Stage 10 M4 test (docs/3ds/CITRO3D_DESIGN.md, M4 specification): the player's invisibility is held in
-- [1500, 2000] ticks, at most kINVISIBILITY_DURATION (2100: _xfer_invisibility, tint k 15) and never under 10 s
-- (player.cpp:1993-2000: the weapon would flicker to no transfer mode). On the first idle tick two invisible
-- compilers are created 2 and 3 WU ahead of the player (tinted sprites; recorded, not gated).
Triggers = {}
local spawned = false

local function keep_invisible()
	local p = Players[0]
	if p.invisibility_duration < 1500 then
		p.invisibility_duration = 2000
	end
end

local function spawn(dist, side, kind)
	local p = Players[0]
	local a = math.rad(p.yaw)
	local x = p.x + dist * math.cos(a) - side * math.sin(a)
	local y = p.y + dist * math.sin(a) + side * math.cos(a)
	local poly = p.polygon:find_polygon(p.x, p.y, x, y)
	if poly then
		Monsters.new(x, y, poly.floor.height + 0.25, poly, kind)
	end
end

function Triggers.init()
	keep_invisible()
end

function Triggers.idle()
	keep_invisible()
	if not spawned then
		spawned = true
		pcall(spawn, 2, -0.5, "minor invisible compiler")
		pcall(spawn, 3, 0.5, "major invisible compiler")
	end
end
