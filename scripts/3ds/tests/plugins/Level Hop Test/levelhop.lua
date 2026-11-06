-- Stage 10 M4 test (docs/3ds/CITRO3D_DESIGN.md, M4 specification): 10 s into an even-numbered level (the bench levels
-- 0, 6 and 20 are even), teleport to the next level. Lua restarts with every level, so the odd level it lands on stays.
Triggers = {}
local ticks = 0
function Triggers.idle()
	ticks = ticks + 1
	if ticks == 300 and Level.index % 2 == 0 then
		Players[0]:teleport_to_level(Level.index + 1)
	end
end
