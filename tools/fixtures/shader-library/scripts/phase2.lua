-- Shader library phase 2: the outline and stencil shaders (library:outline,
-- library:outline_stencil, library:silhouette, library:stencil_mask, library:sprite_mask)
-- and every material preset of the editor (editor/material/MaterialPresets.cpp), whose
-- settings PRESETS below reproduces field for field (the presets are editor code).
--
--   row 1, outline hull (an extra pass, After on opaque objects):
--     1. a hard-edged cube, 3 px (screen width): closed at its corners (smoothed normals)
--     2. the same cube, world width 0.06
--     3. a skinned, morphed hard-edged column bending (screen width)
--     4. an instanced batch of four cubes (world width): every instance outlined
--     5. a sphere, world width growing with distance (DISTANCE_SCALE), capped
--     6. a cube whose outline is tinted by a checker texture (TEXTURE)
--     7. a vertex-coloured bumpy box, width times its vertex colour red (VERTEX_COLOR_WIDTH)
--     8. a blended glass box: the Outline (hull) preset gives it the stencil outline instead
--        (the material marks stencil bit 1, an After outline_stencil pass, Visible only, draws
--        the hull's look outside the mark: a dark blue ring, the glass not filled)
--   row 2, stencil (each object behind a low grey wall that hides its lower half):
--     1. Outline (stencil) preset, screen width: the orange outline runs over the wall
--        around the hidden half too, never over the cube itself
--     2. the same with VISIBILITY_VISIBLE_ONLY: outlined above the wall only
--     3. Outline (stencil), world width, on a sphere
--     4. See-through silhouette preset, flat: blue over the wall where the sphere hides
--     5. the silhouette in FRESNEL shading on a torus (overlapping hidden parts blend once)
--     6. draw-once transparency: one mesh of three overlapping squares stacking (left)
--        beside the same mesh with the Draw-once transparency preset (right: one flat tint)
--     7. Stencil mask preset (a square, invisible) in front of a sphere with Visible inside
--        mask: a red sphere clipped to the square
--     8. a leaf-shaped Stencil mask (ALPHA_CLIP) in front of a green sphere with Visible
--        outside mask: a leaf-shaped hole
--   row 3:
--     1. Sprite mask preset on a character sprite; a checker sprite with Visible inside
--        mask behind it: a checkered character
--     2. the same with Visible outside mask: a checker with a character-shaped hole
--     3. an orthographic render-to-texture camera's view of a screen-width outline (3 px of
--        its 256 px texture), a world-width one and a through-walls stencil outline
--     4, 5. screen-width 4 px magenta outlines on spheres at three distances (the cell, 4
--        and 6.5 units behind it), 4 px at each; then the same with world width 0.05
--        (thinner with distance)
--
-- SL_GROUPS=phase2 frames the group; PH2_VIEW=<view> a close-up: outline_a, outline_b
-- (row 1, left and right half), stencil_a, stencil_b (row 2), row3 (the sprite masks and
-- the orthographic screen), distance (the distance spheres). PH2_ORTHO=1 makes the main
-- camera orthographic (screen widths stay exact).
--   SMOKE_GAME_ENV="SL_GROUPS=phase2" SMOKE_GAME_RUNS="outline_a PH2_VIEW=outline_a; outline_b PH2_VIEW=outline_b;
--       stencil_a PH2_VIEW=stencil_a; stencil_b PH2_VIEW=stencil_b; row3 PH2_VIEW=row3;
--       distance PH2_VIEW=distance; ortho PH2_ORTHO=1" tools/smoke-test.sh tools/fixtures/shader-library
local Phase2 = {}

local OUTLINE = "library:outline"
local OUTLINE_STENCIL = "library:outline_stencil"
local SILHOUETTE = "library:silhouette"
local STENCIL_MASK = "library:stencil_mask"
local SPRITE_MASK = "library:sprite_mask"

