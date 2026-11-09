// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UndoTimeline.h"

#include "CommandHandle.h"
#include "Out.h"

#include <algorithm>

using namespace jasperengine;

namespace {

    editor::CommandHistory* projectHistory = nullptr;
    editor::CommandHistory* assetEditHistory = nullptr;

    bool exists(const editor::CommandHistory* history){
        const std::vector<editor::CommandHistory*>& all = editor::CommandHistory::all();
        return history && std::find(all.begin(), all.end(), history) != all.end();
    }

    std::string nameOf(const editor::CommandHistory* history){
        const uint32_t sceneId = history == projectHistory ? 0 : (uint32_t)history->getSceneId();
        if (editor::UndoTimeline::historyName){
            return editor::UndoTimeline::historyName(sceneId);
        }
        return sceneId == 0 ? std::string("Project") : "Scene " + std::to_string(sceneId);
    }

    void warnUser(const std::string& message){
        if (editor::UndoTimeline::warn){
            editor::UndoTimeline::warn(message);
        }else{
            editor::Out::warning(message);
        }
    }

    editor::UndoTimeline::EntryView view(const editor::UndoEntry* entry, editor::CommandHistory* history, bool applied, bool editable){
        editor::UndoTimeline::EntryView result;
        result.entry = entry;
        result.history = history;
        result.project = history == projectHistory;
        result.sceneId = result.project ? 0 : (uint32_t)history->getSceneId();
        result.applied = applied;
        result.editable = editable;
        return result;
    }

    // While the scene plays, P's steps from before Play are edit-time steps
    uint64_t projectFloorId(const editor::CommandHistory* scene){
        return scene ? scene->getPlayStartId() : 0;
    }

    std::string refusal(const char* fallback){
        const std::string& why = editor::CommandHistory::lastBusyRefusal();
        return why.empty() ? std::string(fallback) : "Busy: " + why;
    }

    // Undoes the newest applied step of each history holding `linkId` on top, newest first;
    // a linked step under newer edits stays, with a warning naming its history
    void undoLinked(uint64_t linkId, std::vector<editor::UndoTimeline::Step>* steps, bool& touchedProject){
        while (true){
            editor::CommandHistory* newest = nullptr;
            for (editor::CommandHistory* history : editor::CommandHistory::all()){
                const editor::UndoEntry* top = history->top();
                if (top && top->info.linkId == linkId && (!newest || top->info.id > newest->top()->info.id)){
                    newest = history;
                }
            }
            if (!newest){
                break;
            }
            editor::UndoTimeline::Step step{newest->top()->info.id, newest->top()->name(), newest == projectHistory,
                                            newest == projectHistory ? 0u : (uint32_t)newest->getSceneId()};
            if (!newest->undo()){
                break;
            }
            touchedProject = touchedProject || step.project;
            if (steps) steps->push_back(step);
        }
        for (editor::CommandHistory* history : editor::CommandHistory::all()){
            for (const editor::UndoEntry* entry : history->entries()){
                if (entry->info.linkId == linkId && history->isApplied(entry->info.id)){
                    warnUser("Batch step partly undone: " + nameOf(history) + " has newer edits");
                    break;
                }
            }
        }
    }

    void redoLinked(uint64_t linkId, std::vector<editor::UndoTimeline::Step>* steps, bool& touchedProject){
        while (true){
            editor::CommandHistory* oldest = nullptr;
            for (editor::CommandHistory* history : editor::CommandHistory::all()){
                const editor::UndoEntry* next = history->nextRedo();
                if (next && next->info.linkId == linkId && (!oldest || next->info.id < oldest->nextRedo()->info.id)){
                    oldest = history;
                }
            }
            if (!oldest){
                break;
            }
            editor::UndoTimeline::Step step{oldest->nextRedo()->info.id, oldest->nextRedo()->name(), oldest == projectHistory,
                                            oldest == projectHistory ? 0u : (uint32_t)oldest->getSceneId()};
            if (!oldest->redo()){
                break;
            }
            touchedProject = touchedProject || step.project;
            if (steps) steps->push_back(step);
        }
        for (editor::CommandHistory* history : editor::CommandHistory::all()){
            for (const editor::UndoEntry* entry : history->entries()){
                if (entry->info.linkId == linkId && !history->isApplied(entry->info.id)){
                    warnUser("Batch step partly redone: " + nameOf(history) + " has other steps to redo first");
                    break;
                }
            }
        }
    }

}

std::function<uint32_t()> editor::UndoTimeline::selectedScene = nullptr;
std::function<std::string(uint32_t)> editor::UndoTimeline::historyName = nullptr;
std::function<void(const std::string&)> editor::UndoTimeline::warn = nullptr;
std::function<void()> editor::UndoTimeline::onChanged = nullptr;
std::function<void()> editor::UndoTimeline::onProjectChanged = nullptr;

