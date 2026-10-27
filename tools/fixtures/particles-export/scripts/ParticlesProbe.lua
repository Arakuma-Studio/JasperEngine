-- S20 AC16: what the exported game simulates and where its camera looks, logged once after a
-- second of play and every 600 frames (the smoke test's screenshot is a few seconds in).
local ParticlesProbe = {
    properties = {}
}

function ParticlesProbe:init()
    self.frames = 0
    RegisterEngineEvent(self, "onUpdate")
end

function ParticlesProbe:onUpdate()
    self.frames = self.frames + 1
    if self.frames ~= 60 and self.frames % 600 ~= 0 then
        return
    end
    local fire = ParticleSystem(self.scene, self.scene:findEntity("Fire"))
    local sparks = ParticleSystem(self.scene, self.scene:findEntity("Sparks"))
    Log.print(string.format("[smoke] frame %d: Fire %d at %.2f s (playing %s), Sparks %d at %.2f s (playing %s)",
        self.frames, fire.particleCount, fire.time, tostring(fire.isPlaying), sparks.particleCount, sparks.time, tostring(sparks.isPlaying)))
end

return ParticlesProbe