local function stencil(compare, ref, readMask, writeMask, pass, depthFail)
    local s = MaterialStencil()
    s.compare = compare
    s.ref = ref
    s.readMask = readMask
    s.writeMask = writeMask
    s.pass = pass or MaterialStencilOp.KEEP
    s.fail = MaterialStencilOp.KEEP
    s.depthFail = depthFail or MaterialStencilOp.KEEP
    return s
end

-- editor/material/MaterialPresets.cpp in Lua: preset(material, blended) edits a material
-- copy as the editor's preset does (blended: what isMaterialBlended answers for it), for
-- the materials this group starts from (no stencil outline mark or pass to re-apply over;
-- the readers here have no extra pass to mask)
local PRESETS = {}

local OUTLINE_MARK = 1      -- stencil bit of the outline mark
local DRAW_ONCE_BITS = 12   -- 0x0C
local MASK_BITS = 240       -- 0xF0

local function setExtraPass(material, shader, order)
    if material.extraPass.shader ~= shader then
        material.extraPass = MaterialPass()
        material.extraPass.shader = shader
    end
    material.extraPass.order = order
end

-- (the editor's preset also clears the old shader's features; these materials have none)
local function makeMask(material, shader)
    material.shader = shader
    material.lighting = MaterialLighting.AUTO
    material.blendMode = MaterialBlendMode.AUTO
    material.depthWrite = MaterialDepthWrite.AUTO
    material.colorWrite = MaterialColorWrite.AUTO
    material.stencil = MaterialStencil()
    material.sortPriority = math.min(material.sortPriority, -1)
end

-- the material's test plus the outline mark (withVisibleOutlineMark): draw-once and mask
-- readers keep their test and also write bit 1; anything else becomes the plain mark
local function withVisibleOutlineMark(s)
    if s.compare == MaterialStencilCompare.NOT_EQUAL and s.readMask == DRAW_ONCE_BITS then
        return stencil(s.compare, s.ref | OUTLINE_MARK, DRAW_ONCE_BITS, DRAW_ONCE_BITS | OUTLINE_MARK, MaterialStencilOp.REPLACE)
    end
    if (s.compare == MaterialStencilCompare.EQUAL or s.compare == MaterialStencilCompare.NOT_EQUAL) and s.readMask == MASK_BITS then
        return stencil(s.compare, s.ref | OUTLINE_MARK, MASK_BITS, OUTLINE_MARK, MaterialStencilOp.REPLACE)
    end
    return stencil(MaterialStencilCompare.ALWAYS, OUTLINE_MARK, 255, OUTLINE_MARK, MaterialStencilOp.REPLACE, MaterialStencilOp.REPLACE)
end

-- Outline (hull) on a blended material (makeVisibleOutline): a hull would fill the
-- see-through shape, so an After outline_stencil pass, Visible only, starting from the
-- hull's look (its default colour and widths), outside the material's stencil mark
local function makeVisibleOutline(material)
    if material.extraPass.shader ~= OUTLINE_STENCIL then
        local hull = MaterialPass()
        hull.shader = OUTLINE
        local look = {
            outlineColor = hull:getShaderParam("outlineColor"),
            screenWidth = hull:getShaderParam("screenWidth").x,
            worldWidth = hull:getShaderParam("worldWidth").x,
        }
        setExtraPass(material, OUTLINE_STENCIL, MaterialPassOrder.AFTER)
        for member, value in pairs(look) do
            material.extraPass:setShaderParam(member, value)
        end
    end
    material.extraPass:setShaderFeature("VISIBILITY_VISIBLE_ONLY", true)
    material.extraPass.order = MaterialPassOrder.AFTER
    material.stencil = withVisibleOutlineMark(material.stencil)
end

function PRESETS.outline_hull(material, blended)
    if blended then
        makeVisibleOutline(material)
    else
        setExtraPass(material, OUTLINE, MaterialPassOrder.AFTER)
    end
end