editor::UndoTimeline::Target editor::UndoTimeline::resolveTarget(bool textInput, bool codeEditorFocused, FocusedWindow lastFocusedWindow){
    if (textInput){
        return Target::TextField;
    }
    if (codeEditorFocused || lastFocusedWindow == FocusedWindow::Code){
        return Target::CodeEditor;
    }
    return Target::Timeline;
}

void editor::UndoTimeline::setProjectHistory(CommandHistory* history){
    projectHistory = history;
}

void editor::UndoTimeline::setAssetEditHistory(CommandHistory* history){
    assetEditHistory = history;
}

editor::CommandHistory* editor::UndoTimeline::getProjectHistory(){
    return exists(projectHistory) ? projectHistory : nullptr;
}

editor::CommandHistory* editor::UndoTimeline::sceneHistory(uint32_t sceneId){
    if (sceneId == 0){
        return nullptr;
    }
    return CommandHandle::find(sceneId);
}

editor::UndoTimeline::EntryView editor::UndoTimeline::nextUndo(uint32_t sceneId){
    CommandHistory* scene = sceneHistory(sceneId);
    CommandHistory* project = getProjectHistory();
    const UndoEntry* sceneTop = scene ? scene->top() : nullptr;
    const UndoEntry* projectTop = project ? project->top() : nullptr;
    if (projectTop && projectTop->info.id < projectFloorId(scene)){
        projectTop = nullptr;
    }
    if (sceneTop && (!projectTop || sceneTop->info.id > projectTop->info.id)){
        return view(sceneTop, scene, true, true);
    }
    if (projectTop){
        return view(projectTop, project, true, true);
    }
    return EntryView();
}

editor::UndoTimeline::EntryView editor::UndoTimeline::nextRedo(uint32_t sceneId){
    CommandHistory* scene = sceneHistory(sceneId);
    CommandHistory* project = getProjectHistory();
    const UndoEntry* sceneNext = scene ? scene->nextRedo() : nullptr;
    const UndoEntry* projectNext = project ? project->nextRedo() : nullptr;
    if (projectNext && projectNext->info.id < projectFloorId(scene)){
        projectNext = nullptr;
    }
    if (sceneNext && (!projectNext || sceneNext->info.id < projectNext->info.id)){
        return view(sceneNext, scene, false, true);
    }
    if (projectNext){
        return view(projectNext, project, false, true);
    }
    return EntryView();
}

bool editor::UndoTimeline::canUndo(uint32_t sceneId){
    CommandHistory* scene = sceneHistory(sceneId);
    return (scene && scene->hasOpenInteractive()) || (assetEditHistory && assetEditHistory->hasOpenInteractive()) || (bool)nextUndo(sceneId);
}

bool editor::UndoTimeline::canRedo(uint32_t sceneId){
    return (bool)nextRedo(sceneId);
}

bool editor::UndoTimeline::undo(uint32_t sceneId, std::string* why, std::vector<Step>* steps){
    // A Physics Placement gesture running in the scene is cancelled instead, whichever history
    // holds the next step (CommandHistory::beforeChange)
    if (CommandHistory::beforeChange && !CommandHistory::beforeChange(sceneId, CommandHistory::Change::Undo)){
        if (why) *why = "Cancelled the running Physics Placement gesture instead of undoing.";
        return false;
    }
    // Ctrl+Z in a drag puts the values back and undoes nothing more
    CommandHistory* scene = sceneHistory(sceneId);
    if (scene && scene->hasOpenInteractive()){
        return scene->cancelInteractive();
    }
    if (assetEditHistory && assetEditHistory->hasOpenInteractive()){
        return assetEditHistory->cancelInteractive();
    }
    EntryView next = nextUndo(sceneId);
    if (!next){
        if (why) *why = "Nothing to undo.";
        return false;
    }
    const uint64_t linkId = next.entry->info.linkId;
    Step step{next.entry->info.id, next.entry->name(), next.project, next.sceneId};
    if (!next.history->undo()){
        if (why) *why = refusal("The step could not be undone.");
        return false;
    }
    if (steps) steps->push_back(step);
    bool touchedProject = step.project;
    if (linkId != 0){
        undoLinked(linkId, steps, touchedProject);
    }
    if (touchedProject && onProjectChanged){
        onProjectChanged();
    }
    return true;
}

