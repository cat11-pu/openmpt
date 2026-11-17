/*
 * module_property_runner.cpp
 * --------------------------
 * Purpose: Isolated libopenmpt property-test and differential-render runner
 *
 * This program is intentionally small and uses only the public C API so the
 * same source can be linked against the sanitizer build and an independent
 * reference libopenmpt build.
 */

#include "libopenmpt/libopenmpt.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kGuardSamples = 64;
constexpr std::uint16_t kGuardValue = 0xA55A;

struct CallbackStats
{
	std::size_t log_events = 0;
	std::size_t error_events = 0;
};

void LogFunc(const char * /*message*/, void *user)
{
	auto *stats = static_cast<CallbackStats *>(user);
	if(stats)
	{
		stats->log_events++;
	}
}

int ErrorFunc(int /*error*/, void *user)
{
	auto *stats = static_cast<CallbackStats *>(user);
	if(stats)
	{
		stats->error_events++;
	}
	return OPENMPT_ERROR_FUNC_RESULT_LOG;
}

bool ReadFile(const std::string &path, std::vector<std::uint8_t> &data)
{
	std::ifstream file(path, std::ios::binary);
	if(!file)
	{
		return false;
	}
	file.seekg(0, std::ios::end);
	const auto length = file.tellg();
	if(length < 0)
	{
		return false;
	}
	file.seekg(0, std::ios::beg);
	data.resize(static_cast<std::size_t>(length));
	if(!data.empty())
	{
		file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
	}
	return file.good() || file.eof();
}

bool WriteFile(const std::string &path, const std::vector<std::int16_t> &data)
{
	std::ofstream file(path, std::ios::binary);
	if(!file)
	{
		return false;
	}
	if(!data.empty())
	{
		file.write(
			reinterpret_cast<const char *>(data.data()),
			static_cast<std::streamsize>(data.size() * sizeof(std::int16_t)));
	}
	return file.good();
}

bool GuardsIntact(const std::vector<std::uint16_t> &storage)
{
	for(std::size_t i = 0; i < kGuardSamples; ++i)
	{
		if(storage[i] != kGuardValue)
		{
			return false;
		}
		if(storage[storage.size() - 1 - i] != kGuardValue)
		{
			return false;
		}
	}
	return true;
}

std::vector<std::uint16_t> MakeGuardedBuffer(std::size_t payload_samples)
{
	std::vector<std::uint16_t> storage(payload_samples + 2 * kGuardSamples, kGuardValue);
	return storage;
}

std::int16_t *Payload(std::vector<std::uint16_t> &storage)
{
	return reinterpret_cast<std::int16_t *>(storage.data() + kGuardSamples);
}

void ApplyDeterministicRenderSettings(openmpt_module *mod)
{
	openmpt_module_set_render_param(mod, OPENMPT_MODULE_RENDER_MASTERGAIN_MILLIBEL, 0);
	openmpt_module_set_render_param(mod, OPENMPT_MODULE_RENDER_STEREOSEPARATION_PERCENT, 100);
	openmpt_module_set_render_param(mod, OPENMPT_MODULE_RENDER_INTERPOLATIONFILTER_LENGTH, 1);
	openmpt_module_set_render_param(mod, OPENMPT_MODULE_RENDER_VOLUMERAMPING_STRENGTH, 0);
	openmpt_module_set_repeat_count(mod, 0);
}

const openmpt_module_initial_ctl kCtls[] = {
	{"dither", "0"},
	{"play.at_end", "stop"},
	{"seek.sync_samples", "0"},
	{"render.resampler.emulate_amiga", "0"},
	{nullptr, nullptr},
};

openmpt_module *CreateModule(
	const std::vector<std::uint8_t> &data,
	CallbackStats &stats,
	int &error,
	const char *&error_message)
{
	error = OPENMPT_ERROR_OK;
	error_message = nullptr;
	openmpt_module *mod = openmpt_module_create_from_memory2(
		data.empty() ? nullptr : data.data(),
		data.size(),
		LogFunc,
		&stats,
		ErrorFunc,
		&stats,
		&error,
		&error_message,
		kCtls);
	if(mod)
	{
		openmpt_module_set_log_func(mod, LogFunc, &stats);
		openmpt_module_set_error_func(mod, ErrorFunc, &stats);
		ApplyDeterministicRenderSettings(mod);
	}
	return mod;
}