function PRESETS.outline_stencil(material)
    material.stencil = stencil(MaterialStencilCompare.ALWAYS, 1, 255, 1, MaterialStencilOp.REPLACE, MaterialStencilOp.REPLACE)
    setExtraPass(material, OUTLINE_STENCIL, MaterialPassOrder.AFTER)
    material.sortPriority = math.max(material.sortPriority, 1)
end

function PRESETS.see_through_silhouette(material)
    setExtraPass(material, SILHOUETTE, MaterialPassOrder.BEFORE)
    material.sortPriority = math.max(material.sortPriority, 1)
end

function PRESETS.draw_once_transparency(material, blended)
    material.stencil = stencil(MaterialStencilCompare.NOT_EQUAL, 4, 12, 12, MaterialStencilOp.REPLACE)
    if not blended then
        material.blendMode = MaterialBlendMode.ALPHA
    end
    -- now blended: a hull would fill it
    if material.extraPass.shader == OUTLINE then
        makeVisibleOutline(material)
    end
end

function PRESETS.stencil_mask(material)
    makeMask(material, STENCIL_MASK)
end

function PRESETS.visible_inside_mask(material)
    material.stencil = stencil(MaterialStencilCompare.EQUAL, 16, 240, 240)
end

function PRESETS.visible_outside_mask(material)
    material.stencil = stencil(MaterialStencilCompare.NOT_EQUAL, 16, 240, 240)
end

function PRESETS.sprite_mask(material)
    makeMask(material, SPRITE_MASK)
end

-- every submesh's material, edited by configure(material, pass), set back
local function configure(ctx, mesh, configure)
    local count = mesh:getNumSubmeshes()
    if count == 0 then
        ctx:fail("a mesh has no submesh to configure")
    end
    for i = 0, count - 1 do
        local material = mesh:getMaterial(i)
        configure(material, material.extraPass)
        mesh:setMaterial(i, material)
    end
end

-- library:unlit in a flat colour (linear)
local function unlit(material, r, g, b, a)
    material.shader = "library:unlit"
    material:setShaderParam("baseColor", Vector4(r, g, b, a or 1.0))
end

local function box(ctx, size, r, g, b)
    local p = Primitive(ctx.scene)
    p:createBox(size, size, size)
    configure(ctx, p, function(m) unlit(m, r, g, b) end)
    return p
end

-- an outline_hull preset with its pass parameters (screen width unless world is given)
local function outline(ctx, mesh, params, blended)
    configure(ctx, mesh, function(m, pass)
        PRESETS.outline_hull(m, blended)
        for name, value in pairs(params or {}) do
            if name == "features" then
                for _, define in ipairs(value) do
                    pass:setShaderFeature(define, true)
                end
            elseif name == "texture" then
                pass:setShaderTexture("u_outlineTexture", value)
            else
                pass:setShaderParam(name, value)
            end
        end
    end)
end

-- a low wall in front of a cell (between it and the camera), hiding the lower half
local function wall(ctx, position)
    local w = Primitive(ctx.scene)
    w:createBox(1.7, 0.75, 0.08)
    w.position = Vector3(position.x, 0.375, position.z + 0.75)
    configure(ctx, w, function(m) unlit(m, 0.32, 0.34, 0.38) end)
    table.insert(ctx.objects, w)
    return w
end

