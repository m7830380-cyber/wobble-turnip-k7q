-- iOS port diagnostic: which physgun halo path runs and what it costs
local stats = {native_ok = 0, native_fail = 0, stock = 0, stock_ms = 0, native_ms = 0}
local wrapped = false
local function wrap()
    if wrapped then return end
    if render.RenderGModPhysgunHalo then
        local orig = render.RenderGModPhysgunHalo
        render.RenderGModPhysgunHalo = function(...)
            local t = SysTime()
            local ok = orig(...)
            stats.native_ms = stats.native_ms + (SysTime() - t) * 1000
            if ok then stats.native_ok = stats.native_ok + 1 else stats.native_fail = stats.native_fail + 1 end
            return ok
        end
    end
    if halo and halo.Render then
        local r = halo.Render
        halo.Render = function(e)
            local t = SysTime()
            local a, b = r(e)
            stats.stock = stats.stock + 1
            stats.stock_ms = stats.stock_ms + (SysTime() - t) * 1000
            return a, b
        end
    end
    wrapped = true
    local sup = render.SupportsGModPhysgunHaloOptimization and render.SupportsGModPhysgunHaloOptimization()
    local opt = halo and halo._GModVRPhysgunHaloOptimization
    print(string.format("[haloprobe] installed: RenderGModPhysgunHalo=%s Supports=%s SetGModPhysgunHalos=%s optimization_wrapper=%s optimize=%s fast=%s",
        tostring(render.RenderGModPhysgunHalo ~= nil), tostring(sup), tostring(render.SetGModPhysgunHalos ~= nil),
        tostring(opt ~= nil and opt.wrapper ~= nil),
        GetConVar("gmod_physgun_optimize_halo") and GetConVar("gmod_physgun_optimize_halo"):GetString() or "nil",
        GetConVar("gmod_physgun_fast_halo") and GetConVar("gmod_physgun_fast_halo"):GetString() or "nil"))
end
hook.Add("InitPostEntity", "ios_halo_probe", function() timer.Simple(2, wrap) end)
timer.Create("ios_halo_probe", 2, 0, function()
    if not wrapped then return end
    if stats.stock + stats.native_ok + stats.native_fail == 0 then return end
    print(string.format("[haloprobe] 2s: halo.Render calls %d (%.1f ms CPU) native ok %d fail %d (%.1f ms)",
        stats.stock, stats.stock_ms, stats.native_ok, stats.native_fail, stats.native_ms))
    stats = {native_ok = 0, native_fail = 0, stock = 0, stock_ms = 0, native_ms = 0}
end)
