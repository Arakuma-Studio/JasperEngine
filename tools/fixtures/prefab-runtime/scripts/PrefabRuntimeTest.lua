-- Runtime prefab API test (GameObject.instantiate / destroy, Prefab script properties).
-- Every check prints "[ERROR] [PRT] ..." when it fails, which fails tools/smoke-test.sh;
-- the end prints "[PRT] PASS". Runs the same in the editor's Play and in the exported game.
--   tools/smoke-test.sh tools/fixtures/prefab-runtime
local PrefabRuntimeTest = {
    properties = {
        { name = "enemyPrefab", displayName = "Enemy Prefab", type = "Prefab" },
        { name = "spawnParent", displayName = "Spawn Parent", type = "GameObject" },
    }
}

local COUNT = 1000

local function log(fmt, ...)
    print("[PRT] " .. string.format(fmt, ...))
end

local function fail(fmt, ...)
    PRT.failures = PRT.failures + 1
    print("[ERROR] [PRT] " .. string.format(fmt, ...))
end

local function check(condition, fmt, ...)
    if not condition then
        fail(fmt, ...)
    end
    return condition
end

local function near(a, b)
    return math.abs(a - b) < 0.001
end

local function checkVector(v, x, y, z, what)
    check(v ~= nil and near(v.x, x) and near(v.y, y) and near(v.z, z),
        "%s is (%.3f, %.3f, %.3f), expected (%.3f, %.3f, %.3f)", what, v and v.x or 0, v and v.y or 0, v and v.z or 0, x, y, z)
end

