// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// EditorModal and EditorFilePicker: what MCP sees of a modal and how an answer reaches its widgets,
// on a real ImGui context without a display or GPU (S69 AC2). The whole path through respond_dialog
// is tools/mcp/tests/test_editor_modals.py.

#include <gtest/gtest.h>

#include "window/EditorFilePicker.h"
#include "window/EditorModal.h"
#include "window/LoadingWindow.h"

#include "imgui.h"
#include "thread/ResourceProgress.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    struct ModalContext {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGuiContext* context = ImGui::CreateContext();

        ModalContext() {
            ImGui::SetCurrentContext(context);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(800, 600);
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            EditorModal::reset();
        }

        ~ModalContext() {
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previous);
            EditorModal::reset();
        }

        // a frame as EditorFrame runs it: the windows, then the snapshot
        void frame(const std::function<void()>& windows) {
            ImGui::NewFrame();
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            windows();
            EditorModal::endFrame();
            ImGui::EndFrame();
        }
    };

    std::vector<std::string> strings(std::initializer_list<const char*> items) {
        return std::vector<std::string>(items.begin(), items.end());
    }

    const EditorDialogField* findField(const EditorDialogInfo& info, const std::string& name) {
        for (const EditorDialogField& field : info.fields) {
            if (field.name == name) return &field;
        }
        return nullptr;
    }

}

TEST(EditorModalTest, ReportsTheModalAfterItsFrame) {
    ModalContext ui;
    EditorDialogInfo info;
    EXPECT_FALSE(EditorModal::topModal(info));
    auto draw = [&]() {
        ImGui::OpenPopup("Delete it?##modal_id");
        if (EditorModal::begin("Delete it?##modal_id")) {
            EditorModal::message("The file goes away.");
            EditorModal::button("Yes", "yes");
            EditorModal::button("Keep File", "no");
            ImGui::EndPopup();
        }
    };
    ui.frame(draw);
    ASSERT_TRUE(EditorModal::topModal(info));
    EXPECT_EQ(info.kind, "modal");
    EXPECT_EQ(info.title, "Delete it?");
    EXPECT_EQ(info.message, "The file goes away. [yes = Yes, no = Keep File]");
    EXPECT_EQ(info.choices, strings({"yes", "no"}));
    EXPECT_TRUE(info.blocksEdits);
}

TEST(EditorModalTest, AnswerPressesTheButtonOnce) {
    ModalContext ui;
    int yes = 0, no = 0;
    bool open = true;
    auto draw = [&]() {
        if (open) ImGui::OpenPopup("Question");
        if (EditorModal::begin("Question")) {
            if (EditorModal::button("Yes", "yes")) {
                yes++;
                open = false;
                ImGui::CloseCurrentPopup();
            }
            if (EditorModal::button("No", "no")) no++;
            ImGui::EndPopup();
        }
    };
    ui.frame(draw);
    std::string error;
    ASSERT_TRUE(EditorModal::answer("yes", {}, error)) << error;
    ui.frame(draw);
    EXPECT_EQ(yes, 1);
    EXPECT_EQ(no, 0);
    EXPECT_TRUE(EditorModal::answerSettled());
    ui.frame(draw);
    EXPECT_EQ(yes, 1) << "only once";
    EditorDialogInfo info;
    EXPECT_FALSE(EditorModal::topModal(info));
    EXPECT_FALSE(EditorModal::answer("yes", {}, error));
}

TEST(EditorModalTest, AChoiceItDoesNotHaveIsRefusedNamingTheValidOnes) {
    ModalContext ui;
    ui.frame([&]() {
        ImGui::OpenPopup("Question");
        if (EditorModal::begin("Question")) {
            EditorModal::button("Yes", "yes");
            EditorModal::button("No", "no");
            ImGui::EndPopup();
        }
    });
    std::string error;
    EXPECT_FALSE(EditorModal::answer("cancel", {}, error));
    EXPECT_NE(error.find("yes, no"), std::string::npos) << error;
}