local function buildOutlineRow(ctx)
    local scene = ctx.scene
    -- 1, 2: screen and world width on a hard-edged cube
    local cube = box(ctx, 0.9, 0.85, 0.3, 0.25)
    ctx:place(cube, 0.7)
    cube:setRotation(25, 40, 0)
    outline(ctx, cube)
    local worldCube = box(ctx, 0.9, 0.85, 0.3, 0.25)
    ctx:place(worldCube, 0.7)
    worldCube:setRotation(25, 40, 0)
    outline(ctx, worldCube, { features = { "WIDTH_MODE_WORLD" }, worldWidth = 0.06 })

    -- 3: skinned and morphed
    local column = Model(scene)
    if column:loadModel("models/skinned_column.gltf") then
        ctx:place(column, 0.0)
        column:setMorphWeight(0, 0.6)
        outline(ctx, column, { outlineColor = Vector4(0.05, 0.02, 0.15, 1.0) })
        ctx.tip = column:getBone("Tip")
        if not ctx.tip then
            ctx:fail("the column has no Tip bone")
        end
    else
        ctx:fail("models/skinned_column.gltf did not load")
    end

    -- 4: instanced
    local batch = Primitive(scene)
    batch:createBox(0.45, 0.45, 0.45)
    ctx:place(batch, 0.0)
    configure(ctx, batch, function(m) unlit(m, 0.9, 0.9, 0.9) end)
    batch:createInstancedMesh()
    local colours = { Vector4(1, 0.3, 0.3, 1), Vector4(0.3, 1, 0.4, 1), Vector4(0.35, 0.5, 1, 1), Vector4(1, 1, 0.3, 1) }
    for i = 1, 4 do
        batch:addInstance(Vector3((i % 2) * 0.6 - 0.3, 0.3 + (i - 1) * 0.5, 0.0), Quaternion(), Vector3(1, 1, 1), colours[i])
    end
    outline(ctx, batch, { features = { "WIDTH_MODE_WORLD" }, worldWidth = 0.035 })

    -- 5: world width growing with distance, capped
    local sphere = Primitive(scene)
    sphere:createSphere(0.6, 32, 16)
    ctx:place(sphere, 0.7)
    configure(ctx, sphere, function(m) unlit(m, 0.95, 0.95, 0.9) end)
    outline(ctx, sphere, { features = { "WIDTH_MODE_WORLD", "DISTANCE_SCALE" }, worldWidth = 0.02,
                           referenceDistance = 4.0, maxWidth = 0.08, outlineColor = Vector4(0.0, 0.2, 0.05, 1.0) })

    -- 6: a textured outline
    local textured = box(ctx, 0.9, 0.95, 0.95, 0.95)
    ctx:place(textured, 0.7)
    textured:setRotation(25, 40, 0)
    outline(ctx, textured, { features = { "WIDTH_MODE_WORLD", "TEXTURE" }, worldWidth = 0.09,
                             texture = "textures/checker.png", textureTiling = Vector4(4, 4, 0, 0),
                             outlineColor = Vector4(1.0, 0.25, 0.05, 1.0) })

    -- 7: width from the vertex colour's red
    local painted = Model(scene)
    if painted:loadModel("models/bumpy_box.gltf") then
        ctx:place(painted, 0.45)
        painted:setScale(0.5)
        configure(ctx, painted, function(m)
            unlit(m, 1, 1, 1)
            m:setShaderFeature("VERTEX_COLOR", true)
        end)
        outline(ctx, painted, { features = { "WIDTH_MODE_WORLD", "VERTEX_COLOR_WIDTH" }, worldWidth = 0.12 })
    else
        ctx:fail("models/bumpy_box.gltf did not load")
    end

    -- 8: blended: the preset gives it the stencil outline, Visible only
    local glass = Primitive(scene)
    glass:createBox(0.9, 0.9, 0.9)
    ctx:place(glass, 0.7)
    glass:setRotation(25, 40, 0)
    configure(ctx, glass, function(m)
        unlit(m, 0.2, 0.45, 1.0, 0.45)
        m.blendMode = MaterialBlendMode.ALPHA
    end)
    outline(ctx, glass, { outlineColor = Vector4(0.0, 0.05, 0.3, 1.0) }, true)
    configure(ctx, glass, function(m, pass)
        if pass.shader ~= OUTLINE_STENCIL or not pass:isShaderFeatureEnabled("VISIBILITY_VISIBLE_ONLY") or
           pass.order ~= MaterialPassOrder.AFTER or m.stencil.compare ~= MaterialStencilCompare.ALWAYS or
           m.stencil.writeMask ~= OUTLINE_MARK then
            ctx:fail("the blended glass's outline is not the visible-only stencil outline")
        end
    end)

    ctx.spinning = { cube, worldCube, textured }
