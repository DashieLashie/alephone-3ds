-- Stage 2 play/verify driver (needs -i for io/os). On level 1 it quick-saves,
-- then teleports to level 2; on level 2 it quick-saves every 2 s so the
-- harness can quit without the "quit without saving?" prompt.
local log = io.open(os.getenv("HOME") .. "/harness.log", "a")
local function say(fmt, ...)
  log:write(string.format("%d %s\n", Game.ticks, string.format(fmt, ...)))
  log:flush()
end

local level_ticks = 0

Triggers = {}

function Triggers.init(restoring)
  level_ticks = 0
  say("init level=%d name=%s restoring=%s", Level.index, Level.name, tostring(restoring))
end

function Triggers.idle()
  level_ticks = level_ticks + 1
  if Level.index == 0 then
    if level_ticks == 20 * 30 then
      Game.save(); say("saved level=%d", Level.index)
    elseif level_ticks == 22 * 30 then
      say("teleport to level 1")
      Players[0]:teleport_to_level(1)
    end
  elseif Level.index == 1 then
    if level_ticks >= 8 * 30 and level_ticks % 60 == 0 then
      Game.save(); say("saved level=%d x=%.2f y=%.2f", Level.index, Players[0].x, Players[0].y)
    end
  end
end
