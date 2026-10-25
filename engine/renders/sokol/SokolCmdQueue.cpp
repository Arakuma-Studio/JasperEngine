// ---------------------------------------
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Based on: https://github.com/oviano/sokol-multithread
// ---------------------------------------

#include <algorithm>
#include <cstdio>

#include "SokolCmdQueue.h"
#include "profiler/GpuMemory.h"
#include "SokolPassPreview.h"
#include "SokolPipelineCache.h"

using namespace jasperengine;

std::vector<SokolRenderCommand> SokolCmdQueue::m_commands[2];
int32_t SokolCmdQueue::m_pending_commands_index = 0;
int32_t SokolCmdQueue::m_commit_commands_index = 1;
std::vector<SokolRenderCleanup> SokolCmdQueue::m_cleanups;
Semaphore SokolCmdQueue::m_update_semaphore;
Semaphore SokolCmdQueue::m_render_semaphore;
std::atomic<bool> SokolCmdQueue::m_flushing = false;
std::atomic<bool> SokolCmdQueue::m_commited = false;
std::mutex SokolCmdQueue::m_execute_mutex;
int32_t SokolCmdQueue::m_frame_index = 0;

constexpr int32_t INITIAL_NUMBER_OF_COMMANDS = 256;
constexpr int32_t INITIAL_NUMBER_OF_CLEANUPS = 64;

void SokolCmdQueue::start(){
	for (int32_t i = 0; i < 2; i ++)
	{
		m_commands[i].reserve(INITIAL_NUMBER_OF_COMMANDS);
	}

	m_cleanups.reserve(INITIAL_NUMBER_OF_CLEANUPS);
	
	m_render_semaphore.release();
}

void SokolCmdQueue::finish(){
	process_cleanups(-1);
}

void SokolCmdQueue::execute_commands()
{
	m_frame_index ++;

	if (!m_commited)
	{
		return;
	}

	if (!m_flushing)
	{
		m_update_semaphore.acquire();
	}
	
	{
		std::scoped_lock<std::mutex> lock(m_execute_mutex);

		for (const auto& command : m_commands[m_commit_commands_index])
		{
			switch (command.type)
			{
			case SokolRenderCommand::TYPE::MAKE_BUFFER:
			{
#if JASPERENGINE_PROFILER
				gpumem::Scope allocScope(gpumem::ScopeState{command.allocCategory, command.allocOwner});
#endif
				sg_buffer_desc desc = command.make_buffer.desc;
				desc.label = command.allocLabel[0] ? command.allocLabel : nullptr;
				sg_init_buffer(command.make_buffer.buffer, &desc);
				break;
			}
			case SokolRenderCommand::TYPE::MAKE_IMAGE:
			{
#if JASPERENGINE_PROFILER
				gpumem::Scope allocScope(gpumem::ScopeState{command.allocCategory, command.allocOwner});
#endif
				sg_image_desc desc = command.make_image.desc;
				desc.label = command.allocLabel[0] ? command.allocLabel : nullptr;
				sg_init_image(command.make_image.image, &desc);
				break;
			}
			case SokolRenderCommand::TYPE::MAKE_SAMPLER:
				sg_init_sampler(command.make_sampler.sampler, command.make_sampler.desc);
				break;
			case SokolRenderCommand::TYPE::MAKE_SHADER:
				sg_init_shader(command.make_shader.shader, command.make_shader.desc);
				break;
			case SokolRenderCommand::TYPE::MAKE_PIPELINE:
				sg_init_pipeline(command.make_pipeline.pipeline, command.make_pipeline.desc);
				break;
			case SokolRenderCommand::TYPE::MAKE_VIEW:
				sg_init_view(command.make_view.view, command.make_view.desc);
				break;
			case SokolRenderCommand::TYPE::DESTROY_BUFFER:
				sg_uninit_buffer(command.destroy_buffer.buffer);
				break;
			case SokolRenderCommand::TYPE::DESTROY_IMAGE:
				sg_uninit_image(command.destroy_image.image);
				break;
			case SokolRenderCommand::TYPE::DESTROY_SAMPLER:
				sg_uninit_sampler(command.destroy_sampler.sampler);
				break;
			case SokolRenderCommand::TYPE::DESTROY_SHADER:
				sg_uninit_shader(command.destroy_shader.shader);
				break;
			case SokolRenderCommand::TYPE::DESTROY_PIPELINE:
				sg_uninit_pipeline(command.destroy_pipeline.pipeline);
				break;
			case SokolRenderCommand::TYPE::DESTROY_VIEW:
				sg_uninit_view(command.destroy_view.view);
				break;
			case SokolRenderCommand::TYPE::UPDATE_BUFFER:
				sg_update_buffer(command.update_buffer.buffer, command.update_buffer.data);
				break;
			case SokolRenderCommand::TYPE::UPDATE_IMAGE:
				sg_update_image(command.update_image.image, command.update_image.data);
				break;
			case SokolRenderCommand::TYPE::BEGIN_PASS:
				sg_begin_pass(command.begin_pass.pass);
				SokolPipelineCache::invalidateApplied();
				break;
			case SokolRenderCommand::TYPE::END_PASS:
				sg_end_pass();
				SokolPassPreview::afterEndPass();
				break;
			case SokolRenderCommand::TYPE::CUSTOM:
				command.custom.custom_cb(command.custom.custom_data);
				break;
			case SokolRenderCommand::TYPE::NOT_SET:
				break;
			}
		}
	}

	process_cleanups(m_frame_index);

	m_commited = false;

	m_render_semaphore.release();
}

