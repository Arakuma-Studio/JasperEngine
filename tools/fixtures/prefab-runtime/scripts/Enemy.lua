-- On the root of prefabs/Enemy.prefab: checks that its references point inside its own copy
local Enemy = {
    properties = {
        { name = "speed", displayName = "Speed", type = "float", default = 1.0 },
        { name = "body", displayName = "Body", type = "GameObject" },
        { name = "bulletPrefab", displayName = "Bullet Prefab", type = "Prefab" },
    }
}

PRT = PRT or { inits = 0, destroyed = 0, bulletInits = 0, failures = 0, enemies = {} }

local function fail(fmt, ...)
    PRT.failures = PRT.failures + 1
    print("[ERROR] [PRT] " .. string.format(fmt, ...))
end

function Enemy:init()
    PRT.inits = PRT.inits + 1
    PRT.enemies[self.entity] = self

    if not self.body then
        fail("Enemy %d: body is nil", self.entity)
    elseif self.body.name ~= "Body" or not self.scene:isParentOf(self.entity, self.body.entity) then
        fail("Enemy %d: body points at %d (%s), not at its own Body", self.entity, self.body.entity, self.body.name)
    end
    if not self.bulletPrefab or not self.bulletPrefab.valid then
        fail("Enemy %d: bulletPrefab is not a registered prefab", self.entity)
    end
end

function Enemy:onDestroy()
    PRT.destroyed = PRT.destroyed + 1
    PRT.enemies[self.entity] = nil
end

return Enemy
