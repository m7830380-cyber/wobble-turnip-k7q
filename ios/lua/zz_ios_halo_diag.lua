-- iOS diag: why the native physgun halo path is not taken
local function log(...) MsgN("[HALODIAG] ", ...) end
local stat = {pre = 0, set = 0, setok = 0, setn = 0, rend = 0, rendok = 0}
hook.Add("PreRender", "ios_halo_diag_cnt", function() stat.pre = stat.pre + 1 end)
local function wrap()
    if render.SetGModPhysgunHalos and not render._iosSet then
        local o = render.SetGModPhysgunHalos; render._iosSet = o
        render.SetGModPhysgunHalos = function(e)
            local ok = o(e); stat.set = stat.set + 1; stat.setn = stat.setn + #e
            if ok then stat.setok = stat.setok + 1 end
            return ok
        end
    end
    if render.RenderGModPhysgunHalo and not render._iosRend then
        local o = render.RenderGModPhysgunHalo; render._iosRend = o
        render.RenderGModPhysgunHalo = function(...)
            local ok = o(...); stat.rend = stat.rend + 1
            if ok then stat.rendok = stat.rendok + 1 end
            return ok
        end
    end
end
wrap()
hook.Add("InitPostEntity", "ios_halo_diag", function()
    wrap()
    log("cvars optimize=", GetConVar("gmod_physgun_optimize_halo"):GetString(), " fast=", GetConVar("gmod_physgun_fast_halo"):GetString())
    local h = hook.GetTable().PreRender or {}
    for k in pairs(h) do log("PreRender hook ", tostring(k)) end
    local o = halo._GModVRPhysgunHaloOptimization
    log("optimization table=", tostring(o ~= nil), " halo.Render is wrapper=", tostring(o and halo.Render == o.wrapper))
end)
timer.Create("ios_halo_diag_t", 2, 0, function()
    log(string.format("2s: PreRender=%d SetCalls=%d SetOK=%d SetEntries=%d RenderCalls=%d RenderOK=%d frame=%d",
        stat.pre, stat.set, stat.setok, stat.setn, stat.rend, stat.rendok, FrameNumber()))
    if render.GetGModPhysgunHaloOptimizationStats then
        local ok, a, b, c, d = pcall(render.GetGModPhysgunHaloOptimizationStats)
        if type(a) == "table" then local t = {} for k, v in pairs(a) do t[#t+1] = tostring(k) .. "=" .. tostring(v) end log("stats ", table.concat(t, " "))
        else log("stats ", tostring(ok), " ", tostring(a), " ", tostring(b), " ", tostring(c), " ", tostring(d)) end
    end
    local g = GetConVar("gmod_physgun_glow"); log("gmod_physgun_glow=", g and g:GetString() or "nil")
    for k in pairs(stat) do stat[k] = 0 end
end)
local oldAdd = halo.Add
local lastA = 0
halo.Add = function(ents, col, bx, by, passes, add, ignz)
    if CurTime() - lastA > 2 then lastA = CurTime(); log("FALLBACK halo.Add col=", math.floor(col.r), ",", col.g, ",", col.b) end
    return oldAdd(ents, col, bx, by, passes, add, ignz)
end