TEST(EditorModalTest, NestedModalTheTopMostIsReportedAndAnswered) {
    ModalContext ui;
    int parentOk = 0, childOk = 0;
    bool childOpen = true;
    auto draw = [&]() {
        ImGui::OpenPopup("Parent");
        if (EditorModal::begin("Parent")) {
            if (EditorModal::button("OK", "ok")) parentOk++;
            if (childOpen) ImGui::OpenPopup("Child");
            if (EditorModal::begin("Child")) {
                EditorModal::message("A problem.");
                if (EditorModal::button("OK", "ok")) {
                    childOk++;
                    childOpen = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::EndPopup();
        }
    };
    ui.frame(draw);
    EditorDialogInfo info;
    ASSERT_TRUE(EditorModal::topModal(info));
    EXPECT_EQ(info.title, "Child");
    EXPECT_EQ(info.message, "A problem.");
    EXPECT_TRUE(EditorModal::isOpen("Parent")) << "under the top-most one";
    std::string error;
    ASSERT_TRUE(EditorModal::answer("ok", {}, error)) << error;
    ui.frame(draw);
    EXPECT_EQ(childOk, 1);
    EXPECT_EQ(parentOk, 0) << "the parent's button shares the choice, not the answer";
    ui.frame(draw);
    ASSERT_TRUE(EditorModal::topModal(info));
    EXPECT_EQ(info.title, "Parent");
}

TEST(EditorModalTest, RolesProgressIsNotADialogAndMergesEditsDoesNotRefuse) {
    {
        ModalContext ui;
        ui.frame([&]() {
            ImGui::OpenPopup("Loading");
            if (EditorModal::begin("Loading", nullptr, 0, EditorModal::Role::Progress)) ImGui::EndPopup();
        });
        EditorDialogInfo info;
        EXPECT_FALSE(EditorModal::topModal(info));
    }
    {
        ModalContext ui;
        ui.frame([&]() {
            ImGui::OpenPopup("Question");
            if (EditorModal::begin("Question")) {
                EditorModal::button("OK", "ok");
                ImGui::OpenPopup("Loading");
                if (EditorModal::begin("Loading", nullptr, 0, EditorModal::Role::Progress)) ImGui::EndPopup();
                ImGui::EndPopup();
            }
        });
        EditorDialogInfo info;
        ASSERT_TRUE(EditorModal::topModal(info)) << "what is under a progress modal still waits";
        EXPECT_EQ(info.title, "Question");
    }
    {
        ModalContext ui;
        ui.frame([&]() {
            ImGui::OpenPopup("Project Settings");
            if (EditorModal::begin("Project Settings", nullptr, 0, EditorModal::Role::MergesEdits)) ImGui::EndPopup();
        });
        EditorDialogInfo info;
        ASSERT_TRUE(EditorModal::topModal(info));
        EXPECT_FALSE(info.blocksEdits);
    }
}

TEST(EditorModalTest, CancelPressesTheTitleBarCloseButton) {
    ModalContext ui;
    bool open = true;
    auto draw = [&]() {
        if (open) ImGui::OpenPopup("Window");
        if (EditorModal::begin("Window", &open)) ImGui::EndPopup();
    };
    ui.frame(draw);
    EditorDialogInfo info;
    ASSERT_TRUE(EditorModal::topModal(info));
    EXPECT_EQ(info.choices, strings({"cancel"}));
    std::string error;
    ASSERT_TRUE(EditorModal::answer("cancel", {}, error)) << error;
    ui.frame(draw);
    EXPECT_FALSE(open);
    ui.frame(draw);
    EXPECT_FALSE(EditorModal::topModal(info));
}

TEST(EditorModalTest, FieldsAreReportedWithTheirValues) {
    ModalContext ui;
    char name[32] = "Hero";
    bool shared = true;
    int width = 640;
    float dpi = 96.5f;
    int mode = 1;
    std::string path = "assets/a.png";
    ui.frame([&]() {
        ImGui::OpenPopup("Prompt");
        if (EditorModal::begin("Prompt")) {
            EditorModal::inputText("##name", "name", name, sizeof(name));
            EditorModal::checkbox("Shared", "shared", &shared);
            EditorModal::inputInt("Width", "width", &width);
            EditorModal::inputFloat("DPI", "dpi", &dpi);
            EditorModal::combo("Mode", "mode", &mode, {"Box", "Disc"});
            EditorModal::pathField("path", path);
            ImGui::EndPopup();
        }
    });
    EditorDialogInfo info;
    ASSERT_TRUE(EditorModal::topModal(info));
    ASSERT_EQ(info.fields.size(), 6u);
    const EditorDialogField* field = findField(info, "name");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->type, "text");
    EXPECT_EQ(field->value.text, "Hero");
    EXPECT_TRUE(findField(info, "shared")->value.flag);
    EXPECT_EQ(findField(info, "width")->type, "int");
    EXPECT_EQ(findField(info, "width")->value.number, 640.0);
    EXPECT_FLOAT_EQ(static_cast<float>(findField(info, "dpi")->value.number), 96.5f);
    EXPECT_EQ(findField(info, "mode")->type, "enum");
    EXPECT_EQ(findField(info, "mode")->value.text, "Disc");
    EXPECT_EQ(findField(info, "mode")->options, strings({"Box", "Disc"}));
    EXPECT_EQ(findField(info, "path")->type, "path");
}

TEST(EditorModalTest, ValuesGoIntoTheFieldsThenTheButtonIsPressed) {
    ModalContext ui;
    char name[32] = "Old";
    bool shared = false;
    int width = 1;
    int mode = 0;
    std::string pressedWith;
    bool open = true;
    auto draw = [&]() {
        if (open) ImGui::OpenPopup("Prompt");
        if (EditorModal::begin("Prompt")) {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            EditorModal::inputText("##name", "name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue);
            EditorModal::checkbox("Shared", "shared", &shared);
            EditorModal::inputInt("Width", "width", &width);
            EditorModal::combo("Mode", "mode", &mode, {"Box", "Disc"});
            if (EditorModal::button("OK", "ok")) {
                pressedWith = name;
                open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    };
    ui.frame(draw);
    ui.frame(draw);     // the caret is in the name field: its widget has its own copy of the text
    std::string error;
    ASSERT_TRUE(EditorModal::answer("ok", {{"name", EditorDialogValue::ofText("New")},
                                           {"shared", EditorDialogValue::ofBool(true)},
                                           {"width", EditorDialogValue::ofNumber(320)},
                                           {"mode", EditorDialogValue::ofText("Disc")}}, error)) << error;
    ui.frame(draw);
    EXPECT_STREQ(name, "New");
    EXPECT_TRUE(shared);
    EXPECT_EQ(width, 320);
    EXPECT_EQ(mode, 1);
    EXPECT_TRUE(pressedWith.empty()) << "the button waits for a frame whose checks saw the values";
    ui.frame(draw);
    EXPECT_EQ(pressedWith, "New");
    EXPECT_TRUE(EditorModal::takeAnswerError().empty());
}

TEST(EditorModalTest, ValuesWithoutAChoiceOnlyFillTheFields) {
    ModalContext ui;
    char name[32] = "Old";
    int pressed = 0;
    auto draw = [&]() {
        ImGui::OpenPopup("Prompt");
        if (EditorModal::begin("Prompt")) {
            EditorModal::inputText("##name", "name", name, sizeof(name));
            if (EditorModal::button("OK", "ok")) pressed++;
            ImGui::EndPopup();
        }
    };
    ui.frame(draw);
    std::string error;
    ASSERT_TRUE(EditorModal::answer("", {{"name", EditorDialogValue::ofText("New")}}, error)) << error;
    ui.frame(draw);
    ui.frame(draw);
    EXPECT_STREQ(name, "New");
    EXPECT_EQ(pressed, 0);
    EXPECT_TRUE(EditorModal::answerSettled());
    EditorDialogInfo info;
    ASSERT_TRUE(EditorModal::topModal(info));
    EXPECT_EQ(findField(info, "name")->value.text, "New");
}

TEST(EditorModalTest, BadValuesAreRefusedBeforeAnyIsWritten) {
    ModalContext ui;
    char name[32] = "Old";
    bool flag = false;
    int count = 3;
    int mode = 0;
    auto draw = [&]() {
        ImGui::OpenPopup("Prompt");
        if (EditorModal::begin("Prompt")) {
            EditorModal::inputText("##name", "name", name, sizeof(name));
            EditorModal::checkbox("Flag", "flag", &flag);
            EditorModal::inputInt("Count", "count", &count);
            EditorModal::combo("Mode", "mode", &mode, {"Box", "Disc"});
            EditorModal::button("OK", "ok");
            ImGui::EndPopup();
        }
    };
    ui.frame(draw);
    std::string error;
    EXPECT_FALSE(EditorModal::answer("ok", {{"name", EditorDialogValue::ofText("New")}, {"nope", EditorDialogValue::ofBool(true)}}, error));
    EXPECT_NE(error.find("no field nope"), std::string::npos) << error;
    EXPECT_NE(error.find("name, flag, count, mode"), std::string::npos) << "names the fields: " << error;
    EXPECT_FALSE(EditorModal::answer("ok", {{"flag", EditorDialogValue::ofText("yes")}}, error));
    EXPECT_NE(error.find("true or false"), std::string::npos) << error;
    EXPECT_FALSE(EditorModal::answer("ok", {{"count", EditorDialogValue::ofNumber(1.5)}}, error));
    EXPECT_NE(error.find("whole number"), std::string::npos) << error;
    EXPECT_FALSE(EditorModal::answer("ok", {{"mode", EditorDialogValue::ofText("Cone")}}, error));
    EXPECT_NE(error.find("Box, Disc"), std::string::npos) << error;
    ui.frame(draw);
    EXPECT_STREQ(name, "Old") << "nothing of a refused answer applies";
}

TEST(EditorModalTest, AGreyedOutButtonIsNoAnswerAndSaysWhy) {
    ModalContext ui;
    char name[32] = "";
    int pressed = 0;
    auto draw = [&]() {
        ImGui::OpenPopup("Prompt");
        if (EditorModal::begin("Prompt")) {
            EditorModal::inputText("##name", "name", name, sizeof(name));
            const bool valid = std::string(name).find('/') == std::string::npos && name[0] != '\0';
            EditorModal::error(valid ? std::string() : "The name is not valid.");
            ImGui::BeginDisabled(!valid);
            if (EditorModal::button("OK", "ok")) pressed++;
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
    };
    ui.frame(draw);
    EditorDialogInfo info;
    ASSERT_TRUE(EditorModal::topModal(info));
    EXPECT_TRUE(info.choices.empty());
    EXPECT_EQ(info.disabledChoices, strings({"ok"}));
    EXPECT_EQ(info.error, "The name is not valid.");

    std::string error;
    EXPECT_FALSE(EditorModal::answer("ok", {}, error)) << "no new values: it stays greyed out";
    EXPECT_NE(error.find("The name is not valid."), std::string::npos) << error;

    // values that do not enable it: the press is dropped and the reply can say why
    ASSERT_TRUE(EditorModal::answer("ok", {{"name", EditorDialogValue::ofText("a/b")}}, error)) << error;
    for (int i = 0; i < 4; i++) ui.frame(draw);
    EXPECT_EQ(pressed, 0);
    EXPECT_TRUE(EditorModal::answerSettled());
    const std::string refused = EditorModal::takeAnswerError();
    EXPECT_NE(refused.find("greyed out"), std::string::npos) << refused;
    EXPECT_NE(refused.find("The name is not valid."), std::string::npos) << refused;
    EXPECT_TRUE(EditorModal::takeAnswerError().empty()) << "said once";

    // values that do: pressed
    ASSERT_TRUE(EditorModal::answer("ok", {{"name", EditorDialogValue::ofText("good")}}, error)) << error;
    for (int i = 0; i < 2; i++) ui.frame(draw);
    EXPECT_EQ(pressed, 1);
}

TEST(EditorModalTest, AValueForAFieldNoLongerDrawnIsReported) {
    ModalContext ui;
    bool showField = true;
    std::string text = "x";
    auto draw = [&]() {
        ImGui::OpenPopup("Prompt");
        if (EditorModal::begin("Prompt")) {
            if (showField) EditorModal::field("text", text);
            ImGui::EndPopup();
        }
    };
    ui.frame(draw);
    std::string error;
    ASSERT_TRUE(EditorModal::answer("", {{"text", EditorDialogValue::ofText("y")}}, error)) << error;
    showField = false;
    ui.frame(draw);
    ui.frame(draw);
    EXPECT_NE(EditorModal::takeAnswerError().find("no longer shows the field"), std::string::npos);
    EXPECT_EQ(text, "x");
}

TEST(EditorModalTest, AnAnswerToAModalThatWentAwayIsDropped) {
    ModalContext ui;
    bool draw = true;
    int pressed = 0;
    auto windows = [&]() {
        if (!draw) return;
        ImGui::OpenPopup("Question");
        if (EditorModal::begin("Question")) {
            if (EditorModal::button("OK", "ok")) pressed++;
            ImGui::EndPopup();
        }
    };
    ui.frame(windows);
    std::string error;
    ASSERT_TRUE(EditorModal::answer("ok", {}, error)) << error;
    draw = false;
    ui.frame(windows);
    EXPECT_TRUE(EditorModal::answerSettled());
    draw = true;
    ui.frame(windows);
    EXPECT_EQ(pressed, 0);
}

TEST(EditorModalTest, ANewPopupAsksForAFrameAndNoMore) {
    ModalContext ui;
    EXPECT_FALSE(EditorModal::wantsFrame());
    ui.frame([]() {
        // nothing begins it in this frame: a window later in the frame's order would, next frame
        ImGui::OpenPopup("Later");
    });
    EXPECT_TRUE(EditorModal::settling());
    EXPECT_TRUE(EditorModal::wantsFrame());
    ui.frame([]() {
        ImGui::OpenPopup("Later");
        if (EditorModal::begin("Later")) ImGui::EndPopup();
    });
    EXPECT_FALSE(EditorModal::settling());
    EXPECT_FALSE(EditorModal::wantsFrame());
    EditorDialogInfo info;
    EXPECT_TRUE(EditorModal::topModal(info));
}

TEST(EditorModalTest, APopupNothingBeginsDoesNotKeepTheEditorAwake) {
    ModalContext ui;
    auto windows = []() { ImGui::OpenPopup("Never begun"); };
    for (int i = 0; i < 5; i++) ui.frame(windows);
    EXPECT_FALSE(EditorModal::settling());
    EXPECT_FALSE(EditorModal::wantsFrame());
}

TEST(EditorModalTest, ChoiceOfALabel) {
    EXPECT_EQ(EditorModal::choiceOf("Keep Mine"), "keep_mine");
    EXPECT_EQ(EditorModal::choiceOf("Force-Push!"), "force_push");
    EXPECT_EQ(EditorModal::choiceOf("Open##id"), "open");
}

// ------------------------------------------------------------------ the Loading popup

namespace {

    // A popup opened once, as most dialogs are, and the build progress, in one frame's order
    struct DialogAndBuilds {
        ModalContext ui;
        LoadingWindow loading;
        bool ask = false;
        bool answered = false;

        void frame() {
            ui.frame([this]() {
                if (ask) {
                    ImGui::OpenPopup("Question");
                    ask = false;
                }
                if (EditorModal::begin("Question")) {
                    if (EditorModal::button("OK", "ok") || answered) ImGui::CloseCurrentPopup();
                    answered = false;
                    ImGui::EndPopup();
                }
                loading.showBuilds(ResourceProgress::hasActiveBuilds());
            });
        }

        bool loadingIsOpen() {
            bool open = false;
            ui.frame([&]() {
                open = ImGui::IsPopupOpen("Loading");
                if (EditorModal::begin("Question")) ImGui::EndPopup();
                loading.showBuilds(ResourceProgress::hasActiveBuilds());
            });
            return open;
        }

        std::string reported() {
            EditorDialogInfo info;
            return EditorModal::topModal(info) ? info.title : std::string();
        }
    };

    constexpr uint64_t TEST_BUILD = 0x5369ull << 32;

}

TEST(EditorModalLoadingTest, ADialogAskedForOnceSurvivesABuildThatStartsAfterIt) {
    DialogAndBuilds editor;
    editor.ask = true;
    editor.frame();
    EXPECT_EQ(editor.reported(), "Question");

    ResourceProgress::startBuild(TEST_BUILD, ResourceType::Shader, "s69");
    for (int i = 0; i < 4; i++) {
        editor.frame();
        EXPECT_EQ(editor.reported(), "Question") << "frame " << i << ": the progress must not take its place";
    }
    EXPECT_FALSE(editor.loadingIsOpen()) << "the progress waits for the dialog";

    // answered: the progress the builds still owe comes up, and ends with them
    editor.answered = true;
    editor.frame();
    editor.frame();
    EXPECT_TRUE(editor.loadingIsOpen());
    EXPECT_EQ(editor.reported(), "") << "a progress popup is not a dialog";
    ResourceProgress::completeBuild(TEST_BUILD);
    editor.frame();
    editor.frame();
    EXPECT_FALSE(editor.loadingIsOpen());

    // nothing of it is left to hold back the next builds' progress
    ResourceProgress::startBuild(TEST_BUILD + 2, ResourceType::Shader, "s69");
    editor.frame();
    EXPECT_TRUE(editor.loadingIsOpen());
    ResourceProgress::completeBuild(TEST_BUILD + 2);
    editor.frame();
    editor.frame();
    EXPECT_FALSE(editor.loadingIsOpen());
}

TEST(EditorModalLoadingTest, ADialogAskedForDuringABuildOpensAndTheProgressComesBack) {
    DialogAndBuilds editor;
    ResourceProgress::startBuild(TEST_BUILD + 1, ResourceType::Shader, "s69");
    editor.frame();
    EXPECT_TRUE(editor.loadingIsOpen());

    editor.ask = true;
    editor.frame();
    editor.frame();
    EXPECT_EQ(editor.reported(), "Question") << "asked for while shaders build";
    EXPECT_FALSE(editor.loadingIsOpen());

    editor.answered = true;
    editor.frame();
    editor.frame();
    EXPECT_TRUE(editor.loadingIsOpen()) << "the builds are not done";
    ResourceProgress::completeBuild(TEST_BUILD + 1);
    editor.frame();
    editor.frame();
    EXPECT_FALSE(editor.loadingIsOpen());
}

// ------------------------------------------------------------------ file pickers

namespace {

    struct PickerHost {
        std::atomic<bool> openedCalled{false};

        explicit PickerHost(EditorFilePicker::Route route, const std::string& base = std::string()) {
            EditorFilePicker::Host host;
            host.route = [route]() { return route; };
            host.opened = [this]() { openedCalled = true; };
            host.baseDirectory = [base]() { return base; };
            EditorFilePicker::setHost(std::move(host));
        }

        ~PickerHost() {
            EditorFilePicker::setHost({});
        }
    };

    std::filesystem::path scratch(const char* name) {
        std::filesystem::path folder = std::filesystem::temp_directory_path() /
            ("je_picker_" + std::string(name) + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(folder);
        return folder;
    }

    // ask() on another thread, as the main thread is in the editor; true once it reports open
    bool waitOpen() {
        for (int i = 0; i < 2000 && !EditorFilePicker::isOpen(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return EditorFilePicker::isOpen();
    }

}

TEST(EditorFilePickerTest, RoutesOfAHost) {
    EditorFilePicker::Request request;
    {
        EXPECT_FALSE(EditorFilePicker::ask(request).has_value()) << "no host: the native dialog";
    }
    {
        PickerHost host(EditorFilePicker::Route::Native);
        EXPECT_FALSE(EditorFilePicker::ask(request).has_value());
    }
    {
        PickerHost host(EditorFilePicker::Route::Cancel);
        const auto answered = EditorFilePicker::ask(request);
        ASSERT_TRUE(answered.has_value()) << "headless, nobody connected: as if cancelled, never a hang";
        EXPECT_TRUE(answered->empty());
    }
}

TEST(EditorFilePickerTest, AnAgentAnswersWithAPathItChecks) {
    const std::filesystem::path folder = scratch("answer");
    { std::ofstream(folder / "a.png") << "x"; }
    { std::ofstream(folder / "b.txt") << "x"; }
    PickerHost host(EditorFilePicker::Route::Agent, folder.string());

    EditorFilePicker::Request request;
    request.mode = EditorFilePicker::Mode::OpenFile;
    request.extensions = {"png", "jpg"};
    std::vector<std::string> result;
    std::thread editor([&]() { result = EditorFilePicker::ask(request).value_or(std::vector<std::string>{"NATIVE"}); });
    ASSERT_TRUE(waitOpen());
    EXPECT_TRUE(host.openedCalled);

    EditorDialogInfo info;
    ASSERT_TRUE(EditorFilePicker::info(info));
    EXPECT_EQ(info.kind, "file_picker");
    EXPECT_EQ(info.title, "Open File");
    EXPECT_NE(info.message.find("png, jpg"), std::string::npos) << info.message;
    EXPECT_EQ(info.choices, strings({"ok", "cancel"}));
    ASSERT_EQ(info.fields.size(), 1u);
    EXPECT_EQ(info.fields[0].name, "path");

    std::string error;
    EXPECT_FALSE(EditorFilePicker::answer({}, error));
    EXPECT_NE(error.find("values.path"), std::string::npos) << error;
    EXPECT_FALSE(EditorFilePicker::answer({"missing.png"}, error));
    EXPECT_NE(error.find("does not exist"), std::string::npos) << error;
    EXPECT_FALSE(EditorFilePicker::answer({"b.txt"}, error));
    EXPECT_NE(error.find("png, jpg"), std::string::npos) << error;
    EXPECT_FALSE(EditorFilePicker::answer({"a.png", "a.png"}, error));
    EXPECT_TRUE(EditorFilePicker::isOpen()) << "a refused answer leaves it open";

    ASSERT_TRUE(EditorFilePicker::answer({"a.png"}, error)) << error;
    editor.join();
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(std::filesystem::path(result[0]), (folder / "a.png").lexically_normal()) << "relative to the project";
    EXPECT_FALSE(EditorFilePicker::isOpen());
    std::filesystem::remove_all(folder);
}

TEST(EditorFilePickerTest, FolderAndSaveModes) {
    const std::filesystem::path folder = scratch("modes");
    PickerHost host(EditorFilePicker::Route::Agent, folder.string());
    std::string error;
    {
        EditorFilePicker::Request request;
        request.mode = EditorFilePicker::Mode::OpenFolder;
        std::vector<std::string> result;
        std::thread editor([&]() { result = *EditorFilePicker::ask(request); });
        ASSERT_TRUE(waitOpen());
        EXPECT_FALSE(EditorFilePicker::answer({"no-such-folder"}, error));
        ASSERT_TRUE(EditorFilePicker::answer({folder.string()}, error)) << error;
        editor.join();
        ASSERT_EQ(result.size(), 1u);
    }
    {
        EditorFilePicker::Request request;
        request.mode = EditorFilePicker::Mode::SaveFile;
        std::vector<std::string> result;
        std::thread editor([&]() { result = *EditorFilePicker::ask(request); });
        ASSERT_TRUE(waitOpen());
        EXPECT_FALSE(EditorFilePicker::answer({"no-such-folder/new.anim"}, error));
        EXPECT_NE(error.find("does not exist"), std::string::npos) << error;
        ASSERT_TRUE(EditorFilePicker::answer({"new.anim"}, error)) << error;
        editor.join();
        ASSERT_EQ(result.size(), 1u);
    }
    std::filesystem::remove_all(folder);
}

TEST(EditorFilePickerTest, CancelAndAbandonReturnNothing) {
    PickerHost host(EditorFilePicker::Route::Agent);
    EditorFilePicker::Request request;
    for (const bool abandon : {false, true}) {
        std::vector<std::string> result = {"unset"};
        std::thread editor([&]() { result = *EditorFilePicker::ask(request); });
        ASSERT_TRUE(waitOpen());
        std::string error;
        if (abandon) {
            EditorFilePicker::abandon();
        } else {
            ASSERT_TRUE(EditorFilePicker::cancel(error)) << error;
        }
        editor.join();
        EXPECT_TRUE(result.empty());
    }
}

TEST(EditorFilePickerTest, ANativePickerIsReportedButNotAnswerable) {
    EditorFilePicker::Request request;
    request.mode = EditorFilePicker::Mode::SaveFile;
    {
        EditorFilePicker::NativeScope native(request);
        EditorDialogInfo info;
        ASSERT_TRUE(EditorFilePicker::info(info));
        EXPECT_EQ(info.kind, "file_picker");
        EXPECT_TRUE(info.choices.empty()) << "only a human can close it";
        EXPECT_NE(info.message.find("human"), std::string::npos) << info.message;
        std::string error;
        EXPECT_FALSE(EditorFilePicker::answer({"x"}, error));
        EXPECT_NE(error.find("human"), std::string::npos) << error;
        EXPECT_FALSE(EditorFilePicker::cancel(error));
    }
    EXPECT_FALSE(EditorFilePicker::isOpen());
}
