// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// jasperengine-editor generate-sprite-normals (sprite normals Phase 3, section 4): the
// generate_sprite_normals MCP tool for a shell or CI, through the same Phase 2 session (so a CLI
// write is the same pixels as an MCP or window write). Without --sprite: no engine, window or
// GPU (the session reads the assets directory from project.yaml then). With --sprite: one
// headless engine run (place-physics / bake-vat pattern) holds the whole command: the project
// loaded, the scene opened, one Spec per distinct colour texture of the GameObject (a Tilemap: per
// tileset) naming it (sceneId, entity, submesh), so frames auto are the Sprite's and the recipes
// record kind sprite and the entity, exactly as the MCP tool's.
// - Preflight before any write: every source resolved and probed, settings, frames and outputs
//   checked (plannedOutputs), duplicates across the batch and outputs equal to an input refused
//   (symlinks resolved), existing outputs without --overwrite listed; then every output of every
//   source is encoded and written by one writeOutputsDirect (all or nothing).
// - Output: a summary per source on stderr (`-q` silences it, not errors), or with --format json
//   exactly one object on stdout {ok, dry_run, seconds, results: [...], errors: [{kind, message,
//   fix, source?}]}. Engine logs never reach stdout.
// - Exit code: 0 written (or dry run done; warnings allowed), 1 generation or write failed, or an
//   output exists without --overwrite (nothing written), 2 usage error. argv[0] is the subcommand.

#include <iosfwd>
#include <string>

namespace jasperengine::editor::spritenormals {

    int runGenerateSpriteNormals(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err);
    void printGenerateSpriteNormalsUsage(const std::string& commandName, std::ostream& out);

}