void FreeLibString(const char *value)
{
	if(value)
	{
		openmpt_free_string(value);
	}
}

void TouchMetadata(openmpt_module *mod)
{
	const char *keys_raw = openmpt_module_get_metadata_keys(mod);
	if(keys_raw)
	{
		const std::string keys(keys_raw);
		openmpt_free_string(keys_raw);
		std::size_t begin = 0;
		while(begin <= keys.size())
		{
			const std::size_t end = keys.find(';', begin);
			const std::string key = keys.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
			if(!key.empty())
			{
				FreeLibString(openmpt_module_get_metadata(mod, key.c_str()));
			}
			if(end == std::string::npos)
			{
				break;
			}
			begin = end + 1;
		}
	}
}

void TouchNames(openmpt_module *mod)
{
	const std::int32_t subsongs = std::clamp<std::int32_t>(openmpt_module_get_num_subsongs(mod), 0, 256);
	const std::int32_t orders = std::clamp<std::int32_t>(openmpt_module_get_num_orders(mod), 0, 256);
	const std::int32_t patterns = std::clamp<std::int32_t>(openmpt_module_get_num_patterns(mod), 0, 128);
	const std::int32_t instruments = std::clamp<std::int32_t>(openmpt_module_get_num_instruments(mod), 0, 256);
	const std::int32_t samples = std::clamp<std::int32_t>(openmpt_module_get_num_samples(mod), 0, 256);
	const std::int32_t channels = std::clamp<std::int32_t>(openmpt_module_get_num_channels(mod), 0, 128);

	for(std::int32_t i = 0; i < subsongs; ++i)
	{
		FreeLibString(openmpt_module_get_subsong_name(mod, i));
	}
	for(std::int32_t i = 0; i < orders; ++i)
	{
		FreeLibString(openmpt_module_get_order_name(mod, i));
	}
	for(std::int32_t i = 0; i < patterns; ++i)
	{
		FreeLibString(openmpt_module_get_pattern_name(mod, i));
	}
	for(std::int32_t i = 0; i < instruments; ++i)
	{
		FreeLibString(openmpt_module_get_instrument_name(mod, i));
	}
	for(std::int32_t i = 0; i < samples; ++i)
	{
		FreeLibString(openmpt_module_get_sample_name(mod, i));
	}
	for(std::int32_t i = 0; i < channels; ++i)
	{
		FreeLibString(openmpt_module_get_channel_name(mod, i));
	}
	FreeLibString(openmpt_module_get_metadata(mod, "type"));
	FreeLibString(openmpt_module_get_metadata(mod, "tracker"));
	FreeLibString(openmpt_module_get_metadata(mod, "title"));
}

void TouchPatterns(openmpt_module *mod)
{
	const std::int32_t patterns = std::clamp<std::int32_t>(openmpt_module_get_num_patterns(mod), 0, 64);
	const std::int32_t channels = std::clamp<std::int32_t>(openmpt_module_get_num_channels(mod), 0, 32);
	for(std::int32_t pattern = 0; pattern < patterns; ++pattern)
	{
		const std::int32_t rows = std::clamp<std::int32_t>(openmpt_module_get_pattern_num_rows(mod, pattern), 0, 256);
		(void)openmpt_module_get_pattern_rows_per_beat(mod, pattern);
		(void)openmpt_module_get_pattern_rows_per_measure(mod, pattern);
		for(std::int32_t row = 0; row < rows; ++row)
		{
			for(std::int32_t channel = 0; channel < channels; ++channel)
			{
				FreeLibString(openmpt_module_format_pattern_row_channel(mod, pattern, row, channel, 0, 1));
				if(pattern < 2 && row < 2 && channel < 2)
				{
					FreeLibString(openmpt_module_highlight_pattern_row_channel(mod, pattern, row, channel, 0, 1));
					for(int command = 0; command < 6; ++command)
					{
						FreeLibString(openmpt_module_format_pattern_row_channel_command(mod, pattern, row, channel, command));
					}
				}
			}
		}
	}
}

struct ProbeResult
{
	bool ok = true;
	std::uint64_t frames = 0;
	bool guard_ok = true;
};

