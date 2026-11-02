// ---------------------------------------
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Based on: https://github.com/oviano/sokol-multithread
// ---------------------------------------

#ifndef SokolCmdQueue_H
#define SokolCmdQueue_H

// ----------------------------------------------------------------------------------------------------

#include <vector>
#include <mutex>
#include <atomic>

#include "sokol_gfx.h"

#include "util/ThreadUtils.h"

// ----------------------------------------------------------------------------------------------------

namespace jasperengine {

	struct SokolRenderCommand
	{
		// types
		struct TYPE
		{
			enum ENUM
			{
				NOT_SET = 0,
				
				MAKE_BUFFER,
				MAKE_IMAGE,
				MAKE_SAMPLER,
				MAKE_SHADER,
				MAKE_PIPELINE,
				MAKE_VIEW,
				DESTROY_BUFFER,
				DESTROY_IMAGE,
				DESTROY_SAMPLER,
				DESTROY_SHADER,
				DESTROY_PIPELINE,
				DESTROY_VIEW,
				UPDATE_BUFFER,
				UPDATE_IMAGE,

				BEGIN_PASS,
				END_PASS,
				
				CUSTOM
			};
		};
		
		SokolRenderCommand() {}
		SokolRenderCommand(TYPE::ENUM _type) : type(_type) {}

		TYPE::ENUM type = TYPE::NOT_SET;
		// MAKE_BUFFER / MAKE_IMAGE: the JE_GPU_ALLOC_SCOPE the creation was queued in (the VRAM
		// tracker sees the creation when it runs, on the main thread)
		uint8_t allocCategory = 0xFF;
		uint32_t allocOwner = 0;
		// and its label, copied: the caller's string is gone by the time the command runs
		char allocLabel[40] = {};

		union
		{
			struct
			{
				sg_buffer_desc desc;
				sg_buffer buffer;
			} make_buffer;
			
			struct
			{
				sg_image_desc desc;
				sg_image image;
			} make_image;

			struct
			{
				sg_sampler_desc desc;
				sg_sampler sampler;
			} make_sampler;
			
			struct
			{
				sg_shader_desc desc;
				sg_shader shader;
			} make_shader;
			
			struct
			{
				sg_pipeline_desc desc;
				sg_pipeline pipeline;
			} make_pipeline;
			
			struct
			{
				sg_view_desc desc;
				sg_view view;
			} make_view;
			
			struct
			{
				sg_buffer buffer;
			} destroy_buffer;
			
			struct
			{
				sg_image image;
			} destroy_image;
			
			struct
			{
				sg_sampler sampler;
			} destroy_sampler;
			
			struct
			{
				sg_shader shader;
			} destroy_shader;
			
			struct
			{
				sg_pipeline pipeline;
			} destroy_pipeline;
			
			struct
			{
				sg_view view;
			} destroy_view;
			
			struct
			{
				sg_buffer buffer;
				sg_range data;
			} update_buffer;
			
			struct
			{
				sg_image image;
				sg_image_data data;
			} update_image;
			
			struct
			{
				void (*custom_cb)(void* custom_data);
				void* custom_data;
			} custom;

			struct
			{
				sg_pass pass;
			} begin_pass;

		};
	};

// ----------------------------------------------------------------------------------------------------

	struct SokolRenderCleanup
	{
		SokolRenderCleanup() {}
		SokolRenderCleanup(void (*_cleanup_cb)(void* cleanup_data), void* _cleanup_data) : cleanup_cb(_cleanup_cb), cleanup_data(_cleanup_data) {}
		
		void (*cleanup_cb)(void* cleanup_data) = nullptr;
		void* cleanup_data = nullptr;
		int32_t frame_index = 0;
	};

// ----------------------------------------------------------------------------------------------------

	class SokolCmdQueue
	{
	public:
		static void start();
		static void finish();

		// render thread functions
		static void execute_commands();
		static void wait_for_flush();

		// update thread functions
		static sg_buffer add_command_make_buffer(const sg_buffer_desc& desc);
		static sg_image add_command_make_image(const sg_image_desc& desc);
		static sg_sampler add_command_make_sampler(const sg_sampler_desc& desc);
		static sg_shader add_command_make_shader(const sg_shader_desc& desc);
		static sg_pipeline add_command_make_pipeline(const sg_pipeline_desc& desc);
		static sg_view add_command_make_view(const sg_view_desc& desc);

		static void add_command_destroy_buffer(sg_buffer buffer);
		static void add_command_destroy_image(sg_image image);
		static void add_command_destroy_sampler(sg_sampler sampler);
		static void add_command_destroy_shader(sg_shader shader);
		static void add_command_destroy_pipeline(sg_pipeline pipeline);
		static void add_command_destroy_view(sg_view view);
		
		static void add_command_update_buffer(sg_buffer buffer, const sg_range& data);
		static void add_command_update_image(sg_image image, const sg_image_data& data);
		
		static void add_command_begin_pass(const sg_pass& pass);
		static void add_command_end_pass();

		static void add_command_custom(void (*custom_cb)(void* custom_data), void* custom_data);
		
		static void schedule_cleanup(void (*cleanup_cb)(void* cleanup_data), void* cleanup_data, int32_t number_of_frames_to_defer = 0);

		static void commit_commands();
		static void flush_commands();
		
	private:
		static void process_cleanups(int32_t frame_index);

		static void dealloc_buffer_cb(void* cleanup_data) { sg_dealloc_buffer({(uint32_t)(uintptr_t)cleanup_data}); }
		static void dealloc_image_cb(void* cleanup_data) { sg_dealloc_image({(uint32_t)(uintptr_t)cleanup_data}); }
		static void dealloc_sampler_cb(void* cleanup_data) { sg_dealloc_sampler({(uint32_t)(uintptr_t)cleanup_data}); }
		static void dealloc_view_cb(void* cleanup_data) { sg_dealloc_view({(uint32_t)(uintptr_t)cleanup_data}); }

		static std::vector<SokolRenderCommand> m_commands[2];
		static int32_t m_pending_commands_index;
		static int32_t m_commit_commands_index;
		static std::vector<SokolRenderCleanup> m_cleanups;
		static std::mutex m_cleanups_mutex;

		static Semaphore m_update_semaphore;
		static Semaphore m_render_semaphore;
		static std::atomic<bool> m_flushing;
		static std::atomic<bool> m_commited;
		static std::mutex m_execute_mutex;
		static std::atomic<int32_t> m_frame_index;
	};

// ----------------------------------------------------------------------------------------------------

}

#endif // SokolCmdQueue_H
