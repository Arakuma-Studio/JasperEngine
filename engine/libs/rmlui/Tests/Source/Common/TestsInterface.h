#pragma once

#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <Shell.h>

class TestsSystemInterface : public Rml::SystemInterface {
public:
	~TestsSystemInterface();

	double GetElapsedTime() override;

	bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;

	// Checks and clears previously logged messages, then sets the number of expected
	// warnings and errors until the next call.
	void SetNumExpectedWarnings(int num_expected_warnings);

	void SetManualTime(double t);

	void Reset();

private:
	bool manual_time = false;
	double elapsed_time = 0.0;

	int num_logged_warnings = 0;
	int num_expected_warnings = 0;

	Rml::StringList warnings;
};

class TestsRenderInterface : public Rml::RenderInterface {
public:
	struct Counters {
		size_t compile_geometry;
		size_t render_geometry;
		size_t release_geometry;
		size_t load_texture;
		size_t generate_texture;
		size_t release_texture;
		size_t enable_scissor;
		size_t set_scissor;
		size_t enable_clip_mask;
		size_t render_to_clip_mask;
		size_t set_transform;
		size_t compile_filter;
		size_t release_filter;
		size_t compile_shader;
		size_t render_shader;
		size_t release_shader;
		// Layers (phase 3, p3-paint: group opacity, filters).
		size_t push_layer;
		size_t composite_layers;
		size_t pop_layer;
		size_t composite_layers_plus; // with BlendMode::Plus (cross-faded background images)
		size_t generate_texture_with_wrap; // GenerateTextureWithWrap with an axis repeated (also counted in generate_texture)
		size_t save_layer_into_texture;    // SaveLayerIntoTexture: tiles of a texture larger than the window
	};

	Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
	void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) override;
	void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override;

	Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
	Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) override;
	Rml::TextureHandle GenerateTextureWithWrap(Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions, bool repeat_x,
		bool repeat_y) override;
	void ReleaseTexture(Rml::TextureHandle texture_handle) override;
	bool SupportsTextureWrapHint() override;

	void EnableScissorRegion(bool enable) override;
	void SetScissorRegion(Rml::Rectanglei region) override;

	void EnableClipMask(bool enable) override;
	void RenderToClipMask(Rml::ClipMaskOperation mask_operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;

	void SetTransform(const Rml::Matrix4f* transform) override;

	Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
	void ReleaseFilter(Rml::CompiledFilterHandle filter) override;

	Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
	void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
		Rml::TextureHandle texture) override;
	void ReleaseShader(Rml::CompiledShaderHandle shader) override;

	Rml::LayerHandle PushLayer() override;
	Rml::TextureHandle SaveLayerIntoTexture(Rml::TextureHandle texture, Rml::Vector2i texture_dimensions, Rml::Vector2i destination_offset) override;
	void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
		Rml::Span<const Rml::CompiledFilterHandle> filters) override;
	void PopLayer() override;

	// The values of the 'opacity' filters compiled since the last Reset (group opacity), in order.
	const Rml::Vector<float>& GetCompiledOpacities() const { return compiled_opacities; }
	// The shaders compiled since the last Reset (gradients of background layers), and the sources of the textures loaded.
	struct CompiledShaderRecord {
		Rml::String name;
		Rml::Dictionary parameters;
	};
	const Rml::Vector<CompiledShaderRecord>& GetCompiledShaders() const { return compiled_shaders; }
	const Rml::StringList& GetLoadedTextureSources() const { return loaded_texture_sources; }

	const Counters& GetCounters() const { return counters; }
	void ResetCounters();
	const Counters& GetCountersFromPreviousReset() const { return counters_from_previous_reset; }

	void ExpectCompileGeometry(Rml::Vector<Rml::Mesh> meshes);

	void Reset();

private:
	void VerifyMeshes();

	Counters counters = {};
	Counters counters_from_previous_reset = {};
	Rml::Vector<Rml::Mesh> meshes;
	bool meshes_set = false;
	Rml::Vector<float> compiled_opacities;
	Rml::Vector<CompiledShaderRecord> compiled_shaders;
	Rml::StringList loaded_texture_sources;
	int layer_depth = 0;
};