void SokolCmdQueue::wait_for_flush()
{
	bool finished_flushing = false;

	while (!finished_flushing)
	{
		if (!m_flushing)
		{
			m_update_semaphore.acquire();
		}
		
		{
			std::scoped_lock<std::mutex> lock(m_execute_mutex);
			
			for (const auto& command : m_commands[m_commit_commands_index])
			{
				switch (command.type)
				{
				case SokolRenderCommand::TYPE::DESTROY_BUFFER:
					sg_uninit_buffer(command.destroy_buffer.buffer);
					break;
				case SokolRenderCommand::TYPE::DESTROY_IMAGE:
					sg_uninit_image(command.destroy_image.image);
					break;
				case SokolRenderCommand::TYPE::DESTROY_SAMPLER:
					sg_uninit_sampler(command.destroy_sampler.sampler);
					break;
				case SokolRenderCommand::TYPE::DESTROY_SHADER:
					sg_uninit_shader(command.destroy_shader.shader);
					break;
				case SokolRenderCommand::TYPE::DESTROY_PIPELINE:
					sg_uninit_pipeline(command.destroy_pipeline.pipeline);
					break;
				case SokolRenderCommand::TYPE::DESTROY_VIEW:
					sg_uninit_view(command.destroy_view.view);
					break;
				default:
					break;
				}
			}
		}
		
		finished_flushing = m_flushing;

		m_render_semaphore.release();
	}
}

sg_buffer SokolCmdQueue::add_command_make_buffer(const sg_buffer_desc& desc)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::MAKE_BUFFER);

	command.make_buffer.desc = desc;
	if (desc.label)
		std::snprintf(command.allocLabel, sizeof(command.allocLabel), "%s", desc.label);
#if JASPERENGINE_PROFILER
	const gpumem::ScopeState scope = gpumem::currentScope();
	command.allocCategory = scope.category;
	command.allocOwner = scope.owner;
#endif
	
	command.make_buffer.buffer = sg_alloc_buffer();
	
	return command.make_buffer.buffer;
}

sg_image SokolCmdQueue::add_command_make_image(const sg_image_desc& desc)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::MAKE_IMAGE);

	command.make_image.desc = desc;
	if (desc.label)
		std::snprintf(command.allocLabel, sizeof(command.allocLabel), "%s", desc.label);
#if JASPERENGINE_PROFILER
	const gpumem::ScopeState scope = gpumem::currentScope();
	command.allocCategory = scope.category;
	command.allocOwner = scope.owner;
#endif

	command.make_image.image = sg_alloc_image();
	
	return command.make_image.image;
}

sg_sampler SokolCmdQueue::add_command_make_sampler(const sg_sampler_desc& desc)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::MAKE_SAMPLER);

	command.make_sampler.desc = desc;

	command.make_sampler.sampler = sg_alloc_sampler();

	return command.make_sampler.sampler;
}

sg_shader SokolCmdQueue::add_command_make_shader(const sg_shader_desc& desc)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::MAKE_SHADER);

	command.make_shader.desc = desc;

	command.make_shader.shader = sg_alloc_shader();
	
	return command.make_shader.shader;
}

sg_pipeline SokolCmdQueue::add_command_make_pipeline(const sg_pipeline_desc& desc)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::MAKE_PIPELINE);

	command.make_pipeline.desc = desc;

	command.make_pipeline.pipeline = sg_alloc_pipeline();
	
	return command.make_pipeline.pipeline;
}

