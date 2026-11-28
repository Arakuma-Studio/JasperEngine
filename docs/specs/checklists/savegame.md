# SaveGame manual check on iOS (docs/specs/savegame.md AC23)

Run on a device or the Simulator with tools/test-projects/savegame exported for iOS (or any game
that saves with SaveGame). Each step names what to see; the Xcode console shows the game's log.

1. Launch the app for the first time. The console shows `savegame: written`, then
   `savegame: big ok`.
2. In the Files app (or the Simulator's container: `xcrun simctl get_app_container booted <id>
   data`), Documents/saves holds `slot1.g1.jsave` and `big.g1.jsave`.
3. Send the app to the background (swipe up) and kill it from the app switcher.
4. Launch it again. The console shows `savegame: read ok gen 1`.
5. Background the app while it writes (a game that saves in `onApplicationPause`): the console has
   no `was not written before the app paused` line, and the next launch reads the new data.
6. With the device almost full (or a 1 GB free-space limit in the Simulator's settings), a write
   that cannot land completes with code `io`; the earlier generation still reads.
7. `SaveGame.storage()` (through the dev console's `lua return Json.encode(SaveGame.storage())`)
   gives `free` above 0 and `quota` -1.
