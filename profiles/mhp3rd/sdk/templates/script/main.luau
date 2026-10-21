-- The SDK template's example as a script: while its setting is on, it logs
-- each armor model the played hunter's parts use, once per model, shows the
-- latest model of each part on the mod's page, and notes quests starting and
-- ending. Nothing to compile; put this folder in the mods folder as it is.

-- The game's armor model lookup, by its name in sdk/symbols.txt
-- (docs/LAYERED_ARMOR.md): a1 is a hunter record's index, 0 for the hunter
-- played here, a2 the part (0 to 6), and v0 the model it returns.
local MODEL_OF_PART = "armor.model_of_part"
local PARTS = { "Chest", "Arms", "Waist", "Legs", "Head", "Hair", "Face" }

local logging = yakumo.setting_switch("log", "Log Armor Models", true)
local record, part = 0, 0
local seen = {}
local latest = {}

-- Before the lookup runs: what it was asked for. Its arguments are gone by
-- the time it returns.
yakumo.hook(MODEL_OF_PART, "before", function(cpu)
    record, part = cpu.a1, cpu.a2
end)

-- After it returns: the model, in v0.
yakumo.hook(MODEL_OF_PART, "after", function(cpu)
    if record ~= 0 then
        return
    end
    latest[part] = cpu.v0
    local key = part * 65536 + cpu.v0 % 65536
    if logging and not seen[key] then
        seen[key] = true
        yakumo.log("part " .. part .. " uses model " .. cpu.v0)
    end
end)

-- The player may change the setting on the mod's page at any time.
yakumo.on("frame", function()
    logging = yakumo.setting("log")
end)

yakumo.on("quest_started", function(quest)
    yakumo.log("quest " .. quest .. " started")
end)
yakumo.on("quest_ended", function(quest)
    yakumo.log("quest " .. quest .. " ended")
end)

-- On the mod's page, under its settings.
yakumo.panel("Models Now", function(ui)
    for index, name in ipairs(PARTS) do
        local model = latest[index - 1]
        ui.text(name .. ": " .. (model and tostring(model) or "not seen yet"))
    end
end)

yakumo.log(logging and "logging armor models" or "armor model logging is off")
