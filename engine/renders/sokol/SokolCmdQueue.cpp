// ---------------------------------------
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Based on: https://github.com/oviano/sokol-multithread
// ---------------------------------------

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include "SokolCmdQueue.h"
#include "profiler/GpuMemory.h"
#include "SokolSystem.h"
#include "SokolPassPreview.h"
#include "SokolAsyncCreate.h"
#include "SokolPipelineCache.h"

using namespace jasperengine;

std::vector<SokolRenderCommand> SokolCmdQueue::m_commands[2];
int32_t SokolCmdQueue::m_pending_commands_index = 0;
int32_t SokolCmdQueue::m_commit_commands_index = 1;
std::vector<SokolRenderCleanup> SokolCmdQueue::m_cleanups;
std::mutex SokolCmdQueue::m_cleanups_mutex;
Semaphore SokolCmdQueue::m_update_semaphore;
Semaphore SokolCmdQueue::m_render_semaphore;
std::atomic<bool> SokolCmdQueue::m_flushing = false;
std::atomic<bool> SokolCmdQueue::m_commited = false;
std::mutex SokolCmdQueue::m_execute_mutex;
std::atomic<int32_t> SokolCmdQueue::m_frame_index = 0;
std::vector<SokolRenderCommand> SokolCmdQueue::m_backlog;
std::vector<SokolRenderCommand> SokolCmdQueue::m_run;
std::atomic<uint64_t> SokolCmdQueue::m_upload_budget = 32ull * 1024ull * 1024ull;
std::atomic<uint64_t> SokolCmdQueue::m_issued = 0;
std::atomic<uint64_t> SokolCmdQueue::m_completed = 0;
uint64_t SokolCmdQueue::m_backlog_end = 0;
std::atomic<size_t> SokolCmdQueue::m_deferred = 0;
std::atomic<uint64_t> SokolCmdQueue::m_last_frame_bytes = 0;

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

	// nothing committed and nothing waiting: no cost
	if (!m_commited && m_deferred.load(std::memory_order_relaxed) == 0)
	{
		return;
	}

	const bool commited = m_commited;
	if (commited && !m_flushing)
	{
		m_update_semaphore.acquire();
	}
	
	{
		std::scoped_lock<std::mutex> lock(m_execute_mutex);

		if (commited)
		{
			// the batch joins what waits, behind it: the committing thread goes on at once
			std::vector<SokolRenderCommand>& batch = m_commands[m_commit_commands_index];
			if (!batch.empty())
			{
				m_backlog_end = batch.back().seq;
			}
			if (m_backlog.empty())
			{
				m_backlog.swap(batch);
			}
			else
			{
				m_backlog.insert(m_backlog.end(), batch.begin(), batch.end());
			}
			batch.clear();
		}
		run_backlog(m_upload_budget.load(std::memory_order_relaxed));
	}

	process_cleanups(m_frame_index);

	if (commited)
	{
		m_commited = false;

		m_render_semaphore.release();
	}
}

void SokolCmdQueue::run_command(const SokolRenderCommand& command)
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
		// created off the render thread where the backend allows (spec S32)
		SokolAsyncCreate::initShader(command.make_shader.shader, command.make_shader.desc);
		break;
	case SokolRenderCommand::TYPE::MAKE_PIPELINE:
		SokolAsyncCreate::initPipeline(command.make_pipeline.pipeline, command.make_pipeline.desc);
		break;
	case SokolRenderCommand::TYPE::MAKE_VIEW:
		sg_init_view(command.make_view.view, command.make_view.desc);
		break;
	case SokolRenderCommand::TYPE::DESTROY_BUFFER:
		sg_uninit_buffer(command.destroy_buffer.buffer);
		SokolSystem::forgetAppliedBindings();
		break;
	case SokolRenderCommand::TYPE::DESTROY_IMAGE:
		sg_uninit_image(command.destroy_image.image);
		SokolSystem::forgetAppliedBindings();
		break;
	case SokolRenderCommand::TYPE::DESTROY_SAMPLER:
		sg_uninit_sampler(command.destroy_sampler.sampler);
		SokolSystem::forgetAppliedBindings();
		break;
	case SokolRenderCommand::TYPE::DESTROY_SHADER:
		// the whole destroy (a pending one's waits for its creation): no deferred dealloc
		SokolAsyncCreate::destroyShader(command.destroy_shader.shader);
		break;
	case SokolRenderCommand::TYPE::DESTROY_PIPELINE:
		SokolAsyncCreate::destroyPipeline(command.destroy_pipeline.pipeline);
		break;
	case SokolRenderCommand::TYPE::DESTROY_VIEW:
		sg_uninit_view(command.destroy_view.view);
		SokolSystem::forgetAppliedBindings();
		break;
	case SokolRenderCommand::TYPE::UPDATE_BUFFER:
		SokolSystem::updateBuffer(command.update_buffer.buffer, command.update_buffer.data);
		break;
	case SokolRenderCommand::TYPE::UPDATE_IMAGE:
		SokolSystem::updateImage(command.update_image.image, command.update_image.data);
		break;
	case SokolRenderCommand::TYPE::BEGIN_PASS:
		SokolSystem::beginPass(command.begin_pass.pass);
		break;
	case SokolRenderCommand::TYPE::END_PASS:
		SokolSystem::endPass();
		break;
	case SokolRenderCommand::TYPE::CUSTOM:
		command.custom.custom_cb(command.custom.custom_data);
		break;
	case SokolRenderCommand::TYPE::NOT_SET:
		break;
	}
}

