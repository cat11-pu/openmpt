/*
 * bench_mixer.cpp
 * ---------------
 * Purpose: Reproducible mixer benchmark and bit-exact output comparison.
 * Notes  : Renders a module in fixed-size chunks at 48 kHz stereo and hashes
 *          every interleaved float sample, optionally dumping raw PCM.
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#include <libopenmpt/libopenmpt.hpp>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

constexpr std::int32_t samplerate = 48000;
constexpr std::size_t chunkFrames = 512;

double thread_cpu_seconds()
{
	timespec ts{};
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1000000000.0;
}

std::uint64_t hash_samples(const float *samples, std::size_t count)
{
	std::uint64_t hash = 1469598103934665603ull;
	const unsigned char *bytes = reinterpret_cast<const unsigned char *>(samples);
	for(std::size_t i = 0; i < count * sizeof(float); i++)
	{
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

struct Result
{
	std::uint64_t hash = 0;
	std::size_t frames = 0;
	double renderSeconds = 0.0;
	double loadSeconds = 0.0;
};

Result run(const std::string &filename, double seconds, bool dump, const std::string &dumpFilename, std::int32_t filterLength)
{
	std::ifstream file(filename, std::ios::binary);
	if(!file)
		throw std::runtime_error("cannot open " + filename);

	const double loadStart = thread_cpu_seconds();
	openmpt::module mod{file};
	const double loadEnd = thread_cpu_seconds();

	mod.set_render_param(openmpt::module::RENDER_INTERPOLATIONFILTER_LENGTH, filterLength);
	mod.set_render_param(openmpt::module::RENDER_VOLUMERAMPING_STRENGTH, 0);
	mod.set_render_param(openmpt::module::RENDER_STEREOSEPARATION_PERCENT, 100);
	mod.set_position_order_row(0, 0);

	const std::size_t totalFrames = static_cast<std::size_t>(seconds * samplerate);
	std::vector<float> buffer(chunkFrames * 2);
	std::ofstream dumpFile;
	if(dump)
	{
		dumpFile.open(dumpFilename, std::ios::binary);
		if(!dumpFile)
			throw std::runtime_error("cannot open " + dumpFilename);
	}

	std::uint64_t hash = 1469598103934665603ull;
	std::size_t frames = 0;
	const double renderStart = thread_cpu_seconds();
	while(frames < totalFrames)
	{
		const std::size_t wanted = std::min(chunkFrames, totalFrames - frames);
		const std::size_t got = mod.read_interleaved_stereo(samplerate, wanted, buffer.data());
		if(got == 0)
			break;
		hash = hash ^ hash_samples(buffer.data(), got);
		hash *= 1099511628211ull;
		if(dump)
			dumpFile.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(got * 2 * sizeof(float)));
		frames += got;
	}
	const double renderEnd = thread_cpu_seconds();

	Result result;
	result.hash = hash;
	result.frames = frames;
	result.renderSeconds = renderEnd - renderStart;
	result.loadSeconds = loadEnd - loadStart;
	return result;
}

} // namespace

int main(int argc, char **argv)
{
	if(argc < 4)
	{
		std::cerr << "Usage: bench_mixer MODULE SECONDS RUNS [DUMP.pcm [FILTER_LENGTH]]\n";
		return 1;
	}

	const std::string filename = argv[1];
	const double seconds = std::stod(argv[2]);
	const int runs = std::stoi(argv[3]);
	const bool dump = argc >= 5;
	const std::string dumpFilename = dump ? argv[4] : std::string{};
	const std::int32_t filterLength = argc >= 6 ? std::stoi(argv[5]) : 8;

	Result first;
	std::vector<double> renderTimes;
	std::vector<double> loadTimes;
	for(int runIndex = 0; runIndex < runs; runIndex++)
	{
		const Result result = run(filename, seconds, dump && runIndex == 0, dumpFilename, filterLength);
		if(runIndex == 0)
			first = result;
		else if(result.hash != first.hash || result.frames != first.frames)
			throw std::runtime_error("non-deterministic output between benchmark runs");
		renderTimes.push_back(result.renderSeconds);
		loadTimes.push_back(result.loadSeconds);
	}

	std::sort(renderTimes.begin(), renderTimes.end());
	std::sort(loadTimes.begin(), loadTimes.end());
	const double renderMedian = renderTimes[renderTimes.size() / 2];
	const double loadMedian = loadTimes[loadTimes.size() / 2];
	const double audioSeconds = static_cast<double>(first.frames) / samplerate;

	std::cout << std::fixed << std::setprecision(6);
	std::cout << "file=" << filename
	          << " frames=" << first.frames
	          << " audio_s=" << audioSeconds
	          << " render_s=" << renderMedian
	          << " load_ms=" << loadMedian * 1000.0
	          << " realtime_x=" << (renderMedian > 0.0 ? audioSeconds / renderMedian : 0.0)
	          << " hash=" << std::hex << std::setw(16) << std::setfill('0') << first.hash
	          << std::dec << std::setfill(' ') << "\n";
	return 0;
}
