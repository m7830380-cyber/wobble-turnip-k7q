-- iOS port: Viewfinder (ffcam) sees system.IsLinux() and keeps its VHS signal shaders off because on real ToGL a
-- shader the translator can't handle kills the game. On this port that error is non-fatal (libtogl patch in main.c),
-- so opt in once. The player can still turn it off in Viewfinder's settings; we only flip it the first time.
local MARK = "ios_ffcam_togl.txt"
local function try()
    if file.Exists(MARK, "DATA") then return true end
    local cv = GetConVar("ffcam_vhs_signal_togl")
    if not cv then return false end
    if not cv:GetBool() then RunConsoleCommand("ffcam_vhs_signal_togl", "1") end
    file.Write(MARK, "1")
    MsgN("[ios] Viewfinder: compiled VHS signal shaders enabled (ffcam_vhs_signal_togl 1)")
    return true
end
hook.Add("InitPostEntity", "ios_ffcam_togl", function()
    if try() then return end
    timer.Create("ios_ffcam_togl", 2, 10, function() if try() then timer.Remove("ios_ffcam_togl") end end)
end)
