#pragma once

#include <RmlUi/Core/Types.h>
namespace Rml {
class Element;
class RenderInterface;
} // namespace Rml
class TestsRenderInterface;
class TestsSystemInterface;

namespace TestsShell {

// Will initialize the shell and create a context on first use.
Rml::Context* GetContext(bool allow_debugger = true, Rml::RenderInterface* override_render_interface = nullptr);

void BeginFrame();
void PresentFrame();

// Render the current state of the context. Press 'escape' or 'return' to break out of the loop.
// Useful for viewing documents while building the RML to benchmark.
// Applies only when compiled with the shell backend.
void RenderLoop(bool block_until_escape = true);

void ShutdownShell(bool reset_tests_render_interface = true);

// Set the number of expected warnings and errors logged by RmlUi until the next call to this function
// or until 'ShutdownShell()'.
void SetNumExpectedWarnings(int num_warnings);

// Stats only available for the dummy renderer.
Rml::String GetRenderStats();

// Returns nullptr if the dummy renderer is not being used.
TestsRenderInterface* GetTestsRenderInterface();
void ResetTestsRenderInterface();
TestsSystemInterface* GetTestsSystemInterface();

// The fork keeps white-space-only text between elements as text elements (CSS Text 3: it is a space between
// inline-level boxes; docs/html-ui/FORK.md P2-8). These walk the children of an element without them, like the DOM's
// 'children': the child at 'index', the number of such children, and the next such sibling.
Rml::Element* GetElementChild(Rml::Element* parent, int index);
int GetNumElementChildren(Rml::Element* parent);
Rml::Element* GetNextElementSibling(Rml::Element* element);

} // namespace TestsShell