bool editor::UndoTimeline::redo(uint32_t sceneId, std::string* why, std::vector<Step>* steps){
    // ... and a redo waits for it
    if (CommandHistory::beforeChange && !CommandHistory::beforeChange(sceneId, CommandHistory::Change::Redo)){
        if (why) *why = "A Physics Placement gesture is running: redo waits until it ends.";
        return false;
    }
    if (assetEditHistory && assetEditHistory->hasOpenInteractive()){
        if (why) *why = "A material is being edited: redo waits until the edit ends.";
        return false;
    }
    EntryView next = nextRedo(sceneId);
    if (!next){
        if (why) *why = "Nothing to redo.";
        return false;
    }
    const uint64_t linkId = next.entry->info.linkId;
    Step step{next.entry->info.id, next.entry->name(), next.project, next.sceneId};
    if (!next.history->redo()){
        if (why) *why = refusal("The step could not be redone.");
        return false;
    }
    if (steps) steps->push_back(step);
    bool touchedProject = step.project;
    if (linkId != 0){
        redoLinked(linkId, steps, touchedProject);
    }
    if (touchedProject && onProjectChanged){
        onProjectChanged();
    }
    return true;
}

bool editor::UndoTimeline::jumpTo(uint32_t sceneId, uint64_t entryId, std::string* why, std::vector<Step>* steps){
    if (entryId == 0){
        while (nextUndo(sceneId)){
            if (!undo(sceneId, why, steps)) return false;
        }
        return true;
    }
    EntryView target = find(sceneId, entryId);
    if (!target){
        if (why) *why = "No step " + std::to_string(entryId) + " in this timeline.";
        return false;
    }
    if (!target.editable){
        if (why) *why = "Edit-time steps can't be undone or redone while the scene plays.";
        return false;
    }
    if (target.applied){
        // undo the newer steps, until the target is on top (or a linked undo took it too)
        while (true){
            EntryView next = nextUndo(sceneId);
            if (!next || next.entry->info.id == entryId) return true;
            EntryView now = find(sceneId, entryId);
            if (!now || !now.applied) return true;
            if (!undo(sceneId, why, steps)) return false;
        }
    }
    while (true){
        EntryView now = find(sceneId, entryId);
        if (!now) return true;      // trimmed meanwhile: nothing more to do
        if (now.applied) return true;
        if (!redo(sceneId, why, steps)) return false;
    }
}

std::vector<editor::UndoTimeline::EntryView> editor::UndoTimeline::entries(uint32_t sceneId, bool includeSelection){
    std::vector<EntryView> result;
    CommandHistory* scene = sceneHistory(sceneId);
    CommandHistory* project = getProjectHistory();
    const uint64_t floorId = projectFloorId(scene);

    if (scene){
        const std::vector<const UndoEntry*> list = scene->entries();
        const size_t applied = scene->appliedCount();
        const size_t floor = scene->isInPlaySession() ? scene->getPlayFloor() - scene->getTrimmedCount() : 0;
        result.reserve(list.size());
        for (size_t i = 0; i < list.size(); i++){
            if (!includeSelection && list[i]->info.selectionOnly) continue;
            result.push_back(view(list[i], scene, i < applied, i >= floor));
        }
    }
    if (project){
        const std::vector<const UndoEntry*> list = project->entries();
        const size_t applied = project->appliedCount();
        for (size_t i = 0; i < list.size(); i++){
            result.push_back(view(list[i], project, i < applied, list[i]->info.id >= floorId));
        }
    }
    std::sort(result.begin(), result.end(), [](const EntryView& a, const EntryView& b){
        return a.entry->info.id < b.entry->info.id;
    });
    return result;
}

editor::UndoTimeline::EntryView editor::UndoTimeline::find(uint32_t sceneId, uint64_t entryId){
    CommandHistory* scene = sceneHistory(sceneId);
    CommandHistory* project = getProjectHistory();
    for (CommandHistory* history : {scene, project}){
        if (!history) continue;
        if (const UndoEntry* entry = history->find(entryId)){
            bool editable = true;
            if (history == project){
                editable = entryId >= projectFloorId(scene);
            }else if (history->isInPlaySession()){
                editable = history->isPlayStep(entryId);
            }
            return view(entry, history, history->isApplied(entryId), editable);
        }
    }
    return EntryView();
}

std::string editor::UndoTimeline::undoLabel(uint32_t sceneId){
    EntryView next = nextUndo(sceneId);
    return next ? "Undo " + next.entry->name() : std::string("Undo");
}

std::string editor::UndoTimeline::redoLabel(uint32_t sceneId){
    EntryView next = nextRedo(sceneId);
    return next ? "Redo " + next.entry->name() : std::string("Redo");
}

void editor::UndoTimeline::freePartnerRedo(CommandHistory& history){
    CommandHistory* project = getProjectHistory();
    if (&history == project){
        CommandHistory* scene = selectedScene ? sceneHistory(selectedScene()) : nullptr;
        if (scene && scene != &history){
            scene->freeRedo();
        }
    }else if (project){
        project->freeRedo();
    }
}

uint64_t editor::UndoTimeline::link(const std::vector<CommandHistory*>& histories){
    const uint64_t linkId = CommandHistory::newLinkId();
    for (CommandHistory* history : histories){
        history->setTopLink(linkId);
    }
    return linkId;
}