void SokolCmdQueue::run_backlog(uint64_t budget)
{
	m_run.clear();
	const uint64_t bytes = select_commands(m_backlog, budget, m_run);
	for (const auto& command : m_run)
	{
		run_command(command);
	}
	m_last_frame_bytes.store(bytes, std::memory_order_relaxed);
	m_deferred.store(m_backlog.size(), std::memory_order_relaxed);
	// everything before the first one waiting has run
	m_completed.store(m_backlog.empty() ? m_backlog_end : m_backlog.front().seq - 1, std::memory_order_release);
}

void SokolCmdQueue::drain()
{
	std::scoped_lock<std::mutex> lock(m_execute_mutex);
	run_backlog(UINT64_MAX);
}

uint64_t SokolCmdQueue::upload_bytes(const SokolRenderCommand& command)
{
	uint64_t bytes = 0;
	switch (command.type)
	{
	case SokolRenderCommand::TYPE::MAKE_IMAGE:
		for (const sg_range& level : command.make_image.desc.data.mip_levels)
			bytes += level.size;
		break;
	case SokolRenderCommand::TYPE::UPDATE_IMAGE:
		for (const sg_range& level : command.update_image.data.mip_levels)
			bytes += level.size;
		break;
	case SokolRenderCommand::TYPE::MAKE_BUFFER:
		bytes = command.make_buffer.desc.data.ptr ? command.make_buffer.desc.data.size : 0;
		break;
	case SokolRenderCommand::TYPE::UPDATE_BUFFER:
		bytes = command.update_buffer.data.size;
		break;
	default:
		break;
	}
	return bytes;
}

uint64_t SokolCmdQueue::select_commands(std::vector<SokolRenderCommand>& backlog, uint64_t budget, std::vector<SokolRenderCommand>& run)
{
	if (backlog.empty())
		return 0;

	// handles a waiting command creates or touches: what refers to one waits too
	std::unordered_set<uint32_t> images, buffers, views;
	// handles updated this frame (sokol updates a resource once a frame)
	std::unordered_set<uint32_t> updatedImages, updatedBuffers;
	bool barrier = false;
	bool budgetFull = false;
	uint64_t spent = 0;
	size_t kept = 0;

	auto waits = [&](const SokolRenderCommand& command) -> bool {
		using T = SokolRenderCommand::TYPE;
		switch (command.type)
		{
		case T::MAKE_VIEW:
		{
			const sg_view_desc& desc = command.make_view.desc;
			return images.count(desc.texture.image.id) || images.count(desc.storage_image.image.id) ||
			       images.count(desc.color_attachment.image.id) || images.count(desc.resolve_attachment.image.id) ||
			       images.count(desc.depth_stencil_attachment.image.id) || buffers.count(desc.storage_buffer.buffer.id);
		}
		case T::UPDATE_IMAGE: return images.count(command.update_image.image.id) || updatedImages.count(command.update_image.image.id);
		case T::DESTROY_IMAGE: return images.count(command.destroy_image.image.id) > 0;
		case T::UPDATE_BUFFER: return buffers.count(command.update_buffer.buffer.id) || updatedBuffers.count(command.update_buffer.buffer.id);
		case T::DESTROY_BUFFER: return buffers.count(command.destroy_buffer.buffer.id) > 0;
		case T::DESTROY_VIEW: return views.count(command.destroy_view.view.id) > 0;
		default: return false;
		}
	};
	auto remember = [&](const SokolRenderCommand& command) {
		using T = SokolRenderCommand::TYPE;
		switch (command.type)
		{
		case T::MAKE_IMAGE: images.insert(command.make_image.image.id); break;
		case T::UPDATE_IMAGE: images.insert(command.update_image.image.id); break;
		case T::MAKE_BUFFER: buffers.insert(command.make_buffer.buffer.id); break;
		case T::UPDATE_BUFFER: buffers.insert(command.update_buffer.buffer.id); break;
		case T::MAKE_VIEW: views.insert(command.make_view.view.id); break;
		default: break;
		}
	};

	for (size_t i = 0; i < backlog.size(); i++)
	{
		SokolRenderCommand& command = backlog[i];
		using T = SokolRenderCommand::TYPE;
		const bool opaque = command.type == T::CUSTOM || command.type == T::BEGIN_PASS || command.type == T::END_PASS;
		bool wait = barrier || waits(command);
		uint64_t bytes = 0;
		if (!wait)
		{
			bytes = upload_bytes(command);
			// the frame's first upload runs whatever its size, alone when it is over the budget;
			// once one waits for the budget the later ones wait behind it, in order
			wait = bytes > 0 && (budgetFull || (spent > 0 && spent + bytes > budget));
			budgetFull = budgetFull || wait;
		}
		if (!wait && opaque && kept > 0)
		{
			// it would overtake a waiting command: it and everything after it wait
			barrier = true;
			wait = true;
		}
		if (wait)
		{
			remember(command);
			if (kept != i)
				backlog[kept] = command;
			kept++;
			continue;
		}
		spent += bytes;
		if (command.type == T::UPDATE_IMAGE) updatedImages.insert(command.update_image.image.id);
		if (command.type == T::UPDATE_BUFFER) updatedBuffers.insert(command.update_buffer.buffer.id);
		run.push_back(command);
	}
	backlog.resize(kept);
	return spent;
}

