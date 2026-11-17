-- Stage 10 M6b test (docs/3ds/CITRO3D_DESIGN.md, M6b specification "Tests" (3)). 3 s in, the player is moved to the
-- floor of the first polygon whose water is at least 1.2 world units deep (the camera then under the surface); with
-- none, the player's own polygon gets the level's first water media, raised 2 units above its floor (the fallback that
-- needs no particular level); with no water media at all the test fails loudly. The player is put back on that floor
-- every tick (a swimmer floats up to the surface within ~3 s). Then a flash on top every 2 s: "red"
-- (part 1, liquid + flash) until 33 s, "negative" after (part 2 over a liquid).
Triggers = {}
local ticks, placed, poly_index, spot = 0, false, nil, nil

local function fail(msg)
	print("[lua] ERROR liquid test: " .. msg)
	error("liquid test: " .. msg)
end

local function place()
	for p in Polygons() do
		local m = p.media
		if m and m.type.mnemonic == "water" and m.height - p.floor.height >= 1.2 then
			spot = {p.x, p.y, p.floor.height, p}
			Players[0]:position(p.x, p.y, p.floor.height, p)
			print(string.format("[lua] liquid test: polygon %d, media %d (%s), depth %.2f", p.index, m.index,
				m.type.mnemonic, m.height - p.floor.height))
			return p.index
		end
	end
	for m in Media() do
		if m.type.mnemonic == "water" then
			local p = Players[0].polygon
			p.media = m
			m.low = p.floor.height + 2
			m.high = p.floor.height + 2
			spot = {Players[0].x, Players[0].y, p.floor.height, p}
			print(string.format("[lua] liquid test: fallback, polygon %d given media %d (%s) at %.2f", p.index, m.index,
				m.type.mnemonic, p.floor.height + 2))
			return p.index
		end
	end
	fail("no water media in this level")
end

function Triggers.idle()
	ticks = ticks + 1
	if ticks == 90 then
		poly_index = place()
		placed = true
	end
	if placed and spot then
		Players[0]:position(spot[1], spot[2], spot[3], spot[4])
	end
	if placed and ticks > 90 and (ticks - 90) % 60 == 0 then
		local kind = ticks < 990 and "red" or "negative"
		Players[0]:fade_screen(kind)
		print(string.format("[lua] liquid test: %s flash at tick %d (polygon %d, player in %d)", kind, ticks, poly_index,
			Players[0].polygon.index))
	end
end
