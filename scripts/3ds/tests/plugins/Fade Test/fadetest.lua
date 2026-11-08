-- Stage 10 M6b test (docs/3ds/CITRO3D_DESIGN.md, M6b specification "Tests" (2)): every fade type by index, from 3 s in.
-- A fade of higher priority still running drops a new one silently (fades.cpp explicit_start_fade), so each type waits
-- until the previous one has ended: max(1.5 s, its period + 1/3 s), the periods of fades.cpp's fade_definitions in game
-- ticks (30 a second). _fade_long_bright (9, priority 1, 4 s) goes last; _end_cinematic_fade_out (4, the identity)
-- comes straight after _cinematic_fade_out (3), where it restores the picture. Each start is logged ("[lua] fade").
Triggers = {}
local period = {
	[0] = 0, 15, 45, 15, 0, 8, 23, 8, 10, 120, 15, 30, 8, 15, 8, 15, 8, 90, 15, 90,
	12, 15, 45, 15, 23, 30, 45, 60, 60, 60, 60, 60, 60
}
local order = {}
for i = 0, 32 do
	if i ~= 9 then order[#order + 1] = i end
end
order[#order + 1] = 9
local ticks, step, next_at = 0, 1, 90
function Triggers.idle()
	ticks = ticks + 1
	if step > #order or ticks < next_at then return end
	local t = order[step]
	Players[0]:fade_screen(t)
	print(string.format("[lua] fade %d (%s) at tick %d", t, FadeTypes[t].mnemonic, ticks))
	next_at = ticks + math.max(45, period[t] + 10)
	step = step + 1
	if step > #order then
		print(string.format("[lua] fade test: %d types started, last ends at tick %d", #order, next_at))
	end
end
