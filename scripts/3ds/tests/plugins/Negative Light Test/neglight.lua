-- Stage 10 M3 test (docs/3ds/CITRO3D_DESIGN.md, M3 specification): every light state at intensity -0.5.
-- A state's intensity is read when the light enters it (lightsource.cpp change_light_state), so every light is
-- toggled once to enter a new state now; the states after it are negative too.
Triggers = {}
function Triggers.init()
	for i = 0, #Lights - 1 do
		local light = Lights[i]
		for s = 0, #light.states - 1 do
			light.states[s].intensity = -0.5
			light.states[s].delta_intensity = 0
		end
		light.active = not light.active
	end
end