ProbeResult ExerciseModule(openmpt_module *mod)
{
	ProbeResult result;

	(void)openmpt_module_get_num_subsongs(mod);
	(void)openmpt_module_get_num_channels(mod);
	(void)openmpt_module_get_num_orders(mod);
	(void)openmpt_module_get_num_patterns(mod);
	(void)openmpt_module_get_num_instruments(mod);
	(void)openmpt_module_get_num_samples(mod);
	(void)openmpt_module_get_duration_seconds(mod);
	(void)openmpt_module_get_position_seconds(mod);
	(void)openmpt_module_get_time_at_position(mod, 0, 0);
	TouchMetadata(mod);
	TouchNames(mod);
	TouchPatterns(mod);

	(void)openmpt_module_select_subsong(mod, -1);
	(void)openmpt_module_select_subsong(mod, 0);
	(void)openmpt_module_select_subsong(mod, std::numeric_limits<std::int32_t>::max());
	(void)openmpt_module_set_position_seconds(mod, -1.0);
	(void)openmpt_module_set_position_seconds(mod, 0.0);
	(void)openmpt_module_set_position_seconds(mod, 1.0e12);
	(void)openmpt_module_set_position_order_row(mod, -1, -1);
	(void)openmpt_module_set_position_order_row(mod, 0, 0);
	(void)openmpt_module_set_position_order_row(mod, std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max());
	(void)openmpt_module_set_position_seconds(mod, 0.0);

	constexpr std::int32_t kSampleRate = 48000;
	const std::int32_t chunk_sizes[] = {1, 7, 449, 4096};
	for(std::size_t iteration = 0; iteration < 32; ++iteration)
	{
		const std::int32_t chunk = chunk_sizes[iteration % (sizeof(chunk_sizes) / sizeof(chunk_sizes[0]))];
		if((iteration % 3) == 0)
		{
			auto storage = MakeGuardedBuffer(static_cast<std::size_t>(chunk) * 2);
			const std::size_t got = openmpt_module_read_interleaved_stereo(mod, kSampleRate, static_cast<std::size_t>(chunk), Payload(storage));
			result.frames += got;
			result.guard_ok = result.guard_ok && GuardsIntact(storage);
			if(got == 0)
			{
				break;
			}
		} else if((iteration % 3) == 1)
		{
			auto storage = MakeGuardedBuffer(static_cast<std::size_t>(chunk));
			const std::size_t got = openmpt_module_read_mono(mod, kSampleRate, static_cast<std::size_t>(chunk), Payload(storage));
			result.frames += got;
			result.guard_ok = result.guard_ok && GuardsIntact(storage);
			if(got == 0)
			{
				break;
			}
		} else
		{
			auto storage = MakeGuardedBuffer(static_cast<std::size_t>(chunk) * 4);
			const std::size_t got = openmpt_module_read_interleaved_quad(mod, kSampleRate, static_cast<std::size_t>(chunk), Payload(storage));
			result.frames += got;
			result.guard_ok = result.guard_ok && GuardsIntact(storage);
			if(got == 0)
			{
				break;
			}
		}
	}

	(void)openmpt_module_get_position_seconds(mod);
	(void)openmpt_module_get_current_order(mod);
	(void)openmpt_module_get_current_row(mod);
	(void)openmpt_module_get_current_pattern(mod);
	(void)openmpt_module_get_current_speed(mod);
	(void)openmpt_module_get_current_tempo2(mod);
	result.ok = result.guard_ok;
	return result;
}

