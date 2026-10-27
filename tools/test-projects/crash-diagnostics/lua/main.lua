-- S28's test project: what the Lua Crash API reports at start (main.cpp says where it runs)
Log.print("[gate] crash diagnostics: last session abnormal " .. tostring(Crash.lastSessionEndedAbnormally()))
Log.print("[gate] crash diagnostics: pending reports " .. #Crash.getPendingReports())
Crash.setTag("level", "crash-diagnostics")
Crash.setReportHook(function(report)
    Log.print("[gate] crash diagnostics: hook got " .. report.kind .. " " .. report.id)
    return true
end)
