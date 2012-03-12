/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/

#include "../../StdAfx.h"

#include "frame_muxer.h"

#include "../filter/filter.h"
#include "../util/util.h"

#include <core/producer/frame_producer.h>
#include <core/frame/draw_frame.h>
#include <core/frame/frame_transform.h>
#include <core/frame/pixel_format.h>
#include <core/frame/frame_factory.h>
#include <core/frame/frame.h>

#include <common/env.h>
#include <common/except.h>
#include <common/log.h>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

#include <common/assert.h>
#include <boost/foreach.hpp>
#include <boost/range/algorithm_ext/push_back.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <deque>
#include <queue>
#include <vector>

using namespace caspar::core;

namespace caspar { namespace ffmpeg {
	
struct frame_muxer::impl : boost::noncopyable
{	
	std::queue<std::queue<core::mutable_frame>>		video_streams_;
	std::queue<core::audio_buffer>					audio_streams_;
	std::queue<draw_frame>							frame_buffer_;
	display_mode									display_mode_;
	const double									in_fps_;
	const video_format_desc							format_desc_;
	
	std::vector<int>								audio_cadence_;
			
	spl::shared_ptr<core::frame_factory>			frame_factory_;
	
	filter											filter_;
	const std::wstring								filter_str_;
		
	impl(double in_fps, const spl::shared_ptr<core::frame_factory>& frame_factory, const core::video_format_desc& format_desc, const std::wstring& filter_str)
		: display_mode_(display_mode::invalid)
		, in_fps_(in_fps)
		, format_desc_(format_desc)
		, audio_cadence_(format_desc_.audio_cadence)
		, frame_factory_(frame_factory)
		, filter_str_(filter_str)
	{
		video_streams_.push(std::queue<core::mutable_frame>());
		audio_streams_.push(core::audio_buffer());
		
		// Note: Uses 1 step rotated cadence for 1001 modes (1602, 1602, 1601, 1602, 1601)
		// This cadence fills the audio mixer most optimally.
		boost::range::rotate(audio_cadence_, std::end(audio_cadence_)-1);
	}

	void push_video(const std::shared_ptr<AVFrame>& video_frame)
	{		
		if(!video_frame)
			return;
		
		if(video_frame == flush_video())		
			video_streams_.push(std::queue<core::mutable_frame>());		
		else if(video_frame == empty_video())
		{
			auto empty_frame = frame_factory_->create_frame(this, core::pixel_format_desc(core::pixel_format::invalid));
			video_streams_.back().push(std::move(empty_frame));
			display_mode_ = display_mode::simple;
		}
		else
		{
			if(display_mode_ == display_mode::invalid)
				update_display_mode(video_frame);
				
			filter_.push(video_frame);
			BOOST_FOREACH(auto& av_frame, filter_.poll_all())			
				video_streams_.back().push(make_frame(this, av_frame, format_desc_.fps, *frame_factory_));			
		}

		if(video_streams_.back().size() > 32)
			BOOST_THROW_EXCEPTION(invalid_operation() << source_info("frame_muxer") << msg_info("video-stream overflow. This can be caused by incorrect frame-rate. Check clip meta-data."));
	}

	void push_audio(const std::shared_ptr<core::audio_buffer>& audio)
	{
		if(!audio)	
			return;

		if(audio == flush_audio())		
			audio_streams_.push(core::audio_buffer());		
		else if(audio == empty_audio())		
			boost::range::push_back(audio_streams_.back(), core::audio_buffer(audio_cadence_.front(), 0));		
		else		
			boost::range::push_back(audio_streams_.back(), *audio);		

		if(audio_streams_.back().size() > static_cast<size_t>(32*audio_cadence_.front()))
			BOOST_THROW_EXCEPTION(invalid_operation() << source_info("frame_muxer") << msg_info("audio-stream overflow. This can be caused by incorrect frame-rate. Check clip meta-data."));
	}
	
	bool video_ready() const
	{		
		return video_streams_.size() > 1 || (video_streams_.size() >= audio_streams_.size() && video_ready2());
	}
	
	bool audio_ready() const
	{
		return audio_streams_.size() > 1 || (audio_streams_.size() >= video_streams_.size() && audio_ready2());
	}

	bool video_ready2() const
	{		
		switch(display_mode_)
		{
		case display_mode::deinterlace_bob_reinterlace:					
		case display_mode::interlace:	
		case display_mode::half:
			return video_streams_.front().size() >= 2;
		default:										
			return video_streams_.front().size() >= 1;
		}
	}
	
	bool audio_ready2() const
	{
		switch(display_mode_)
		{
		case display_mode::duplicate:					
			return audio_streams_.front().size()/2 >= static_cast<size_t>(audio_cadence_.front());
		default:										
			return audio_streams_.front().size() >= static_cast<size_t>(audio_cadence_.front());
		}
	}
		