function PrefabRuntimeTest:init()
    -- fresh counters (the Lua state outlives a Play in the editor)
    PRT = { inits = 0, destroyed = 0, bulletInits = 0, failures = 0, enemies = {} }
    RegisterEngineEvent(self, "onUpdate")
    self.frame = 0
    self.time = 0
    local scene = self.scene

    local camera = Camera(scene)
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0, 6, 25)
    camera.target = Vector3(0, 0, 0)
    scene:setCamera(camera)

    -- The prefab property (assigned by GUID) and the registry
    local prefab = self.enemyPrefab
    if not check(prefab ~= nil, "enemyPrefab property is nil") then return end
    check(prefab.valid, "enemyPrefab is not registered")
    check(prefab.path == "prefabs/Enemy.prefab", "enemyPrefab.path is %s", tostring(prefab.path))
    check(prefab.guid == "e0e0e0e0e0e0e0e0e0e0e0e0e0e00001", "enemyPrefab.guid is %s", tostring(prefab.guid))
    check(prefab.name == "Enemy", "enemyPrefab.name is %s", tostring(prefab.name))
    local names = PrefabManager.getPrefabNames()
    log("prefabs: %d (%s)", PrefabManager.prefabCount, table.concat(names, ", "))
    -- ids by path, the same in Play and in the export
    check(PrefabAsset.load("prefabs/Bullet").id == 1, "Bullet id is %d", PrefabAsset.load("prefabs/Bullet").id)
    check(PrefabAsset.load("prefabs/Enemy.prefab").id == 2, "Enemy id is %d", PrefabAsset.load("prefabs/Enemy.prefab").id)
    check(PrefabAsset.load("Wheel").id == 3, "Wheel id is %d", PrefabAsset.load("Wheel").id)
    check(PrefabAsset.load("e0e0e0e0e0e0e0e0e0e0e0e0e0e00001") == prefab, "load by GUID gives another prefab")

    -- At a world position and rotation
    local e1 = GameObject.instantiate(prefab, Vector3(5, 0, 0), Quaternion())
    if not check(e1 ~= nil, "instantiate(prefab, position, rotation) returned nil") then return end
    check(e1.name == "Enemy(Clone)", "the clone is named %s", e1.name)
    checkVector(e1.position, 5, 0, 0, "e1 position")
    check(PRT.inits == 1, "Enemy init ran %d times after one instantiate", PRT.inits)
    check(PRT.enemies[e1.entity] ~= nil, "the clone's script did not start")
    check(scene:findEntity("Hub", e1.entity) ~= NULL_ENTITY, "the nested Wheel prefab has no Hub in the clone")
    check(scene:findEntity("Body", e1.entity) ~= NULL_ENTITY, "the clone has no Body")
    local e1Script = PRT.enemies[e1.entity]
    check(e1Script and near(e1Script.speed, 1.5), "the clone's speed is not the prefab's 1.5")

    -- Below a parent: local placement kept, or world placement with instantiateInWorldSpace
    local parent = self.spawnParent
    if not check(parent ~= nil, "spawnParent property is nil") then return end
    local e2 = GameObject.instantiate(prefab, parent)
    check(scene:isParentOf(parent.entity, e2.entity), "e2 is not below the parent")
    checkVector(e2.position, 1, 2, 3, "e2 local position")
    local e3 = GameObject.instantiate(prefab, parent, true)
    checkVector(e3.position, -9, 2, 3, "e3 local position (world space)")
    local e4 = GameObject.instantiate(prefab, Vector3(1, 1, 1), Quaternion(), parent)
    checkVector(e4.position, -9, 1, 1, "e4 local position")
    self.e2, self.e3, self.e4 = e2, e3, e4

    -- By path and by id
    local byPath = GameObject.instantiate("prefabs/Bullet")
    check(byPath ~= nil and byPath.name == "Bullet(Clone)", "instantiate by path failed")
    check(PRT.bulletInits == 1, "Bullet init ran %d times", PRT.bulletInits)
    local byId = GameObject.instantiate(1)
    check(byId ~= nil and byId.name == "Bullet(Clone)", "instantiate by id failed")
    check(PrefabManager.findPrefab("prefabs/NoSuchPrefab") == 0, "a prefab that is not in the build was found")
    -- the prefab a prefab's script references is in the build too
    check(e1Script.bulletPrefab ~= nil and e1Script.bulletPrefab.valid, "Enemy's bulletPrefab is not in the build")

    -- A copy of a GameObject: current script values, references to the copies
    e1Script.speed = 7
    local copy = GameObject.instantiate(e1)
    check(copy.name == "Enemy(Clone)(Clone)", "the copy is named %s", copy.name)
    checkVector(copy.position, 5, 0, 0, "the copy's position")
    local copyScript = PRT.enemies[copy.entity]
    if check(copyScript ~= nil, "the copy's script did not start") then
        check(near(copyScript.speed, 7), "the copy's speed is %s, not the original's current 7", tostring(copyScript.speed))
        check(copyScript.body.entity ~= e1Script.body.entity, "the copy's body points at the original's")
    end
    -- a copy of a script gives the copy's script
    local scriptCopy = GameObject.instantiate(e1Script, Vector3(0, 0, -5), Quaternion())
    check(type(scriptCopy) == "table" and scriptCopy.__name == "Enemy" and scriptCopy.entity ~= e1.entity,
        "instantiate(script) did not give the copy's script")
    -- a copy below a parent, at its local placement
    local childCopy = GameObject.instantiate(e2, parent)
    checkVector(childCopy.position, 1, 2, 3, "a copy below the parent")
    check(PRT.inits == 7, "Enemy init ran %d times, expected 7", PRT.inits)

    -- Destroy: now, at the end of the frame, later
    local now = GameObject.instantiate(prefab)
    local nowEntity = now.entity
    GameObject.destroyImmediate(now)
    check(not scene:isEntityCreated(nowEntity), "destroyImmediate left the GameObject")
    check(PRT.destroyed == 1, "onDestroy ran %d times after destroyImmediate", PRT.destroyed)
    local atEnd = GameObject.instantiate(prefab)
    self.atEnd = atEnd.entity
    self.atEndBody = scene:findEntity("Body", atEnd.entity)
    GameObject.destroy(atEnd)
    check(scene:isEntityCreated(self.atEnd), "destroy removed the GameObject before the end of the frame")
    local later = GameObject.instantiate(prefab)
    self.later = later.entity
    GameObject.destroy(later, 0.5)

    -- 1000 instances
    local t0 = os.clock()
    self.many = {}
    for i = 1, COUNT do
        self.many[i] = GameObject.instantiate(prefab, Vector3(i % 40 - 20, 0, -math.floor(i / 40)), Quaternion())
    end
    local t1 = os.clock()
    log("instantiated %d prefabs (4 GameObjects, 1 Lua script each) in %.1f ms", COUNT, (t1 - t0) * 1000)
    check(PRT.inits == 10 + COUNT, "Enemy init ran %d times after %d instantiates", PRT.inits, COUNT)

    local t2 = os.clock()
    local copies = {}
    for i = 1, COUNT do
        copies[i] = GameObject.instantiate(self.many[i])
    end
    local t3 = os.clock()
    log("copied %d GameObjects in %.1f ms", COUNT, (t3 - t2) * 1000)
    local t4 = os.clock()
    for i = 1, COUNT do
        GameObject.destroyImmediate(copies[i])
    end
    log("destroyed %d GameObjects (destroyImmediate) in %.1f ms", COUNT, (os.clock() - t4) * 1000)
    check(PRT.destroyed == 1 + COUNT, "onDestroy ran %d times, expected %d", PRT.destroyed, 1 + COUNT)
    for i = 1, COUNT do
        GameObject.destroy(self.many[i])
    end
    self.firstMany = self.many[1].entity
    -- destroyImmediate, the copies, then at the end of the frame atEnd and the 1000
    self.expectedDestroyed = 1 + COUNT + 1 + COUNT
