-- S07 AC10: bodies fall on a floor; each contact callback destroys the falling body with
-- GameObject.destroyImmediate and adds (or removes) a component on the script's own
-- GameObject. The destroys wait until the contact callbacks of the step are over
-- (StructuralDeferScope); run under ASan this must stay clean for 1000 frames.
local CollisionStress = {
    properties = {}
}

function CollisionStress:init()
    self.frames = 0
    self.hits = 0
    self.spawned = 0
    self.toggles = 0

    -- made here, owned by the scene (not by this Lua handle and its garbage collection)
    local floor = GameObject(self.scene)
    floor.entityOwned = false
    floor.name = "Floor"
    floor:setPosition(0, -250, 0)
    local floorBody = floor:getRigidbody2D()
    floorBody.type = BodyType.STATIC
    floorBody:createBoxCollider(2000, 40)
    floorBody.shapeContactEvents = true
    self.floorEntity = floor.entity
    self.floor = floor

    self.me = GameObject(self.scene, self.entity)

    RegisterEvent(self, self.scene:getPhysicsSystem().beginContact2D, "onBeginContact")
    RegisterEngineEvent(self, "onUpdate")
end

function CollisionStress:spawn()
    local box = GameObject(self.scene)
    box.entityOwned = false
    box.name = "Box"
    box:setPosition((self.spawned % 21) * 30 - 300, 200 + (self.spawned % 3) * 40, 0)
    local body = box:getRigidbody2D()
    body.type = BodyType.DYNAMIC
    body:createBoxCollider(20, 20)
    body.shapeContactEvents = true
    self.spawned = self.spawned + 1
end

function CollisionStress:onUpdate()
    self.frames = self.frames + 1
    if self.frames % 3 == 0 then
        self:spawn()
    end
end

function CollisionStress:onBeginContact(bodyA, shapeA, bodyB, shapeB)
    local other = bodyA.entity == self.floorEntity and bodyB or bodyA
    if other.entity == self.floorEntity then
        return
    end
    self.hits = self.hits + 1
    -- the other body goes (after the callbacks of this step)
    GameObject.destroyImmediate(GameObject(self.scene, other.entity))
    -- and this GameObject gains or loses a component, at once
    if self.toggles % 2 == 0 then
        self.me:getRigidbody2D()
    else
        self.me:removeRigidbody2D()
    end
    self.toggles = self.toggles + 1
end

return CollisionStress