end

local function buildStencilRow(ctx)
    local scene = ctx.scene
    -- 1-3: outline (stencil): through walls (screen, world) and visible only
    local function stencilOutline(mesh, features, params)
        configure(ctx, mesh, function(m, pass)
            PRESETS.outline_stencil(m)
            for _, define in ipairs(features) do
                pass:setShaderFeature(define, true)
            end
            for name, value in pairs(params or {}) do
                pass:setShaderParam(name, value)
            end
        end)
    end
    local through = box(ctx, 0.9, 0.95, 0.85, 0.3)
    wall(ctx, ctx:place(through, 0.6))
    through:setRotation(20, 35, 0)
    stencilOutline(through, {})
    local visible = box(ctx, 0.9, 0.95, 0.85, 0.3)
    wall(ctx, ctx:place(visible, 0.6))
    visible:setRotation(20, 35, 0)
    stencilOutline(visible, { "VISIBILITY_VISIBLE_ONLY" })
    local worldSphere = Primitive(scene)
    worldSphere:createSphere(0.55, 32, 16)
    wall(ctx, ctx:place(worldSphere, 0.6))
    configure(ctx, worldSphere, function(m) unlit(m, 0.95, 0.85, 0.3) end)
    stencilOutline(worldSphere, { "WIDTH_MODE_WORLD" }, { worldWidth = 0.05, outlineColor = Vector4(0.1, 0.9, 1.0, 1.0) })

    -- 4, 5: see-through silhouette, flat and fresnel
    local hidden = Primitive(scene)
    hidden:createSphere(0.55, 32, 16)
    wall(ctx, ctx:place(hidden, 0.55))
    configure(ctx, hidden, function(m)
        unlit(m, 0.9, 0.4, 0.2)
        PRESETS.see_through_silhouette(m)
    end)
    local torus = Primitive(scene)
    torus:createTorus(0.42, 0.16)
    wall(ctx, ctx:place(torus, 0.6))
    torus:setRotation(70, 0, 0)
    configure(ctx, torus, function(m, pass)
        unlit(m, 0.9, 0.4, 0.2)
        PRESETS.see_through_silhouette(m)
        pass:setShaderFeature("SILHOUETTE_MODE_FRESNEL", true)
        pass:setShaderParam("color", Vector4(0.2, 1.0, 0.4, 0.9))
    end)

    -- 6: draw-once transparency beside the same mesh stacking its overlaps
    local position
    for side = 0, 1 do
        local overlap = Model(scene)
        if not overlap:loadModel("models/overlap_quads.gltf") then
            ctx:fail("models/overlap_quads.gltf did not load")
            break
        end
        if side == 0 then
            position = ctx:place(overlap, 0.6)
            overlap.position = position + Vector3(-0.42, 0.0, 0.0)
        else
            overlap.position = position + Vector3(0.42, 0.0, 0.0)
            table.insert(ctx.objects, overlap)
        end
        overlap:setScale(0.55)
        overlap.castShadows = false
        configure(ctx, overlap, function(m)
            unlit(m, 0.25, 0.65, 1.0, 0.45)
            m.blendMode = MaterialBlendMode.ALPHA
            if side == 1 then
                PRESETS.draw_once_transparency(m, true)
            end
        end)
    end

    -- 7: visible inside a square mask; 8: outside a leaf-shaped one
    local function maskCase(r, g, b, reader, configureMask)
        local sphere = Primitive(scene)
        sphere:createSphere(0.6, 32, 16)
        local cell = ctx:place(sphere, 0.65)
        configure(ctx, sphere, function(m)
            unlit(m, r, g, b)
            PRESETS[reader](m)
        end)
        local mask = Primitive(scene)
        mask:createBox(0.9, 0.9, 0.02)
        mask.position = cell + Vector3(0.0, 0.05, 0.7)
        configure(ctx, mask, function(m)
            unlit(m, 1.0, 0.0, 1.0) -- magenta if the preset did not take the colour writes off
            PRESETS.stencil_mask(m)
            if configureMask then
                configureMask(m)
            end
        end)
        table.insert(ctx.objects, mask)
    end
    maskCase(0.9, 0.12, 0.1, "visible_inside_mask")
    maskCase(0.15, 0.75, 0.25, "visible_outside_mask", function(m)
        m:setShaderFeature("ALPHA_CLIP", true)
        m:setShaderTexture("u_maskTexture", "textures/leaf.png")
    end)
