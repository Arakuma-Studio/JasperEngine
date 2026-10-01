-- On the root of prefabs/Bullet.prefab, a prefab only scripts reference
local Bullet = { properties = {} }

PRT = PRT or { inits = 0, destroyed = 0, bulletInits = 0, failures = 0, enemies = {} }

function Bullet:init()
    PRT.bulletInits = PRT.bulletInits + 1
end

return Bullet
