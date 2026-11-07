-- Stage 10 M5 test (docs/3ds/CITRO3D_DESIGN.md, M5 specification; critic, M5 code review major 2): pitch the view up
-- (0 -> 28 degrees over 120 ticks), then sway between 14 and 28 degrees (a 4 s period), so the landscape row-512 cut
-- crosses the screen.
Triggers = {}
local ticks = 0
function Triggers.idle()
	ticks = ticks + 1
	local e
	if ticks <= 120 then
		e = 28 * ticks / 120
	else
		e = 21 + 7 * math.cos((ticks - 120) * math.pi / 60)
	end
	Players[0].elevation = e
end
