-- S28's test project: what the Lua Crash API reports at start (main.cpp says where it runs)
Log.print("[gate] crash diagnostics: last session abnormal " .. tostring(Crash.lastSessionEndedAbnormally()))
Log.print("[gate] crash diagnostics: pending reports " .. #Crash.getPendingReports())
Crash.setTag("level", "crash-diagnostics")
Crash.setReportHook(function(report)
    Log.print("[gate] crash diagnostics: hook got " .. report.kind .. " " .. report.id)
    return true
end)

-- the Gameplay Journal lands in the crash report (journal.txt, or the web's report.journal)
Journal.log("gate.ready", {project = "crash-diagnostics"})
-- a user report, read back as a game would upload it (the web keeps it in memory), then handled
local report, err = Crash.createReport{note = "gate user report", category = "gate"}
if report then
    local bytes = Crash.readReportFile(report.id, "user.txt")
    Log.print("[gate] crash diagnostics: user report file " .. tostring(bytes and #bytes > 0))
    Crash.markHandled(report.id)
else
    Log.print("[gate] crash diagnostics: user report failed " .. tostring(err))
end
