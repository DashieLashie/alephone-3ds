-- Stage 7 test (docs/3ds/STAGE7.md): terminal 0, 3 s into the level.
Triggers = {}
local ticks = 0
function Triggers.idle()
	ticks = ticks + 1
	if ticks == 90 then
		Players[0]:activate_terminal(0)
	end
end