void SokolCmdQueue::set_upload_budget(uint64_t bytes)
{
	m_upload_budget.store(std::max<uint64_t>(bytes, 1), std::memory_order_relaxed);
}

uint64_t SokolCmdQueue::get_upload_budget()
{
	return m_upload_budget.load(std::memory_order_relaxed);
}

uint64_t SokolCmdQueue::get_upload_token()
{
	return m_issued.load(std::memory_order_acquire);
}

bool SokolCmdQueue::is_token_complete(uint64_t token)
{
	return m_completed.load(std::memory_order_acquire) >= token;
}

size_t SokolCmdQueue::get_deferred_count()
{
	return m_deferred.load(std::memory_order_relaxed);
}

uint64_t SokolCmdQueue::get_last_frame_upload_bytes()
{
	return m_last_frame_bytes.load(std::memory_order_relaxed);
}

SokolRenderCommand& SokolCmdQueue::push_command(SokolRenderCommand::TYPE::ENUM type)
{
	SokolRenderCommand& command = m_commands[m_pending_commands_index].emplace_back(type);
	command.seq = m_issued.fetch_add(1, std::memory_order_acq_rel) + 1;
	return command;
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
					SokolSystem::forgetAppliedBindings();
					break;
				case SokolRenderCommand::TYPE::DESTROY_IMAGE:
					sg_uninit_image(command.destroy_image.image);
					SokolSystem::forgetAppliedBindings();
					break;
				case SokolRenderCommand::TYPE::DESTROY_SAMPLER:
					sg_uninit_sampler(command.destroy_sampler.sampler);
					SokolSystem::forgetAppliedBindings();
					break;
				case SokolRenderCommand::TYPE::DESTROY_SHADER:
					SokolAsyncCreate::destroyShader(command.destroy_shader.shader);
					break;
				case SokolRenderCommand::TYPE::DESTROY_PIPELINE:
					SokolAsyncCreate::destroyPipeline(command.destroy_pipeline.pipeline);
					break;
				case SokolRenderCommand::TYPE::DESTROY_VIEW:
					sg_uninit_view(command.destroy_view.view);
					SokolSystem::forgetAppliedBindings();
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
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::MAKE_BUFFER);

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
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::MAKE_IMAGE);

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
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::MAKE_SAMPLER);

	command.make_sampler.desc = desc;

	command.make_sampler.sampler = sg_alloc_sampler();

	return command.make_sampler.sampler;
}

sg_shader SokolCmdQueue::add_command_make_shader(const sg_shader_desc& desc)
{
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::MAKE_SHADER);

	command.make_shader.desc = desc;

	command.make_shader.shader = sg_alloc_shader();
	
	return command.make_shader.shader;
}

