// A window-less implementation of RmlUi's sample backend interface (Backends/RmlUi_Backend.h)
// for the standalone test build (tools/htmlui/run-rmlui-tests.sh). RmlUi's unit tests and
// benchmarks draw with their own dummy render interface and only call the backend when
// RMLUI_TESTS_USE_SHELL=1 asks for a window, which this backend cannot open.

#include <RmlUi/Core/Log.h>
#include <RmlUi_Backend.h>

bool Backend::Initialize(const char* /*window_name*/, int /*width*/, int /*height*/, bool /*allow_resize*/)
{
	Rml::Log::Message(Rml::Log::LT_ERROR, "This test build has no window backend (RMLUI_TESTS_USE_SHELL is not supported).");
	return false;
}

void Backend::Shutdown() {}

Rml::SystemInterface* Backend::GetSystemInterface()
{
	return nullptr;
}

Rml::RenderInterface* Backend::GetRenderInterface()
{
	return nullptr;
}

bool Backend::ProcessEvents(Rml::Context* /*context*/, KeyDownCallback /*key_down_callback*/, bool /*power_save*/)
{
	return false;
}

void Backend::RequestExit() {}

void Backend::BeginFrame() {}

void Backend::PresentFrame() {}
