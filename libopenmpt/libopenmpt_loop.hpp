/*
 * libopenmpt_loop.hpp
 * -------------------
 * Purpose: Sample-continuous loop playback for libopenmpt.
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#pragma once

#include "libopenmpt.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <istream>
#include <iterator>
#include <limits>
#include <memory>
#include <vector>

namespace openmpt {

// Fixed playback and buffering parameters. The stream never reallocates after construction.
struct loop_config {
	// Output sample rate in [8000, 192000].
	std::int32_t samplerate = 48000;
	// A single subsong is required; -1 (all subsongs) is rejected.
	std::int32_t subsong = 0;
	// Maximum number of frames returned by one read() call.
	std::size_t max_read_frames = 1024;
	// Frames kept decoded ahead of the consumer.
	std::size_t prefetch_frames = 4096;
};

// Frame counts refer to the stereo output stream at info.samplerate.
// intro_frames is the first frame of the natural restart position.
// first_pass_frames is the frame where the first restart occurs.
// loop_frames is the natural loop body length after the intro.
struct loop_info {
	std::int32_t start_order = 0;
	std::int32_t start_row = 0;
	std::int32_t end_order = 0;
	std::int32_t end_row = 0;
	std::uint64_t intro_frames = 0;
	std::uint64_t first_pass_frames = 0;
	std::uint64_t loop_frames = 0;
	std::uint64_t samplerate = 0;
};

class loop_stream {
private:
	struct boundary {
		std::uint64_t frame = 0;
		std::int32_t end_order = 0;
		std::int32_t end_row = 0;
	};

	static constexpr std::size_t calibration_chunk = 4096;
	static constexpr std::uint64_t calibration_margin_seconds = 5;

	loop_config m_config;
	loop_info m_info;
	std::unique_ptr<module> m_module;
	std::vector<float> m_ring;
	std::size_t m_capacity = 0;
	std::size_t m_read = 0;
	std::size_t m_write = 0;
	std::size_t m_size = 0;
	std::uint64_t m_emitted = 0;

	static bool at_position( const module & mod, std::int32_t order, std::int32_t row ) {
		return mod.get_current_order() == order && mod.get_current_row() == row;
	}

	static std::uint64_t approximate_frame( double seconds, std::int32_t samplerate ) {
		if ( !std::isfinite( seconds ) || seconds < 0.0 ) {
			throw openmpt::exception( "loop point is not representable" );
		}
		const long double frames = static_cast<long double>( seconds ) * static_cast<long double>( samplerate );
		if ( frames > static_cast<long double>( std::numeric_limits<std::uint64_t>::max() ) ) {
			throw openmpt::exception( "loop point is out of range" );
		}
		return static_cast<std::uint64_t>( std::floor( frames ) );
	}

	static boundary find_boundary( module & mod, const loop_config & config, std::int32_t order, std::int32_t row, double seconds, std::uint64_t minimum_frame ) {
		mod.set_repeat_count( -1 );
		mod.select_subsong( config.subsong );

		const std::uint64_t predicted = approximate_frame( seconds, config.samplerate );
		const std::uint64_t margin = std::min<std::uint64_t>( static_cast<std::uint64_t>( config.samplerate ) * calibration_margin_seconds, predicted / 2 );
		const std::uint64_t scan_start = std::max( minimum_frame, predicted > margin ? predicted - margin : 0 );

		std::vector<float> scratch( calibration_chunk * 2 );
		std::uint64_t total = 0;
		while ( total < scan_start ) {
			const std::size_t want = static_cast<std::size_t>( std::min<std::uint64_t>( calibration_chunk, scan_start - total ) );
			const std::size_t got = mod.read_interleaved_stereo( config.samplerate, want, scratch.data() );
			if ( got == 0 ) {
				throw openmpt::exception( "unable to reach loop point" );
			}
			total += got;
		}

		boundary result;
		result.end_order = mod.get_current_order();
		result.end_row = mod.get_current_row();
		bool left_target = !at_position( mod, order, row );
		const std::uint64_t search_limit = static_cast<std::uint64_t>( config.samplerate ) * 30;
		for ( std::uint64_t i = 0; i < search_limit; ++i ) {
			const std::size_t got = mod.read_interleaved_stereo( config.samplerate, 1, scratch.data() );
			if ( got == 0 ) {
				throw openmpt::exception( "song ended before loop point" );
			}
			++total;
			const std::int32_t current_order = mod.get_current_order();
			const std::int32_t current_row = mod.get_current_row();
			if ( current_order != order || current_row != row ) {
				left_target = true;
				result.end_order = current_order;
				result.end_row = current_row;
			} else if ( left_target ) {
				result.frame = total - 1;
				return result;
			}
		}
		throw openmpt::exception( "loop point was not found" );
	}

	void locate( module & calibration ) {
		if ( m_config.subsong < 0 || m_config.subsong >= calibration.get_num_subsongs() ) {
			throw openmpt::exception( "invalid subsong" );
		}
		calibration.set_repeat_count( -1 );
		calibration.select_subsong( m_config.subsong );

		m_info.start_order = calibration.get_restart_order( m_config.subsong );
		m_info.start_row = calibration.get_restart_row( m_config.subsong );
		m_info.samplerate = static_cast<std::uint64_t>( m_config.samplerate );

		calibration.select_subsong( m_config.subsong );
		if ( at_position( calibration, m_info.start_order, m_info.start_row ) ) {
			m_info.intro_frames = 0;
		} else {
			const double intro_seconds = calibration.get_time_at_position( m_info.start_order, m_info.start_row );
			if ( intro_seconds < 0.0 ) {
				throw openmpt::exception( "invalid loop start position" );
			}
			m_info.intro_frames = find_boundary( calibration, m_config, m_info.start_order, m_info.start_row, intro_seconds, 0 ).frame;
		}

		const double first_pass_seconds = calibration.get_duration_seconds();
		const boundary first_pass = find_boundary( calibration, m_config, m_info.start_order, m_info.start_row, first_pass_seconds, m_info.intro_frames + 1 );
		if ( first_pass.frame <= m_info.intro_frames ) {
			throw openmpt::exception( "loop is empty" );
		}
		m_info.first_pass_frames = first_pass.frame;
		m_info.loop_frames = first_pass.frame - m_info.intro_frames;
		m_info.end_order = first_pass.end_order;
		m_info.end_row = first_pass.end_row;
	}

	void validate_config() const {
		if ( m_config.samplerate < 8000 || m_config.samplerate > 192000 ) {
			throw openmpt::exception( "samplerate out of range" );
		}
		if ( m_config.max_read_frames == 0 ) {
			throw openmpt::exception( "max_read_frames must be non-zero" );
		}
		if ( m_config.prefetch_frames == 0 ) {
			throw openmpt::exception( "prefetch_frames must be non-zero" );
		}
		if ( m_config.max_read_frames > ( std::numeric_limits<std::size_t>::max() / 2 ) - m_config.prefetch_frames ) {
			throw openmpt::exception( "ring buffer size overflow" );
		}
	}

	void produce( std::size_t target ) {
		while ( m_size < target ) {
			const std::size_t contiguous = std::min( target - m_size, m_capacity - m_write );
			const std::size_t got = m_module->read_interleaved_stereo( m_config.samplerate, contiguous, m_ring.data() + m_write * 2 );
			if ( got == 0 ) {
				throw openmpt::exception( "loop renderer stopped unexpectedly" );
			}
			m_write = ( m_write + got ) % m_capacity;
			m_size += got;
		}
	}

	void initialize( const void * data, std::size_t size ) {
		validate_config();
		if ( !data || size == 0 ) {
			throw openmpt::exception( "empty module data" );
		}
		{
			module calibration( data, size );
			locate( calibration );
		}
		m_module = std::make_unique<module>( data, size );
		m_module->set_repeat_count( -1 );
		m_module->select_subsong( m_config.subsong );
		m_capacity = m_config.max_read_frames + m_config.prefetch_frames;
		m_ring.resize( m_capacity * 2 );
		produce( m_config.prefetch_frames );
	}

public:
	// Uses the module's natural restart point. It never seeks, clears, or crossfades at a seam;
	// waveform discontinuities authored into the loop point are preserved.
	// Reads the complete module from stream and owns both its calibration and playback state.
	explicit loop_stream( std::istream & stream, loop_config config = loop_config() )
		: m_config( config ) {
		std::vector<char> data( ( std::istreambuf_iterator<char>( stream ) ), std::istreambuf_iterator<char>() );
		initialize( data.data(), data.size() );
	}

	loop_stream( const void * data, std::size_t size, loop_config config = loop_config() )
		: m_config( config ) {
		initialize( data, size );
	}

	// Loop points are derived from the module's own restart order and row.
	const loop_info & get_info() const noexcept {
		return m_info;
	}

	// Number of decoded frames currently ahead of the consumer.
	std::size_t buffered_frames() const noexcept {
		return m_size;
	}

	std::uint64_t emitted_frames() const noexcept {
		return m_emitted;
	}

	// Frames until the next natural restart boundary in the output stream.
	std::uint64_t frames_until_loop() const noexcept {
		if ( m_emitted < m_info.first_pass_frames ) {
			return m_info.first_pass_frames - m_emitted;
		}
		const std::uint64_t phase = ( m_emitted - m_info.first_pass_frames ) % m_info.loop_frames;
		return phase == 0 ? m_info.loop_frames : m_info.loop_frames - phase;
	}

	// Copies at most max_read_frames interleaved stereo float frames.
	// The next segment is decoded before the call returns.
	std::size_t read( float * interleaved_stereo, std::size_t frames ) {
		if ( !interleaved_stereo || frames == 0 ) {
			return 0;
		}
		frames = std::min( frames, m_config.max_read_frames );
		produce( frames + m_config.prefetch_frames );

		const std::size_t first = std::min( frames, m_capacity - m_read );
		std::memcpy( interleaved_stereo, m_ring.data() + m_read * 2, first * 2 * sizeof( float ) );
		if ( first < frames ) {
			std::memcpy( interleaved_stereo + first * 2, m_ring.data(), ( frames - first ) * 2 * sizeof( float ) );
		}
		m_read = ( m_read + frames ) % m_capacity;
		m_size -= frames;
		m_emitted += frames;

		produce( m_config.prefetch_frames );
		return frames;
	}
};

} // namespace openmpt