end

local function sprite(ctx, texture, size)
    local s = Sprite(ctx.scene)
    s:setTexture(texture)
    s:setSize(64, 64)
    s:setScale(size / 64)
    s.pivotPreset = PivotPreset.CENTER
    s.receiveLights = false
    s.castShadows = false
    return s
end

local function buildSpriteCases(ctx)
    for _, reader in ipairs({ "visible_inside_mask", "visible_outside_mask" }) do
        local masked = sprite(ctx, "textures/checker.png", 1.5)
        local cell = ctx:place(masked, 0.9)
        configure(ctx, masked, function(m)
            m.shader = "library:sprite"
            PRESETS[reader](m)
        end)
        local mask = sprite(ctx, "textures/character.png", 1.5)
        mask.position = cell + Vector3(0.0, 0.0, 0.05)
        configure(ctx, mask, PRESETS.sprite_mask)
        table.insert(ctx.objects, mask)
    end
end

-- what an orthographic render-to-texture camera sees, far below, on a screen
local function buildOrtho(ctx)
    local scene = ctx.scene
    local origin = Vector3(0, -120, 0)
    local backdrop = Primitive(scene)
    backdrop:createBox(8, 4, 0.1)
    backdrop.position = origin + Vector3(0, 0, -1.5)
    configure(ctx, backdrop, function(m) unlit(m, 0.55, 0.6, 0.7) end)
    table.insert(ctx.objects, backdrop)

    local screenCube = box(ctx, 1.1, 0.95, 0.75, 0.2)
    screenCube.position = origin + Vector3(-2.2, 0, 0)
    screenCube:setRotation(25, 35, 0)
    outline(ctx, screenCube)
    local worldCube = box(ctx, 1.1, 0.95, 0.75, 0.2)
    worldCube.position = origin + Vector3(0, 0, 0)
    worldCube:setRotation(25, 35, 0)
    outline(ctx, worldCube, { features = { "WIDTH_MODE_WORLD" }, worldWidth = 0.08 })
    local throughCube = box(ctx, 1.1, 0.4, 0.8, 0.3)
    throughCube.position = origin + Vector3(2.2, 0, 0)
    throughCube:setRotation(25, 35, 0)
    configure(ctx, throughCube, function(m) PRESETS.outline_stencil(m) end)
    local front = Primitive(scene)
    front:createBox(1.6, 0.7, 0.1)
    front.position = origin + Vector3(2.2, -0.4, 1.2)
    configure(ctx, front, function(m) unlit(m, 0.3, 0.3, 0.35) end)
    for _, o in ipairs({ screenCube, worldCube, throughCube, front }) do
        table.insert(ctx.objects, o)
    end

    local camera = Camera(scene)
    camera:setOrtho(-4, 4, -2, 2, 0.1, 50)
    camera.position = origin + Vector3(0, 0, 10)
    camera.target = origin
    camera.renderToTexture = true
    camera:setFramebufferSize(512, 256)
    table.insert(ctx.objects, camera)

    local screen = Primitive(scene)
    screen:createBox(1.8, 0.9, 0.04)
    ctx:place(screen, 0.8)
    screen.receiveLights = false
    screen.castShadows = false
    screen:setTexture(camera:getFramebuffer())
    return screen
end