end

function PrefabRuntimeTest:onUpdate()
    self.frame = self.frame + 1
    self.time = self.time + Engine.deltatime
    local scene = self.scene

    if self.frame == 1 then
        -- destroy() waits for the end of the frame
        check(scene:isEntityCreated(self.atEnd), "destroy removed the GameObject before the end of the frame")

        -- a copy of the prefab instance placed in the scene (its scripts started after init)
        local placed = scene:findEntity("Enemy")
        if check(placed ~= NULL_ENTITY, "the scene's Enemy instance is missing") then
            check(PRT.enemies[placed] ~= nil, "the scene's Enemy instance script did not start")
            PRT.enemies[placed].speed = 3
            local instanceCopy = GameObject.instantiate(GameObject(scene, placed))
            check(instanceCopy.name == "Enemy(Clone)", "the instance's copy is named %s", instanceCopy.name)
            checkVector(instanceCopy.position, 1, 2, 3, "the instance's copy position")
            check(scene:findEntity("Hub", instanceCopy.entity) ~= NULL_ENTITY, "the instance's copy has no nested Hub")
            local copyScript = PRT.enemies[instanceCopy.entity]
            if check(copyScript ~= nil, "the instance's copy script did not start") then
                check(near(copyScript.speed, 3), "the instance's copy speed is %s, not 3", tostring(copyScript.speed))
                check(scene:isParentOf(instanceCopy.entity, copyScript.body.entity), "the instance's copy body is not its own")
            end
            self.instanceCopy = instanceCopy.entity
            GameObject.destroyImmediate(instanceCopy)
            self.expectedDestroyed = self.expectedDestroyed + 1
        end
        self.frameStart = os.clock()
    elseif self.frame == 2 then
        log("the frame destroying %d GameObjects (destroy) took %.1f ms", COUNT + 1, (os.clock() - self.frameStart) * 1000)
        check(not scene:isEntityCreated(self.firstMany), "destroy did not remove the 1000 at the end of the frame")
        check(not scene:isEntityCreated(self.atEnd), "destroy did not remove the GameObject at the end of the frame")
        check(not scene:isEntityCreated(self.atEndBody), "destroy left the GameObject's child")
        check(scene:isEntityCreated(self.later), "destroy(object, 0.5) removed it already")
        check(PRT.destroyed == self.expectedDestroyed, "onDestroy ran %d times, expected %d", PRT.destroyed, self.expectedDestroyed)
    elseif self.frame == 4 then
        -- world placements, once the Transforms were updated
        local p2 = self.e2:getWorldPosition()
        checkVector(p2, 11, 2, 3, "e2 world position")
        checkVector(self.e3:getWorldPosition(), 1, 2, 3, "e3 world position (instantiateInWorldSpace)")
        checkVector(self.e4:getWorldPosition(), 1, 1, 1, "e4 world position")
    end

    if self.time > 1.0 and not self.spawnerFrame then
        check(not scene:isEntityCreated(self.later), "destroy(object, 0.5) did not remove it after 1 s")
        check(PRT.destroyed == self.expectedDestroyed + 1, "onDestroy ran %d times, expected %d", PRT.destroyed, self.expectedDestroyed + 1)
        -- a GameObject of the scene, with the copies below it (Stop brings it back in the editor)
        self.spawnerFrame = self.frame
        self.spawner = self.spawnParent.entity
        self.e2Entity = self.e2.entity
        GameObject.destroy(self.spawnParent)
    elseif self.spawnerFrame and self.frame == self.spawnerFrame + 2 then
        check(not scene:isEntityCreated(self.spawner), "destroy did not remove the scene's Spawner")
        check(not scene:isEntityCreated(self.e2Entity), "destroy did not remove the Spawner's children")
        if PRT.failures == 0 then
            log("PASS")
        else
            fail("%d checks failed", PRT.failures)
        end
    end
end

return PrefabRuntimeTest