	bool try_pop(core::draw_frame& result)
	{
		if(!frame_buffer_.empty())
		{
			result = std::move(frame_buffer_.front());
			frame_buffer_.pop();	
			return true;
		}

		if(video_streams_.size() > 1 && audio_streams_.size() > 1 && (!video_ready2() || !audio_ready2()))
		{
			if(!video_streams_.front().empty() || !audio_streams_.front().empty())
				CASPAR_LOG(trace) << "Truncating: " << video_streams_.front().size() << L" video-frames, " << audio_streams_.front().size() << L" audio-samples.";

			video_streams_.pop();
			audio_streams_.pop();
		}

		if(!video_ready2() || !audio_ready2() || display_mode_ == display_mode::invalid)
			return false;
				
		auto frame1				= pop_video();
		frame1.audio_data()	= pop_audio();

		switch(display_mode_)
		{
		case display_mode::simple:						
		case display_mode::deinterlace_bob:				
		case display_mode::deinterlace:	
			{
				frame_buffer_.push(core::draw_frame(std::move(frame1)));
				break;
			}
		case display_mode::interlace:					
		case display_mode::deinterlace_bob_reinterlace:	
			{				
				auto frame2 = pop_video();

				frame_buffer_.push(core::draw_frame::interlace(
					core::draw_frame(std::move(frame1)),
					core::draw_frame(std::move(frame2)),
					format_desc_.field_mode));	
				break;
			}
		case display_mode::duplicate:	
			{
				boost::range::push_back(frame1.audio_data(), pop_audio());

				auto draw_frame = core::draw_frame(std::move(frame1));
				frame_buffer_.push(draw_frame);
				frame_buffer_.push(draw_frame);
				break;
			}
		case display_mode::half:	
			{				
				pop_video(); // Throw away

				frame_buffer_.push(core::draw_frame(std::move(frame1)));
				break;
			}
		default:
			BOOST_THROW_EXCEPTION(invalid_operation());
		}
		
		return try_pop(result);
	}
	
	core::mutable_frame pop_video()
	{
		auto frame = std::move(video_streams_.front().front());
		video_streams_.front().pop();		
		return std::move(frame);
	}

	core::audio_buffer pop_audio()
	{
		CASPAR_VERIFY(audio_streams_.front().size() >= static_cast<size_t>(audio_cadence_.front()));

		auto begin = audio_streams_.front().begin();
		auto end   = begin + audio_cadence_.front();

		core::audio_buffer samples(begin, end);
		audio_streams_.front().erase(begin, end);
		
		boost::range::rotate(audio_cadence_, std::begin(audio_cadence_)+1);

		return samples;
	}
				
	void update_display_mode(const std::shared_ptr<AVFrame>& frame)
	{
		std::wstring filter_str = filter_str_;

		display_mode_ = display_mode::simple;

		auto mode = get_mode(*frame);
		if(mode == core::field_mode::progressive && frame->height < 720 && in_fps_ < 50.0) // SD frames are interlaced. Probably incorrect meta-data. Fix it.
			mode = core::field_mode::upper;

		auto fps  = in_fps_;

		if(filter::is_deinterlacing(filter_str_))
			mode = core::field_mode::progressive;

		if(filter::is_double_rate(filter_str_))
			fps *= 2;
			
		display_mode_ = get_display_mode(mode, fps, format_desc_.field_mode, format_desc_.fps);
			
		if((frame->height != 480 || format_desc_.height != 486) && // don't deinterlace for NTSC DV
				display_mode_ == display_mode::simple && mode != core::field_mode::progressive && format_desc_.field_mode != core::field_mode::progressive && 
				frame->height != format_desc_.height)
		{
			display_mode_ = display_mode::deinterlace_bob_reinterlace; // The frame will most likely be scaled, we need to deinterlace->reinterlace	
		}
		
		if(display_mode_ == display_mode::deinterlace)
			filter_str = append_filter(filter_str, L"YADIF=0:-1");
		else if(display_mode_ == display_mode::deinterlace_bob || display_mode_ == display_mode::deinterlace_bob_reinterlace)
			filter_str = append_filter(filter_str, L"YADIF=1:-1");

		if(display_mode_ == display_mode::invalid)
		{
			CASPAR_LOG(warning) << L"[frame_muxer] Auto-transcode: Failed to detect display-mode.";
			display_mode_ = display_mode::simple;
		}
			
		if(!boost::iequals(filter_.filter_str(), filter_str))
		{
			for(int n = 0; n < filter_.delay(); ++n)
			{
				filter_.push(frame);
				auto av_frame = filter_.poll();
				if(av_frame)							
					video_streams_.back().push(make_frame(this, spl::make_shared_ptr(av_frame), format_desc_.fps, *frame_factory_));
			}
			filter_ = filter(filter_str);
			CASPAR_LOG(info) << L"[frame_muxer] " << display_mode_ << L" " << print_mode(frame->width, frame->height, in_fps_, frame->interlaced_frame > 0);
		}
	}
	
	uint32_t calc_nb_frames(uint32_t nb_frames) const
	{
		uint64_t nb_frames2 = nb_frames;
		
		if(filter_.is_double_rate()) // Take into account transformations in filter.
			nb_frames2 *= 2;

		switch(display_mode_) // Take into account transformation in run.
		{
		case display_mode::deinterlace_bob_reinterlace:
		case display_mode::interlace:	
		case display_mode::half:
			nb_frames2 /= 2;
			break;
		case display_mode::duplicate:
			nb_frames2 *= 2;
			break;
		}

		return static_cast<uint32_t>(nb_frames2);
	}
};

frame_muxer::frame_muxer(double in_fps, const spl::shared_ptr<core::frame_factory>& frame_factory, const core::video_format_desc& format_desc, const std::wstring& filter)
	: impl_(new impl(in_fps, frame_factory, format_desc, filter)){}
void frame_muxer::push_video(const std::shared_ptr<AVFrame>& video_frame){impl_->push_video(video_frame);}
void frame_muxer::push_audio(const std::shared_ptr<core::audio_buffer>& audio_samples){return impl_->push_audio(audio_samples);}
bool frame_muxer::try_pop(core::draw_frame& result){return impl_->try_pop(result);}
uint32_t frame_muxer::calc_nb_frames(uint32_t nb_frames) const {return impl_->calc_nb_frames(nb_frames);}
bool frame_muxer::video_ready() const{return impl_->video_ready();}
bool frame_muxer::audio_ready() const{return impl_->audio_ready();}

}}