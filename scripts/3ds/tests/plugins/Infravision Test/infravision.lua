-- Stage 10 M3 test (docs/3ds/CITRO3D_DESIGN.md, M3 specification): infravision from the level's start.
Triggers = {}
function Triggers.init()
	Players[0].infravision_duration = 3000
end
function Triggers.idle()
	if Players[0].infravision_duration < 1500 then
		Players[0].infravision_duration = 3000
	end
end
