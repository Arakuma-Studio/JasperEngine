#include "../Common/TestsShell.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Types.h>
#include <doctest.h>
#include <nanobench.h>

using namespace ankerl;
using namespace Rml;

// CSS Grid layout (phase 3, p3-grid): a medium grid, an inventory of 100 slots, laid out with fixed tracks, with
// auto-fill flexible tracks sized by their content, and as a wrapping flexbox for reference.

static const String rml_grid_document = R"(
<rml>
<head>
	<link type="text/rcss" href="/../Tests/Data/style.rcss"/>
	<style>
		body { width: 800px; }
		#inventory { padding: 8dp; gap: 6dp; background-color: #333; }
		#inventory.fixed { display: grid; grid-template-columns: repeat(10, 64dp); grid-auto-rows: 64dp; }
		#inventory.auto-fill { display: grid; grid-template-columns: repeat(auto-fill, minmax(64dp, 1fr)); grid-auto-rows: auto; }
		#inventory.flex { display: flex; flex-wrap: wrap; }
		#inventory.flex .slot { width: 64dp; height: 64dp; }
		.slot { display: block; border: 2dp #777; background-color: #555; padding: 4dp; }
		.slot.rare { border-color: #49f; }
		.icon { display: block; height: 32dp; background-color: #8a6; }
		.count { display: block; text-align: right; font-size: 12dp; color: white; }
	</style>
</head>
<body><div id="inventory"/></body>
</rml>
)";

static String InventoryBody()
{
	String rml;
	for (int i = 0; i < 100; i++)
		rml += CreateString("<div class=\"slot%s\"><div class=\"icon\"/><div class=\"count\">%d</div></div>", (i % 7 == 0 ? " rare" : ""), i % 20);
	return rml;
}

TEST_CASE("grid")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);

	ElementDocument* document = context->LoadDocumentFromMemory(rml_grid_document);
	REQUIRE(document);
	document->Show();
	Element* inventory = document->GetElementById("inventory");
	REQUIRE(inventory);
	const String body = InventoryBody();

	nanobench::Bench bench;
	bench.title("Grid inventory (100 items)");
	bench.relative(true);

	for (const char* variant : {"flex", "fixed", "auto-fill"})
	{
		inventory->SetClassNames(variant);
		inventory->SetInnerRML(body);
		context->Update();
		context->Render();
		TestsShell::RenderLoop();

		const String name = String(variant);
		bench.run(name + ": SetInnerRML + Update", [&] {
			inventory->SetInnerRML(body);
			context->Update();
		});
		bench.run(name + ": Relayout", [&] {
			inventory->SetProperty("width", "700px");
			context->Update();
			inventory->RemoveProperty("width");
			context->Update();
		});
		bench.run(name + ": Update (unmodified)", [&] { context->Update(); });
	}

	document->Close();
}
