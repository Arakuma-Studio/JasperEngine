// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The staging folders of the Project Hub's imports and template copies
// (docs/specs/S46-crash-leftovers.md).

namespace jasperengine::editor::hub::staging {

    // Test hook (JASPER_TEST_STAGING_HOLD=<path>): a copy waits here, with part of its files in the
    // staging folder, while <path> exists, after creating <path>.held to say it got here
    void holdForTest();

}
