-- iOS diag: which physgun halo path runs, and with what colour
local last = 0
local function log(...) MsgN("[HALODIAG] ", ...) end
hook.Add("InitPostEntity", "ios_halo_diag", function()
    log("SetGModPhysgunHalos=", tostring(render.SetGModPhysgunHalos ~= nil),
        " RenderGModPhysgunHalo=", tostring(render.RenderGModPhysgunHalo ~= nil),
        " Supports=", tostring(render.SupportsGModPhysgunHaloOptimization and render.SupportsGModPhysgunHaloOptimization()))
    for _, n in ipairs({"physgun_halo", "gmod_physgun_fast_halo", "gmod_physgun_optimize_halo", "cl_weaponcolor"}) do
        local c = GetConVar(n); log(n, "=", c and c:GetString() or "nil")
    end
    local h = hook.GetTable().PreDrawHalos
    log("PreDrawHalos.AddPhysgunHalos src=", h and h.AddPhysgunHalos and debug.getinfo(h.AddPhysgunHalos, "S").source or "nil")
    if render.SetGModPhysgunHalos then
        local orig = render.SetGModPhysgunHalos
        render.SetGModPhysgunHalos = function(e)
            local ok = orig(e)
            if #e > 0 and CurTime() - last > 1 then
                last = CurTime()
                local c = e[1].color
                log("native n=", #e, " ret=", tostring(ok), " col=", c.r, ",", c.g, ",", c.b)
            end
            return ok
        end
    end
    local oldAdd = halo.Add
    local lastA = 0
    halo.Add = function(ents, col, bx, by, passes, add, ignz)
        if CurTime() - lastA > 1 then
            lastA = CurTime()
            log("halo.Add col=", col.r, ",", col.g, ",", col.b, " blur=", bx, " add=", tostring(add), " src=", debug.getinfo(2, "S").source)
        end
        return oldAdd(ents, col, bx, by, passes, add, ignz)
    end
    local p = LocalPlayer()
    if IsValid(p) then log("weaponcolor=", tostring(p:GetWeaponColor())) end
end)
