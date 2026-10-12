-- Load-time test (docs/3ds/NEXT-SPRINT.md B1). Level 0: save at 5 s, teleport
-- to level 1 at 10 s. Level 1: save again at 5 s (one save already there).
Triggers = {}
local ticks = 0
function Triggers.idle()
	ticks = ticks + 1
	if ticks == 150 then
		Game.save()
	elseif ticks == 300 and Level.index == 0 then
		Players[0]:teleport_to_level(1)
	end
end
