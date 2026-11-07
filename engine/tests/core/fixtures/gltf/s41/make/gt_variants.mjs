// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// gltf-transform variants of a source model: Draco, gltf-transform's own meshopt and quantize.
// usage: node gt_variants.mjs <node_modules dir> <source.glb> <out dir>
import { createRequire } from 'module';
import path from 'path';
const [modules, source, out] = process.argv.slice(2);
const require = createRequire(path.join(modules, 'x.js'));
const { NodeIO } = require('@gltf-transform/core');
const { ALL_EXTENSIONS } = require('@gltf-transform/extensions');
const { draco, meshopt, quantize, reorder } = require('@gltf-transform/functions');
const draco3d = require('draco3dgltf');
const { MeshoptEncoder, MeshoptDecoder } = require('meshoptimizer');

await MeshoptEncoder.ready;
await MeshoptDecoder.ready;
const io = new NodeIO().registerExtensions(ALL_EXTENSIONS).registerDependencies({
    'draco3d.decoder': await draco3d.createDecoderModule(),
    'draco3d.encoder': await draco3d.createEncoderModule(),
    'meshopt.decoder': MeshoptDecoder,
    'meshopt.encoder': MeshoptEncoder,
});

async function variant(name, transforms) {
    const document = await io.read(source);
    await document.transform(...transforms);
    await io.write(path.join(out, name), document);
    console.log(name);
}

await variant('draco.glb', [draco({ method: 'edgebreaker' })]);
await variant('draco_sequential.glb', [draco({ method: 'sequential', quantizePosition: 16 })]);
await variant('gt_meshopt.glb', [reorder({ encoder: MeshoptEncoder }), quantize(), meshopt({ encoder: MeshoptEncoder, level: 'medium' })]);