int RunProbe(const std::string &path)
{
	std::vector<std::uint8_t> data;
	if(!ReadFile(path, data))
	{
		std::cout << "{\"error\":\"cannot_read_input\"}\n";
		return 2;
	}

	CallbackStats stats;
	int probe_error = OPENMPT_ERROR_OK;
	const char *probe_error_message = nullptr;
	const int probe = openmpt_probe_file_header(
		OPENMPT_PROBE_FILE_HEADER_FLAGS_DEFAULT,
		data.empty() ? nullptr : data.data(),
		data.size(),
		data.size(),
		LogFunc,
		&stats,
		ErrorFunc,
		&stats,
		&probe_error,
		&probe_error_message);

	int create_error = OPENMPT_ERROR_OK;
	const char *create_error_message = nullptr;
	openmpt_module *mod = CreateModule(data, stats, create_error, create_error_message);
	ProbeResult result;
	std::size_t repeated_loads = 0;
	if(mod)
	{
		result = ExerciseModule(mod);
		openmpt_module_destroy(mod);

		// A few extra complete create/destroy cycles make lifetime leaks and
		// double-frees much more likely to be observed by ASan/LSan.
		for(int i = 0; i < 3; ++i)
		{
			CallbackStats repeat_stats;
			int repeat_error = OPENMPT_ERROR_OK;
			const char *repeat_error_message = nullptr;
			openmpt_module *repeat = CreateModule(data, repeat_stats, repeat_error, repeat_error_message);
			if(repeat)
			{
				auto storage = MakeGuardedBuffer(64 * 2);
				(void)openmpt_module_read_interleaved_stereo(repeat, 48000, 64, Payload(storage));
				result.guard_ok = result.guard_ok && GuardsIntact(storage);
				openmpt_module_destroy(repeat);
				repeated_loads++;
			}
			FreeLibString(repeat_error_message);
		}
	}

	const bool probe_mismatch = mod && (probe == OPENMPT_PROBE_FILE_HEADER_RESULT_FAILURE);
	const bool success = result.ok && !probe_mismatch;
	std::cout
		<< "{"
		<< "\"loaded\":" << (mod ? "true" : "false") << ","
		<< "\"success\":" << (success ? "true" : "false") << ","
		<< "\"probe\":" << probe << ","
		<< "\"probe_mismatch\":" << (probe_mismatch ? "true" : "false") << ","
		<< "\"probe_error\":" << probe_error << ","
		<< "\"create_error\":" << create_error << ","
		<< "\"log_events\":" << stats.log_events << ","
		<< "\"error_events\":" << stats.error_events << ","
		<< "\"frames\":" << result.frames << ","
		<< "\"guard_ok\":" << (result.guard_ok ? "true" : "false") << ","
		<< "\"repeated_loads\":" << repeated_loads
		<< "}\n";

	if(!success)
	{
		std::cerr << "property failure";
		if(create_error_message)
		{
			std::cerr << ": " << create_error_message;
		}
		if(probe_error_message)
		{
			std::cerr << " / probe: " << probe_error_message;
		}
		std::cerr << "\n";
		FreeLibString(probe_error_message);
		FreeLibString(create_error_message);
		return 3;
	}
	FreeLibString(probe_error_message);
	FreeLibString(create_error_message);
	return 0;
}

int RunRender(const std::string &path, const std::string &out_path, std::size_t frames)
{
	std::vector<std::uint8_t> data;
	if(!ReadFile(path, data))
	{
		std::cerr << "cannot read input\n";
		return 2;
	}

	CallbackStats stats;
	int error = OPENMPT_ERROR_OK;
	const char *error_message = nullptr;
	openmpt_module *mod = CreateModule(data, stats, error, error_message);
	if(!mod)
	{
		std::cerr << "cannot load module (error " << error << ")\n";
		return 3;
	}

	std::vector<std::int16_t> pcm(frames * 2);
	const std::size_t got = openmpt_module_read_interleaved_stereo(mod, 48000, frames, pcm.data());
	pcm.resize(got * 2);
	openmpt_module_destroy(mod);
	if(!WriteFile(out_path, pcm))
	{
		std::cerr << "cannot write output\n";
		return 4;
	}
	std::cout << got << "\n";
	return 0;
}

void Usage(const char *argv0)
{
	std::cerr
		<< "usage:\n"
		<< "  " << argv0 << " --probe FILE\n"
		<< "  " << argv0 << " --render FILE OUT.pcm [FRAMES]\n";
}

} // namespace

int main(int argc, char **argv)
{
	if(argc >= 3 && std::strcmp(argv[1], "--probe") == 0)
	{
		return RunProbe(argv[2]);
	}
	if(argc >= 4 && std::strcmp(argv[1], "--render") == 0)
	{
		std::size_t frames = 50000;
		if(argc >= 5)
		{
			frames = static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 10));
		}
		return RunRender(argv[2], argv[3], frames);
	}
	Usage(argv[0]);
	return 1;
}
