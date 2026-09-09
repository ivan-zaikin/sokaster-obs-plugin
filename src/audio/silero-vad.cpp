/*
sokaster — an AI co-host inside OBS Studio
Copyright (C) 2026 sokaster <tnixton@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "silero-vad.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <onnxruntime_cxx_api.h>

#include <cstring>

namespace sokaster {
namespace {

constexpr const char *kModelFile = "silero_vad.onnx";

} // namespace

/* ONNX Runtime types stay behind this so its headers reach exactly one
 * translation unit. */
struct SileroVad::Impl {
	Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "sokaster-vad"};
	Ort::SessionOptions options;
	Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
	std::unique_ptr<Ort::Session> session;

	/* The names are read from the model rather than hard-coded: they are
	 * cheap to fetch once and one less thing to break on a model update. */
	std::vector<Ort::AllocatedStringPtr> output_name_storage;
	std::vector<const char *> output_names;
};

SileroVad::SileroVad() = default;
SileroVad::~SileroVad() = default;

std::string silero_model_path()
{
	char *path = obs_module_file(kModelFile);
	if (!path)
		return {};

	std::string result = path;
	bfree(path);
	return result;
}

bool SileroVad::load(const std::string &model_path)
{
	if (model_path.empty()) {
		obs_log(LOG_ERROR, "vad: %s is missing from the plugin's data directory", kModelFile);
		return false;
	}

	try {
		auto impl = std::make_unique<Impl>();

		/* One thread each way. The model is tiny, it runs once per 32 ms of
		 * audio, and an ONNX thread pool inside OBS would be a pool fighting
		 * the encoder for cores. */
		impl->options.SetIntraOpNumThreads(1);
		impl->options.SetInterOpNumThreads(1);
		impl->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#ifdef _WIN32
		wchar_t *wide = nullptr;
		if (os_utf8_to_wcs_ptr(model_path.c_str(), 0, &wide) == 0 || !wide) {
			obs_log(LOG_ERROR, "vad: cannot convert model path");
			return false;
		}
		impl->session = std::make_unique<Ort::Session>(impl->env, wide, impl->options);
		bfree(wide);
#else
		impl->session = std::make_unique<Ort::Session>(impl->env, model_path.c_str(), impl->options);
#endif

		Ort::AllocatorWithDefaultOptions allocator;
		const size_t outputs = impl->session->GetOutputCount();
		impl->output_name_storage.reserve(outputs);
		impl->output_names.reserve(outputs);
		for (size_t i = 0; i < outputs; ++i) {
			impl->output_name_storage.push_back(impl->session->GetOutputNameAllocated(i, allocator));
			impl->output_names.push_back(impl->output_name_storage.back().get());
		}

		impl_ = std::move(impl);
	} catch (const Ort::Exception &e) {
		obs_log(LOG_ERROR, "vad: model load failed: %s", e.what());
		return false;
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "vad: model load failed: %s", e.what());
		return false;
	}

	reset();
	obs_log(LOG_INFO, "vad: silero v5 ready, %d-sample window", kFrameSize);
	return true;
}

void SileroVad::reset()
{
	state_.fill(0.0f);
	context_.fill(0.0f);
}

float SileroVad::predict(const float *frame)
{
	if (!impl_ || !frame)
		return 0.0f;

	/* 576 = the 64 samples we kept from last time, then this frame. */
	std::memcpy(input_.data(), context_.data(), kContextSize * sizeof(float));
	std::memcpy(input_.data() + kContextSize, frame, kFrameSize * sizeof(float));

	float probability = 0.0f;
	const uint64_t started = os_gettime_ns();

	try {
		const int64_t input_shape[2] = {1, kInputSize};
		const int64_t state_shape[3] = {2, 1, 128};
		int64_t sample_rate = kSampleRate;

		Ort::Value inputs[3] = {
			Ort::Value::CreateTensor<float>(impl_->memory_info, input_.data(), input_.size(), input_shape,
							2),
			Ort::Value::CreateTensor<float>(impl_->memory_info, state_.data(), state_.size(), state_shape,
							3),
			/* Zero dimensions: the model wants a scalar, not a [1] tensor. */
			Ort::Value::CreateTensor<int64_t>(impl_->memory_info, &sample_rate, 1, nullptr, 0),
		};

		static const char *const input_names[3] = {"input", "state", "sr"};

		auto results = impl_->session->Run(Ort::RunOptions{nullptr}, input_names, inputs, 3,
						   impl_->output_names.data(), impl_->output_names.size());

		/* Told apart by size rather than by name, the way the desktop client
		 * did: the probability is the one scalar, the state is the big one. */
		for (auto &value : results) {
			if (!value.IsTensor())
				continue;

			const size_t count = value.GetTensorTypeAndShapeInfo().GetElementCount();
			const float *data = value.GetTensorData<float>();

			if (count == 1)
				probability = data[0];
			else if (count == kStateSize)
				std::memcpy(state_.data(), data, kStateSize * sizeof(float));
		}
	} catch (const Ort::Exception &e) {
		obs_log(LOG_WARNING, "vad: inference failed: %s", e.what());
		return 0.0f;
	}

	inference_ns_ += os_gettime_ns() - started;
	++predictions_;

	/* The tail of this frame is the next frame's context. */
	std::memcpy(context_.data(), frame + (kFrameSize - kContextSize), kContextSize * sizeof(float));

	return probability;
}

} // namespace sokaster