-- three spheres, the cell's and two behind it at increasing distances, outlined by the
-- pass params (a sphere's smoothed normal is radial: the band at its rim is the full width)
local function buildDistance(ctx, params)
    local cell
    for i, distance in ipairs({ 0.0, 4.0, 6.5 }) do
        local sphere = Primitive(ctx.scene)
        sphere:createSphere(0.35, 32, 16)
        configure(ctx, sphere, function(m) unlit(m, 0.9, 0.9, 0.9) end)
        if i == 1 then
            cell = ctx:place(sphere, 0.5)
        else
            sphere.position = cell + Vector3(0.45 * (i - 1), 0.0, -distance)
            table.insert(ctx.objects, sphere)
        end
        outline(ctx, sphere, params)
    end
    return cell
end

-- PH2_VIEW cameras: position and target, from the group's first row at x 0 (rows are
-- 2.4 apart, cells 1.9; an "a" view frames the left four cells of a row, "b" the right four)
local VIEWS = {
    outline_a = { Vector3(-3.8, 2.2, 6.5), Vector3(-3.8, 0.6, 0.0) },
    outline_b = { Vector3(3.8, 2.2, 6.5), Vector3(3.8, 0.6, 0.0) },
    -- the rows in front of the framed one are hidden (hide: the rows to hide)
    stencil_a = { Vector3(-3.8, 2.0, 4.1), Vector3(-3.8, 0.5, -2.4), hide = { 1 } },
    stencil_b = { Vector3(3.8, 2.0, 4.1), Vector3(3.8, 0.5, -2.4), hide = { 1 } },
    row3 = { Vector3(-4.75, 2.2, 0.5), Vector3(-4.75, 0.8, -4.8), hide = { 1, 2 } },
    distance = { Vector3(0.3, 2.6, -1.3), Vector3(0.5, 0.4, -8.8), hide = { 1, 2 } },
}

function Phase2.build(ctx)
    -- the objects of each row (ctx.objects grows in build order)
    local rows = {}
    buildOutlineRow(ctx)
    rows[1] = { 1, #ctx.objects }
    buildStencilRow(ctx)
    rows[2] = { rows[1][2] + 1, #ctx.objects }
    buildSpriteCases(ctx)
    ctx.orthoScreen = buildOrtho(ctx)
    local magenta = Vector4(1.0, 0.0, 1.0, 1.0)
    buildDistance(ctx, { screenWidth = 4.0, outlineColor = magenta })
    buildDistance(ctx, { features = { "WIDTH_MODE_WORLD" }, worldWidth = 0.05, outlineColor = magenta })

    local view = os.getenv("PH2_VIEW")
    if ctx.solo or (view and view ~= "") then
        local origin = Vector3(0, 0, ctx.originZ)
        local v = VIEWS[view or ""] or { Vector3(0, 8.0, 7.0), Vector3(0, 0.0, -4.5) }
        if view and view ~= "" and not VIEWS[view] then
            ctx:fail("PH2_VIEW names unknown view %s", view)
        end
        ctx.camera.position = origin + v[1]
        ctx.camera.target = origin + v[2]
        for _, row in ipairs(v.hide or {}) do
            for i = rows[row][1], rows[row][2] do
                ctx.objects[i].visible = false
            end
        end
    end
    if os.getenv("PH2_ORTHO") == "1" then
        -- an orthographic main camera over the same view
        ctx.camera:setOrtho(-8.5, 8.5, -4.8, 4.8, 0.1, 100.0)
        ctx:log("orthographic main camera")
    end
end

function Phase2.update(ctx, dt)
    ctx.time = (ctx.time or 0) + dt
    if ctx.tip then
        ctx.tip:setRotation(0, 0, math.sin(ctx.time * 1.5) * 35)
    end
    for _, mesh in ipairs(ctx.spinning or {}) do
        mesh:setRotation(25, 40 + ctx.time * 20, 0)
    end
end

return Phase2