sg_view SokolCmdQueue::add_command_make_view(const sg_view_desc& desc)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::MAKE_VIEW);

	command.make_view.desc = desc;

	command.make_view.view = sg_alloc_view();

	return command.make_view.view;
}

void SokolCmdQueue::add_command_destroy_buffer(sg_buffer buffer)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::DESTROY_BUFFER);

	command.destroy_buffer.buffer = buffer;

	schedule_cleanup(dealloc_buffer_cb, (void*)(uintptr_t)command.destroy_buffer.buffer.id);
}

void SokolCmdQueue::add_command_destroy_image(sg_image image)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::DESTROY_IMAGE);

	command.destroy_image.image = image;

	schedule_cleanup(dealloc_image_cb, (void*)(uintptr_t)command.destroy_image.image.id);
}

void SokolCmdQueue::add_command_destroy_sampler(sg_sampler sampler)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::DESTROY_SAMPLER);

	command.destroy_sampler.sampler = sampler;

	schedule_cleanup(dealloc_sampler_cb, (void*)(uintptr_t)command.destroy_sampler.sampler.id);
}

void SokolCmdQueue::add_command_destroy_shader(sg_shader shader)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::DESTROY_SHADER);

	command.destroy_shader.shader = shader;

	schedule_cleanup(dealloc_shader_cb, (void*)(uintptr_t)command.destroy_shader.shader.id);
}

void SokolCmdQueue::add_command_destroy_pipeline(sg_pipeline pipeline)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::DESTROY_PIPELINE);

	command.destroy_pipeline.pipeline = pipeline;

	schedule_cleanup(dealloc_pipeline_cb, (void*)(uintptr_t)command.destroy_pipeline.pipeline.id);
}

void SokolCmdQueue::add_command_destroy_view(sg_view view)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::DESTROY_VIEW);

	command.destroy_view.view = view;

	schedule_cleanup(dealloc_view_cb, (void*)(uintptr_t)command.destroy_view.view.id);
}

void SokolCmdQueue::add_command_update_buffer(sg_buffer buffer, const sg_range& data)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::UPDATE_BUFFER);

	command.update_buffer.buffer = buffer;
	command.update_buffer.data = data;
}

void SokolCmdQueue::add_command_update_image(sg_image image, const sg_image_data& data)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::UPDATE_IMAGE);

	command.update_image.image = image;
	command.update_image.data = data;
}

void SokolCmdQueue::add_command_begin_pass(const sg_pass& pass)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::BEGIN_PASS);

	command.begin_pass.pass = pass;
}

void SokolCmdQueue::add_command_end_pass()
{
	m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::END_PASS);
}

void SokolCmdQueue::add_command_custom(void (*custom_cb)(void* custom_data), void* custom_data)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(SokolRenderCommand::TYPE::CUSTOM);

	command.custom.custom_cb = custom_cb;
	command.custom.custom_data = custom_data;
}

void SokolCmdQueue::schedule_cleanup(void (*cleanup_cb)(void* cleanup_data), void* cleanup_data, int32_t number_of_frames_to_defer)
{
	SokolRenderCleanup& cleanup = m_cleanups.emplace_back(cleanup_cb, cleanup_data);
	
	cleanup.frame_index = m_frame_index + 1 + number_of_frames_to_defer;
}

void SokolCmdQueue::commit_commands()
{
	m_render_semaphore.acquire();
	
	m_commands[m_commit_commands_index].resize(0);
	
	std::swap(m_pending_commands_index, m_commit_commands_index);

	m_commited = true;

	m_update_semaphore.release();
}

void SokolCmdQueue::flush_commands()
{
	m_render_semaphore.acquire();
	
	m_commands[m_commit_commands_index].resize(0);
	
	std::swap(m_pending_commands_index, m_commit_commands_index);

	m_flushing = true;

	m_update_semaphore.release();
}

void SokolCmdQueue::process_cleanups(int32_t frame_index)
{
	for (auto& cleanup : m_cleanups)
	{
		if ((cleanup.frame_index < frame_index || frame_index < 0) && cleanup.cleanup_cb)
		{
			cleanup.cleanup_cb(cleanup.cleanup_data);
			
			cleanup.cleanup_cb = nullptr;
		}
	}
	
	m_cleanups.erase(std::remove_if(m_cleanups.begin(), m_cleanups.end(), [](const auto& cleanup) { return !cleanup.cleanup_cb; }), m_cleanups.end());
}
