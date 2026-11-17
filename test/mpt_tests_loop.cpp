/*
 * mpt_tests_loop.cpp
 * ------------------
 * Purpose: Tests for sample-continuous loop playback.
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#include "stdafx.h"

#ifdef ENABLE_TESTS

#include "test.h"
#include "TestTools.h"

#include "../libopenmpt/libopenmpt_loop.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <new>
#include <sstream>
#include <string>
#include <vector>

static std::atomic<std::int64_t> g_allocations{0};
static std::atomic<std::int64_t> g_deallocations{0};

void * operator new( std::size_t size ) {
	void * const result = std::malloc( size ? size : 1 );
	if ( !result ) {
		throw std::bad_alloc();
	}
	g_allocations.fetch_add( 1, std::memory_order_relaxed );
	return result;
}

void * operator new[]( std::size_t size ) {
	return ::operator new( size );
}

void operator delete( void * ptr ) noexcept {
	if ( ptr ) {
		g_deallocations.fetch_add( 1, std::memory_order_relaxed );
		std::free( ptr );
	}
}

void operator delete[]( void * ptr ) noexcept {
	::operator delete( ptr );
}

void operator delete( void * ptr, std::size_t ) noexcept {
	::operator delete( ptr );
}

void operator delete[]( void * ptr, std::size_t ) noexcept {
	::operator delete( ptr );
}

OPENMPT_NAMESPACE_BEGIN

namespace Test {

namespace {

void put_be16( std::vector<std::uint8_t> & data, std::size_t offset, std::uint16_t value ) {
	data[offset + 0] = static_cast<std::uint8_t>( value >> 8 );
	data[offset + 1] = static_cast<std::uint8_t>( value >> 0 );
}

void put_cell( std::vector<std::uint8_t> & data, std::size_t offset, std::uint8_t sample, std::uint16_t period, std::uint8_t effect, std::uint8_t parameter ) {
	data[offset + 0] = static_cast<std::uint8_t>( ( sample & 0xF0 ) | ( ( period >> 8 ) & 0x0F ) );
	data[offset + 1] = static_cast<std::uint8_t>( period & 0xFF );
	data[offset + 2] = static_cast<std::uint8_t>( ( ( sample & 0x0F ) << 4 ) | ( effect & 0x0F ) );
	data[offset + 3] = parameter;
}

std::vector<std::uint8_t> make_loop_mod( bool with_intro ) {
	constexpr std::size_t sample_frames = 128;
	constexpr std::size_t pattern_size = 64 * 4 * 4;
	const std::size_t order_count = with_intro ? 3 : 2;
	const std::size_t pattern_count = order_count;
	std::vector<std::uint8_t> data( 1084 + pattern_count * pattern_size + sample_frames, 0 );

	const char title[] = "LOOP TEST";
	std::copy( title, title + sizeof( title ) - 1, data.begin() );
	const char sample_name[] = "sine";
	std::copy( sample_name, sample_name + sizeof( sample_name ) - 1, data.begin() + 20 );
	put_be16( data, 42, 64 );
	data[44] = 0;
	data[45] = 64;
	put_be16( data, 46, 0 );
	put_be16( data, 48, 64 );
	data[950] = static_cast<std::uint8_t>( order_count );
	data[951] = with_intro ? 1 : 0;
	for ( std::size_t order = 0; order < order_count; ++order ) {
		data[952 + order] = static_cast<std::uint8_t>( order );
	}
	std::copy( "M.K.", "M.K." + 4, data.begin() + 1080 );

	std::size_t pattern = 1084;
	if ( with_intro ) {
		put_cell( data, pattern + 0, 1, 0x1AC, 0x0F, 0xFF );
		pattern += pattern_size;
		put_cell( data, pattern + 8, 0, 0, 0x0B, 0x02 );
		pattern += pattern_size;
		put_cell( data, pattern + 8, 0, 0, 0x0B, 0x01 );
	} else {
		put_cell( data, pattern + 0, 1, 0x1AC, 0x0F, 0x01 );
		put_cell( data, pattern + 4, 0, 0, 0x0F, 0xFF );
		put_cell( data, pattern + 8, 0, 0, 0x0B, 0x01 );
		pattern += pattern_size;
		put_cell( data, pattern + 8, 0, 0, 0x0B, 0x00 );
	}

	const std::size_t sample_offset = 1084 + pattern_count * pattern_size;
	for ( std::size_t frame = 0; frame < sample_frames; ++frame ) {
		const double value = std::sin( 2.0 * 3.14159265358979323846 * static_cast<double>( frame ) / 32.0 );
		const std::int16_t sample = static_cast<std::int16_t>( value * 12000.0 );
		data[sample_offset + frame] = static_cast<std::uint8_t>( sample & 0xFF );
	}
	return data;
}

struct loop_fixture {
	std::vector<std::uint8_t> data;
	std::vector<float> reference;
};

loop_fixture make_reference( bool with_intro, std::size_t frames ) {
	loop_fixture fixture;
	fixture.data = make_loop_mod( with_intro );
	std::istringstream stream( std::string( fixture.data.begin(), fixture.data.end() ) );
	openmpt::module module( stream );
	module.set_repeat_count( -1 );
	module.select_subsong( 0 );
	fixture.reference.resize( frames * 2 );
	std::size_t total = 0;
	while ( total < frames ) {
		const std::size_t want = std::min<std::size_t>( 257, frames - total );
		const std::size_t got = module.read_interleaved_stereo( 48000, want, fixture.reference.data() + total * 2 );
		VERIFY_EQUAL_QUIET_NONCONT( got, want );
		total += got;
	}
	return fixture;
}

bool throws_openmpt_exception( const std::function<void()> & function ) {
	try {
		function();
	} catch ( const openmpt::exception & ) {
		return true;
	}
	return false;
}

std::int64_t live_allocations() {
	return g_allocations.load( std::memory_order_relaxed ) - g_deallocations.load( std::memory_order_relaxed );
}

bool samples_match( const float * left, const float * right, std::size_t frames ) {
	for ( std::size_t sample = 0; sample < frames * 2; ++sample ) {
		if ( std::abs( left[sample] - right[sample] ) > 1.0e-6f ) {
			return false;
		}
	}
	return true;
}

} // namespace

void TestLoop() {
	constexpr std::int32_t samplerate = 48000;
	constexpr std::size_t max_read_frames = 257;
	constexpr std::size_t prefetch_frames = 131;
	constexpr std::size_t cycles = 10000;

	openmpt::loop_config config;
	config.samplerate = samplerate;
	config.max_read_frames = max_read_frames;
	config.prefetch_frames = prefetch_frames;

	const std::vector<std::uint8_t> data = make_loop_mod( false );
	const std::int64_t allocation_baseline = live_allocations();

	{
		openmpt::loop_stream stream( data.data(), data.size(), config );
		const openmpt::loop_info info = stream.get_info();
		VERIFY_EQUAL_NONCONT( info.start_order, 0 );
		VERIFY_EQUAL_NONCONT( info.start_row, 0 );
		VERIFY_EQUAL_NONCONT( info.end_order, 1 );
		VERIFY_EQUAL_NONCONT( info.end_row, 0 );
		VERIFY_EQUAL_NONCONT( info.intro_frames, 0 );
		VERIFY_EQUAL_NONCONT( info.first_pass_frames, 1430 );
		VERIFY_EQUAL_NONCONT( info.loop_frames, 1430 );
		VERIFY_EQUAL_NONCONT( info.samplerate, 48000 );
		VERIFY_EQUAL_NONCONT( stream.buffered_frames(), prefetch_frames );
		VERIFY_EQUAL_NONCONT( stream.frames_until_loop(), info.first_pass_frames );

		std::vector<float> output( max_read_frames * 2 );
		VERIFY_EQUAL_NONCONT( stream.read( nullptr, 1 ), 0 );
		VERIFY_EQUAL_NONCONT( stream.read( output.data(), 0 ), 0 );
		VERIFY_EQUAL_NONCONT( stream.emitted_frames(), 0 );

		loop_fixture fixture = make_reference( false, info.first_pass_frames + info.loop_frames * cycles );
		const std::uint64_t target_frames = info.first_pass_frames + info.loop_frames * cycles;
		std::uint64_t total = 0;
		std::size_t read_sizes[] = { 257, 1, 127, 255, 64, 256 };
		std::size_t read_index = 0;
		const std::int64_t before_cycles = live_allocations();
		while ( total < target_frames ) {
			std::size_t want = read_sizes[read_index % ( sizeof( read_sizes ) / sizeof( read_sizes[0] ) )];
			++read_index;
			want = static_cast<std::size_t>( std::min<std::uint64_t>( want, target_frames - total ) );
			const std::size_t got = stream.read( output.data(), want );
			VERIFY_EQUAL_QUIET_NONCONT( got, want );
			VERIFY_EQUAL_QUIET_NONCONT(
				samples_match( output.data(), fixture.reference.data() + total * 2, got ),
				true );
			total += got;
			VERIFY_EQUAL_QUIET_NONCONT( stream.buffered_frames(), prefetch_frames );
		}
		VERIFY_EQUAL_NONCONT( total, target_frames );
		VERIFY_EQUAL_NONCONT( stream.emitted_frames(), target_frames );
		VERIFY_EQUAL_NONCONT( stream.frames_until_loop(), info.loop_frames );
		VERIFY_EQUAL_NONCONT( live_allocations(), before_cycles );
		VERIFY_EQUAL_NONCONT( stream.read( output.data(), max_read_frames + 1 ), max_read_frames );
	}
	VERIFY_EQUAL_NONCONT( live_allocations(), allocation_baseline );

	{
		const std::vector<std::uint8_t> intro_data = make_loop_mod( true );
		openmpt::loop_stream stream( intro_data.data(), intro_data.size(), config );
		const openmpt::loop_info info = stream.get_info();
		VERIFY_EQUAL_NONCONT( info.start_order, 1 );
		VERIFY_EQUAL_NONCONT( info.start_row, 0 );
		VERIFY_EQUAL_NONCONT( info.intro_frames > 0, true );
		VERIFY_EQUAL_NONCONT( info.first_pass_frames > info.intro_frames, true );
		VERIFY_EQUAL_NONCONT( info.loop_frames > 0, true );

		const std::uint64_t target_frames = info.first_pass_frames + info.loop_frames * 3;
		loop_fixture fixture = make_reference( true, target_frames );
		std::vector<float> output( max_read_frames * 2 );
		std::uint64_t total = 0;
		while ( total < target_frames ) {
			const std::size_t want = static_cast<std::size_t>( std::min<std::uint64_t>( max_read_frames, target_frames - total ) );
			const std::size_t got = stream.read( output.data(), want );
			VERIFY_EQUAL_QUIET_NONCONT( got, want );
			VERIFY_EQUAL_QUIET_NONCONT(
				samples_match( output.data(), fixture.reference.data() + total * 2, got ),
				true );
			total += got;
		}
	}

	VERIFY_EQUAL_NONCONT( throws_openmpt_exception( [&]() { openmpt::loop_config invalid = config; invalid.samplerate = 7999; openmpt::loop_stream stream( data.data(), data.size(), invalid ); } ), true );
	VERIFY_EQUAL_NONCONT( throws_openmpt_exception( [&]() { openmpt::loop_config invalid = config; invalid.max_read_frames = 0; openmpt::loop_stream stream( data.data(), data.size(), invalid ); } ), true );
	VERIFY_EQUAL_NONCONT( throws_openmpt_exception( [&]() { openmpt::loop_config invalid = config; invalid.prefetch_frames = 0; openmpt::loop_stream stream( data.data(), data.size(), invalid ); } ), true );
	VERIFY_EQUAL_NONCONT( throws_openmpt_exception( [&]() { openmpt::loop_config invalid = config; invalid.subsong = 1; openmpt::loop_stream stream( data.data(), data.size(), invalid ); } ), true );
	VERIFY_EQUAL_NONCONT( throws_openmpt_exception( [&]() { openmpt::loop_stream stream( nullptr, 0, config ); } ), true );
}

} // namespace Test

OPENMPT_NAMESPACE_END

#endif