sg_pipeline SokolCmdQueue::add_command_make_pipeline(const sg_pipeline_desc& desc)
{
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::MAKE_PIPELINE);

	command.make_pipeline.desc = desc;

	command.make_pipeline.pipeline = sg_alloc_pipeline();
	
	return command.make_pipeline.pipeline;
}

sg_view SokolCmdQueue::add_command_make_view(const sg_view_desc& desc)
{
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::MAKE_VIEW);

	command.make_view.desc = desc;

	command.make_view.view = sg_alloc_view();

	return command.make_view.view;
}

void SokolCmdQueue::add_command_destroy_buffer(sg_buffer buffer)
{
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::DESTROY_BUFFER);

	command.destroy_buffer.buffer = buffer;

	schedule_cleanup(dealloc_buffer_cb, (void*)(uintptr_t)command.destroy_buffer.buffer.id);
}

void SokolCmdQueue::add_command_destroy_image(sg_image image)
{
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::DESTROY_IMAGE);

	command.destroy_image.image = image;

	schedule_cleanup(dealloc_image_cb, (void*)(uintptr_t)command.destroy_image.image.id);
}

void SokolCmdQueue::add_command_destroy_sampler(sg_sampler sampler)
{
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::DESTROY_SAMPLER);

	command.destroy_sampler.sampler = sampler;

	schedule_cleanup(dealloc_sampler_cb, (void*)(uintptr_t)command.destroy_sampler.sampler.id);
}

void SokolCmdQueue::add_command_destroy_shader(sg_shader shader)
{
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::DESTROY_SHADER);

	command.destroy_shader.shader = shader;
}

void SokolCmdQueue::add_command_destroy_pipeline(sg_pipeline pipeline)
{
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::DESTROY_PIPELINE);

	command.destroy_pipeline.pipeline = pipeline;
}

void SokolCmdQueue::add_command_destroy_view(sg_view view)
{
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::DESTROY_VIEW);

	command.destroy_view.view = view;

	schedule_cleanup(dealloc_view_cb, (void*)(uintptr_t)command.destroy_view.view.id);
}

void SokolCmdQueue::add_command_update_buffer(sg_buffer buffer, const sg_range& data)
{
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::UPDATE_BUFFER);

	command.update_buffer.buffer = buffer;
	command.update_buffer.data = data;
}

void SokolCmdQueue::add_command_update_image(sg_image image, const sg_image_data& data)
{
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::UPDATE_IMAGE);

	command.update_image.image = image;
	command.update_image.data = data;
}

void SokolCmdQueue::add_command_begin_pass(const sg_pass& pass)
{
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::BEGIN_PASS);

	command.begin_pass.pass = pass;
}

void SokolCmdQueue::add_command_end_pass()
{
	push_command(SokolRenderCommand::TYPE::END_PASS);
}

void SokolCmdQueue::add_command_custom(void (*custom_cb)(void* custom_data), void* custom_data)
{
	SokolRenderCommand& command = push_command(SokolRenderCommand::TYPE::CUSTOM);

	command.custom.custom_cb = custom_cb;
	command.custom.custom_data = custom_data;
}

void SokolCmdQueue::schedule_cleanup(void (*cleanup_cb)(void* cleanup_data), void* cleanup_data, int32_t number_of_frames_to_defer)
{
	std::scoped_lock<std::mutex> lock(m_cleanups_mutex);

	SokolRenderCleanup& cleanup = m_cleanups.emplace_back(cleanup_cb, cleanup_data);
	
	cleanup.frame_index = m_frame_index + 1 + number_of_frames_to_defer;
	cleanup.token = m_issued.load(std::memory_order_acquire);
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
	// the ones due, taken out: a thread queues its cleanups while this one runs them (a thread
	// under Engine::AsyncThreadScope goes on after its commit), and one may queue another
	std::vector<SokolRenderCleanup> due;
	{
		std::scoped_lock<std::mutex> lock(m_cleanups_mutex);

		// due, and after every command queued before it (a deferred upload reads its data, a
		// deferred destroy comes before its dealloc)
		const uint64_t completed = m_completed.load(std::memory_order_acquire);
		const auto first_kept = std::stable_partition(m_cleanups.begin(), m_cleanups.end(), [frame_index, completed](const auto& cleanup) {
			return frame_index < 0 || (cleanup.frame_index < frame_index && cleanup.token <= completed);
		});
		due.assign(m_cleanups.begin(), first_kept);
		m_cleanups.erase(m_cleanups.begin(), first_kept);
	}

	for (const auto& cleanup : due)
	{
		cleanup.cleanup_cb(cleanup.cleanup_data);
	}
}
