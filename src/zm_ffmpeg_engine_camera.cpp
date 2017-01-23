//
// ZoneMinder Ffmpeg Camera Class Implementation, $Date: 2009-01-16 12:18:50 +0000 (Fri, 16 Jan 2009) $, $Revision: 2713 $
// Copyright (C) 2001-2008 Philip Coombes
// 
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
// 


// work in progress
// the only external interface to this file for now is 
// Camera* newFfmpegEngineCamera(...);
//



#include "zm.h"
#include "zm_ffmpeg_engine_camera.h"

#include <float.h>
#include <math.h>
#include <stdint.h>

extern "C" {
#include "libavutil/time.h"
#include "libavutil/log.h"
}

#ifdef SOLARIS
#include <sys/errno.h>  // for ESRCH
#include <signal.h>
#include <pthread.h>
#endif

#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#define HAVE_PRAGMA_DEPRECATED 1 // TODO by config

// -----------------------------------------------------------------------------
// callbacks to be made available to the modified ffmpeg code
// TODO: see how they could be specified without modifying the ffmpeg code
extern "C" {
	// with a new image from libavformat/img2enc.c
	int ffcb_new_image  (AVFormatContext *s, AVPacket *pkt);
	
	// with a new segment from libavformat/hls2enc.c
	int ffcb_new_segment(AVFormatContext *s, const char* filename, double duration);
	
	// patch-replaced instead of exit(code) in cmdutils.c, exit_program()
	void ffcb_exit(int code);
	
	// strftime with milliseconds at %ff (3 digits)
	size_t strftime_f_now(char* dest, size_t size, const char* fmt);
	
	// we'll register a chain exit function here, instead of patching with ffcb_exit
	// to get back in thread when ffmpeg attempts to call (exit)
	// seen in cmdutils.c
	// void register_exit(void (*cb)(int ret)); // (later)
	
	// the entry function into ffmpeg, patch-renamed from main
	int ffmpeg_main(int argc, char **argv);
}

// -----------------------------------------------------------------------------
// synchronized utility queue
template <typename type
        , typename mutex_type    = mutex
        , typename lock_type     = lock_guard<mutex>
        , typename sequence_type = deque<type>
        >
class squeue {
private:
	sequence_type sequence;
	mutable mutex_type mutex;
	
public:
	typedef typename sequence_type::value_type value_type;
	typedef typename sequence_type::reference  reference;
	typedef typename sequence_type::size_type  size_type;
	typedef typename sequence_type::iterator   iterator;
	
public:
	squeue() {
	}
	
	lock_type&& scope_lock() {
		return std::move(lock_type(mutex));
	}
	
	size_type size() const {
		//lock_type lock(mutex);
		return sequence.size();
	}
	
	bool empty() const {
		//lock_type lock(mutex);
		return sequence.empty();
	}
	
	void clear() {
		lock_type lock(mutex);
		sequence.clear();
	}
	
	void push_back(const value_type& element) {
		lock_type lock(mutex);
		sequence.push_back(element);
	}
	
	void push_back(value_type&& element) {
		lock_type lock(mutex);
		sequence.push_back(std::move(element));
	}
	
	bool extract_front(value_type& dest) {
		lock_type lock(mutex);
		if(sequence.empty())
			return false;
		
		dest = sequence.front();
		sequence.pop_front();
		return true;
	}

	bool extract_front(value_type&& dest) {
		lock_type lock(mutex);
		if(sequence.empty())
			return false;
		
		dest = std::move(sequence.front());
		sequence.pop_front();
		return true;
	}
	
	void pop_front() {
		lock_type lock(mutex);
		sequence.pop_front();
	}
	
	bool extract_back(value_type& dest) {
		lock_type lock(mutex);
		if(sequence.empty())
			return false;
		
		dest = sequence.back();
		sequence.pop_back();
		return true;
	}
	
	bool extract_back(value_type&& dest) {
		lock_type lock(mutex);
		if(sequence.empty())
			return false;
		
		dest = std::move(sequence.back());
		sequence.pop_back();
		return true;
	}
	
	template <typename dest_seq_type>
	void move_to(dest_seq_type& dest_seq) {
		lock_type lock(mutex);
		
		for(iterator it = sequence.begin(); it != sequence.end(); ++it) {
			dest_seq.push_back(*it);
		}
		
		sequence.clear();
	}
};

// synchronized sorted queue, used to try to sort by pts, not used now
template <typename type
        , typename compare_type  = less<type>
        , typename mutex_type    = mutex
        , typename lock_type     = lock_guard<mutex>
        , typename sequence_type = deque<type>
        >
class sorted_fifo {
	typedef typename sequence_type::value_type value_type;
	typedef typename sequence_type::reference  reference;
	typedef typename sequence_type::size_type  size_type;
	
	
	sequence_type sequence;
	compare_type comparator;
	mutable mutex_type mutex;
	
public:
	sorted_fifo(const compare_type& p_comparator = compare_type())
		: comparator(p_comparator)
	{
	}
	
	size_type size() const {
		lock_type lock(mutex);
		return sequence.size();
	}
	
	bool empty() const {
		lock_type lock(mutex);
		return sequence.empty();
	}
	
	void push(const value_type& element) {
		lock_type lock(mutex);
		sequence.push_back(element);
		std::push_heap(sequence.begin(), sequence.end(), comparator);
	}
	
#if __cplusplus >= 201103L
	void push(value_type&& element) {
		lock_type lock(mutex);
		sequence.push_back(std::move(element));
		std::push_heap(sequence.begin(), sequence.end(), comparator);
	}
#endif
	
	value_type pop_front() {
		lock_type lock(mutex);
	//std::pop_heap(sequence.begin(), sequence.end(), comparator);
		//value_type result = std::move(sequence.back());
	//sequence.pop_back();
		value_type result = std::move(sequence.front());
		sequence.pop_front();
		return result;
	}
};

// -----------------------------------------------------------------------------
// class encapsulating an AVPacket, to automate resource releasing
// it has to work like a shared_ptr for the AVPacket buffer
class AVSharedPacket: public AVPacket {
public:
	AVSharedPacket() {
		av_init_packet(this);
		//av_packet_unref(this); // done in av_packet_alloc too
	}
	
	// copy constructor
	AVSharedPacket(const AVSharedPacket& other) {
		av_packet_ref(this, &other);
	}
	
	// move constructor
	// ffmpeg assumes that memcpy(sizeof(AVPacket)) is cheap
	// so it only reference counts the buffer
	AVSharedPacket(AVSharedPacket&& other) {
		av_packet_move_ref(this, &other);
	}
	
	~AVSharedPacket() {
		av_packet_unref(this); // done in av_packet_free too
	}
	
	// copy operator
	AVSharedPacket& operator=(const AVSharedPacket& other) {
		av_packet_unref(this);
		av_packet_ref(this, &other);
		return *this;
	}
	
	// Move assignment operator
	AVSharedPacket& operator =(AVSharedPacket&& other) {
		av_packet_unref(this); // in case it owned resources
		av_packet_move_ref(this, &other);
		return *this;
	}
	
	void move_to(AVPacket& dst) {
		av_packet_move_ref(&dst, this);
	}
	
	void move_from(AVPacket& src) {
		av_packet_move_ref(this, &src);
	}
	
	int read_frame(AVFormatContext* s) {
		return av_read_frame(s, this);
	}
	
	// maybe we'll use a memory pool here if needed
	void* operator new(size_t size) {
		// cout << "AVPacketCPP::new(" << size << ")" << endl;
		return av_mallocz(sizeof(AVSharedPacket));
	}
	void operator delete(void* mem) {
		// cout << "AVPacketCPP::delete()" << endl;
		av_freep(&mem);
	}
};

static_assert(sizeof(AVSharedPacket) == sizeof(AVPacket),
	              "don't extend the structure for now");

class Segment {
public:
	Segment(const char* p_filename, chrono::system_clock::time_point p_start, chrono::milliseconds p_duration)
		: m_filename(p_filename)
		, m_start(p_start)
		, m_duration(p_duration)
	{
	}
		
	virtual ~Segment() {}
	
	const char*           filename() const { return m_filename.c_str(); }
	chrono::milliseconds  duration() const { return m_duration; }
	
protected:
	string m_filename;
	//char m_filename[PATH_MAX - sizeof(double) - sizeof(Segment*)]; // trying to keep round blocks
	chrono::system_clock::time_point m_start;
	chrono::milliseconds m_duration;

};

static squeue<string> __file_deletion_queue;

class TempSegment: public Segment {
public:
	TempSegment(const char* p_filename, chrono::system_clock::time_point p_start, chrono::milliseconds p_duration)
		: Segment(p_filename, p_start, p_duration) {}
	
	virtual ~TempSegment() override {
		//if(remove(m_filename.c_str()) != 0) {
		//    int err = errno;
		//    Warning("Cannot delete, errno=%d, file:%s", err, m_filename.c_str());
			__file_deletion_queue.push_back(std::move(m_filename));
		//}
	}
};

class SegmentStore {
public:
	string m_filename;
	deque<shared_ptr<Segment>> m_queue;
	chrono::system_clock::time_point m_start;
	chrono::system_clock::time_point m_end;

	SegmentStore(string p_filename, chrono::system_clock::time_point p_start)
		: m_filename(p_filename)
		, m_start(p_start)
	{
	}
};

// comparators for AVPacket by pts (presentation time stamp)
struct less_packet_pts: public binary_function<AVPacket, AVPacket, bool>
{
	bool operator()(const AVPacket& x, const AVPacket& y) const
	{
		return x.pts < y.pts;
	}
};

struct greater_packet_pts: public binary_function<AVPacket, AVPacket, bool>
{
	bool operator()(const AVPacket& x, const AVPacket& y) const
	{
		return x.pts > y.pts;
	}
};

// -----------------------------------------------------------------------------
class FfmpegEngineCamera : public Camera
{
public:
	FfmpegEngineCamera(int p_id, const std::string &path, const std::string &p_method, const std::string &p_options, int p_width, int p_height, int p_colours, int p_brightness, int p_contrast, int p_hue, int p_colour, bool p_capture, bool p_record_audio, double p_analysis_fps);
	virtual ~FfmpegEngineCamera() override;
	
	virtual int PrimeCapture() override;
	virtual int PreCapture() override;
	virtual int Capture(Image &image) override;
	virtual int PostCapture() override;
	virtual int CaptureAndRecord(Image &image, bool recording, char* event_directory) override;
	
	int ffcb_new_image  (AVFormatContext *s, AVPacket *pkt);
	int ffcb_new_segment(AVFormatContext *s, const char* filename, double duration);
	
	void DoBackgroundWork();
	
protected:
	void AddToFfmpegParameters();
	void ProcessVideo(bool recording, char* event_file);
	
	std::string         mPath;
	std::string         mMethod;
	std::string         mOptions;
	std::string         mLivePlaylist;
	double              mAnalysisFps;
	
	// the waiting lists are coming from the ffmpeg thread, need synchronizing
	squeue<AVSharedPacket>   mWaitingImages;
	// using shared_ptr for segments, in case they are temporary files
	//     that will be deleted in the destructor
	// they may be in multiple lists, waiting to be processed
	squeue< shared_ptr<Segment> > mWaitingSegments;
	// list for the most recent segments, being streamed live
	// used to keep a reference on them so they don't get deleted
	squeue< shared_ptr<Segment> > mLiveSegmentsMRU;
	
	// this list accumulates segments for the current requested video file
	shared_ptr<SegmentStore> mCurrentStore;
	// stores waiting for concatenation
	squeue< shared_ptr<SegmentStore> > mWaitingStores;
	
	size_t mNumberOfLiveSegments;
	
	//mutex mSegmentsMutex;
	//typedef lock_guard<mutex> lock_type;
};

// this function is the only external interface to this file for now
Camera* newFfmpegEngineCamera(int p_id, const std::string &path, const std::string &p_method, const std::string &p_options, int p_width, int p_height, int p_colours, int p_brightness, int p_contrast, int p_hue, int p_colour, bool p_capture, bool p_record_audio, double p_analysis_fps) {
	return new FfmpegEngineCamera(p_id, path, p_method, p_options, p_width, p_height, p_colours, p_brightness, p_contrast, p_hue, p_colour, p_capture, p_record_audio, p_analysis_fps);
}

// -----------------------------------------------------------------------------
// static variables and functions, unique per process
static void ffmpegThreadFunction();
static bool ffmpegInitEngine();
static void backgroundThreadFunction();
static bool __isEngineInitialized = false;
static bool __isCapturePrimed = false;
static vector<string> __ffmpeg_argv;
static thread         __ffmpeg_thread;
static thread         __background_thread;
static mutex          __background_mutex;
// keep the key an int, to avoid repeated allocations of std::string in the callbacks
static map<int, FfmpegEngineCamera*> __cameras_map;
static const char* META_KEY = "zm-key";
//static fifo<string> __file_deletion_queue; // declared just above TempSegment
//static const char* QUOTE = "\"";
//sorted_fifo<AVPacketCPP, greater_packet_pts > m_inputQueue; // tests
//fifo<AVPacketCPP> m_inputQueue; // tests
//int m_inputState = 0; // tests


// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// a strftime with milliseconds, TODO try to make it in C
size_t strftime_f(char* dest, size_t size, const char* fmt, chrono::system_clock::time_point tp) {
	
	auto ttime_t = chrono::system_clock::to_time_t(tp);
	auto tp_sec  = chrono::system_clock::from_time_t(ttime_t);
	chrono::milliseconds ms = chrono::duration_cast<chrono::milliseconds>(tp - tp_sec);

	tm * ttm = localtime(&ttime_t);
	size_t written = strftime(dest, size, fmt, ttm);
	
	char* p = strstr(dest, "%ff");
	const int fflen = 3;
	if(p != NULL && p + fflen + 1 < dest + size) {
		char saved;
		saved = p[fflen];
		
		snprintf(p, fflen+1, "%03ld", ms.count());
		
		p[fflen] = saved;
	}

	return written;
}

size_t strftime_f_now(char* dest, size_t size, const char* fmt) {
	return strftime_f(dest, size, fmt, chrono::system_clock::now());
}

// -----------------------------------------------------------------------------
// a map finder that returns NULL, to avoid allocating an empty entry
template <typename KEY, typename T>
T* find(const map<KEY, T*>& map, const KEY& key) {
	typename map<KEY, T*>::const_iterator it = map.find(key);
	if(it == map.end())
		return NULL;
	
	return it->second;
}

// to be moved in zm_logger.h
class ScopeMarker {
	Logger::Level level;
	const char* method_name;
	const char* file_name;
	int line;
	
public:
	ScopeMarker(Logger::Level p_level, const char* p_method_name, const char* p_file_name, int p_line)
		: level(p_level)
		, method_name(p_method_name)
		, file_name(p_file_name)
		, line(p_line)
	{
		if(level <= Logger::fetch()->level())
			Logger::fetch()->logPrint(false, file_name, line, level, "Entering %s", method_name);
	}
	
	~ScopeMarker() {
		if(level <= Logger::fetch()->level())
			Logger::fetch()->logPrint(false, file_name, line, level, "Exiting %s", method_name);
	}
};

#ifndef DBG_OFF
#define DebugScope(level) ScopeMarker __scopeMarker__(level, __PRETTY_FUNCTION__, __FILE__, __LINE__);
#else
#define DebugScope(level)
#endif

// -----------------------------------------------------------------------------
// helper insertion functions
// no templates here, specializations are less hassle
vector<string>& operator <<(vector<string>& sequence, const string& elem) {
	sequence.push_back(elem);
	return sequence;
}

vector<string>& operator <<(vector<string>& sequence, const ostringstream& elem) {
	sequence.push_back(elem.str());
	return sequence;
}

vector<string>& operator <<(vector<string>& sequence, const char* elem) {
	sequence.push_back(elem);
	return sequence;
}

vector<string>& operator <<(vector<string>& sequence, int d) {
	char buf[10];
	snprintf(buf, sizeof(buf), "%d", d);
	sequence.push_back(buf);
	return sequence;
}

vector<string>& operator <<(vector<string>& sequence, double f) {
	char buf[10];
	snprintf(buf, sizeof(buf), "%f", f);
	sequence.push_back(buf);
	return sequence;
}

// if it's too confusing to have insertion operators for both string and vector
// either one has to go
string& operator <<(string& os, const char* const s) {
	return os.append(s);
}

string& operator <<(string&& os, const char* const s) {
	return os.append(s);
}

string& operator <<(string& os, const string& s) {
	return os.append(s);
}

string& operator <<(string&& os, const string& s) {
	return os.append(s);
}

string& operator <<(string&& os, int d) {
	char buf[30];
	snprintf(buf, sizeof(buf), "%d", d);
	return os.append(buf);
}

string& operator <<(string& os, int d) {
	char buf[30];
	snprintf(buf, sizeof(buf), "%d", d);
	return os.append(buf);
}

// -----------------------------------------------------------------------------
void wait(thread& thread) {
	if(thread.joinable())
		thread.join();
}

// -----------------------------------------------------------------------------
// to be used in preparations for api deprecations in ffmpeg 3
AVMediaType streamCodecType(AVFormatContext* ctx, int iStream) {
	return ctx->streams[iStream]->codec->codec_type;
	//return ctx->streams[iStream]->codecpar->codec_type;
}

// -----------------------------------------------------------------------------
const char* zm_color_to_ffargs(int p_colours) {
	// see libavutil/pixdesc.c
	switch(p_colours) {
		case ZM_COLOUR_RGB32:
			return "rgba";
			
		case ZM_COLOUR_RGB24:
			return "rgb24";
			
		case ZM_COLOUR_GRAY8:
			return "gray";
			
		default:
			Panic("Unexpected colours: %d", p_colours);
			return NULL;
	}
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
FfmpegEngineCamera::FfmpegEngineCamera(int p_id, const std::string &p_path, const std::string &p_method, const std::string &p_options, int p_width, int p_height, int p_colours, int p_brightness, int p_contrast, int p_hue, int p_colour, bool p_capture, bool p_record_audio, double p_analysis_fps)
	: Camera(p_id, FFMPEG_SRC, p_width, p_height, p_colours, ZM_SUBPIX_ORDER_DEFAULT_FOR_COLOUR(p_colours), p_brightness, p_contrast, p_hue, p_colour, p_capture, p_record_audio )
	, mPath(p_path)
	, mMethod(p_method)
	, mOptions(p_options)
	, mAnalysisFps(p_analysis_fps)
	, mNumberOfLiveSegments(7) // TODO: make it configurable
	{
	__cameras_map[monitor_id] = this;
	
	AddToFfmpegParameters();
	
#if 0
	mFormatContext = NULL;
	mVideoStreamId = -1;
	mAudioStreamId = -1;
	mVideoCodecContext = NULL;
	mAudioCodecContext = NULL;
	mVideoCodec = NULL;
	mAudioCodec = NULL;
	mRawFrame = NULL;
	mFrame = NULL;
	frameCount = 0;
	startTime=0;
	mIsOpening = false;
	mCanCapture = false;
	mOpenStart = 0;
	mReopenThread = 0;
	videoStore = NULL;
	
#if HAVE_LIBSWSCALE  
	mConvertContext = NULL;
#endif
	/* Has to be located inside the constructor so other components such as zma will receive correct colours and subpixel order */
	if(colours == ZM_COLOUR_RGB32) {
		subpixelorder = ZM_SUBPIX_ORDER_RGBA;
		imagePixFormat = AV_PIX_FMT_RGBA;
	} else if(colours == ZM_COLOUR_RGB24) {
		subpixelorder = ZM_SUBPIX_ORDER_RGB;
		imagePixFormat = AV_PIX_FMT_RGB24;
	} else if(colours == ZM_COLOUR_GRAY8) {
		subpixelorder = ZM_SUBPIX_ORDER_NONE;
		imagePixFormat = AV_PIX_FMT_GRAY8;
	} else {
		Panic("Unexpected colours: %d",colours);
	}
#endif
	if(capture) {
		if(__isEngineInitialized)
			return;
		
		__isEngineInitialized = ffmpegInitEngine();
	}
}

FfmpegEngineCamera::~FfmpegEngineCamera() {
	
	mWaitingSegments.clear();
	mLiveSegmentsMRU.clear();
	mWaitingStores.clear();
	mCurrentStore = NULL;
	__file_deletion_queue.push_back(mLivePlaylist);
	// here see how we can inform the engine
#if 0
	CloseFfmpeg();
	
	if(capture) {
	  Terminate();
	}
#endif
	bool isLast = false;
	
	{
		lock_guard<mutex> lock(__background_mutex);
		__cameras_map.erase(monitor_id);
		if(__cameras_map.empty())
			isLast = true;
	}
	
	// TODO: try to see how we can remove the input stream from the ffmpeg engine
	
	// if we're the last one, wait for ffmpeg to finish
	// when it detects -1 returned from the callbacks
	if(isLast) {
		wait(__ffmpeg_thread);
		wait(__background_thread);
	}
}

// add to the command line parameters vector, continuing from the previous one
void FfmpegEngineCamera::AddToFfmpegParameters() {
	
	// TODO: use the store from the database
	string eventsRoot = string() 
		<< staticConfig.PATH_WEB << "/" << config.dir_events << "/" << monitor_id;
	mLivePlaylist = string() << eventsRoot << "/live.m3u8";
	bool useDeeperScheme = true;
	
	// try to see if unix user can write into it
	if((ofstream(mLivePlaylist, ios::out | ios::ate) << endl << flush).fail()) {
		Warning("File not writable: %s", mLivePlaylist.c_str());
		// this alternative is for interactive debugging, not meant for the end user
		(eventsRoot = "/tmp/zm-work/") << monitor_id;
		(mLivePlaylist = eventsRoot) << "/live.m3u8";
		useDeeperScheme = false;
	}
	
	const string eventsPath = string(eventsRoot) << (useDeeperScheme
	                            ? "/%y/%m/%d/%H/%M-%S.%ff" 
	                            : "/%Y-%m-%d/%H-%M/%S.%ff"
	                            );
	
	if(__ffmpeg_argv.empty()) {
		__ffmpeg_argv.reserve(120);
		__ffmpeg_argv << "ffmpeg_zmc";
	}
	
	if(mMethod == "rtpMulti") {
		__ffmpeg_argv << "-rtsp_transport" << "udp_multicast";
	} else if(mMethod == "rtpRtsp") {
		__ffmpeg_argv << "-rtsp_transport" << "tcp";
	} else if(mMethod == "rtpRtspHttp") {
		__ffmpeg_argv << "-rtsp_transport" << "http";
	}
	
	// TODO: parse and include options
	
	string meta = string() << META_KEY << "=" << monitor_id;
	
	__ffmpeg_argv
		// start first input spec
		<< "-i" << mPath
			// start first output spec
			<< "-metadata" << meta
				//<< "-f" << "image2zma" // not used, since image2 is hardcoded in a few places inside the libs; see in ffmpegInitEngine
				<< "-r" << mAnalysisFps
				<< "-vf" << (string() << "scale=" << width << ":" << height << ",vflip")
				<< "-pix_fmt" << zm_color_to_ffargs(this->colours)
				<< "-strftime" << 1
				<< string(eventsPath).append(".bmp")
			
			// start second output spec
			<< "-metadata" << meta
				<< "-vcodec" << "copy"
				<< "-map" << 0
				<< "-f" << "zm-hls"
				//<< "-hls_list_size" << "0"
				<< "-use_localtime" << 1
				<< "-use_localtime_mkdir" << 1
				<< "-hls_flags" << "program_date_time"
				<< "-hls_segment_filename" << string(eventsPath).append(".mp4")
				<< mLivePlaylist
			
		;
}

int FfmpegEngineCamera::PrimeCapture() {
	if(__isCapturePrimed)
		return 0;
	
	Info("Priming capture from ffmpeg engine");
#if 0
	mVideoStreamId = -1;
	mAudioStreamId = -1;
	Info("Priming capture from %s", mPath.c_str());
	
	if(OpenFfmpeg() != 0) {
		ReopenFfmpeg();
	}
#endif
	
	if(!__ffmpeg_thread.joinable())
		__ffmpeg_thread = thread(&ffmpegThreadFunction);
	
	// if background thread is launched in constructor,
	// coming from static Monitor::Load, will segfault
	if(!__background_thread.joinable())
		__background_thread = thread(&backgroundThreadFunction);
	
	__isCapturePrimed = true;
	
	//__ffmpeg_thread.join(); // wait for now, for tests
	
	return 0;
}

int FfmpegEngineCamera::PreCapture() {
	// Nothing to do here
	return 0;
}

int FfmpegEngineCamera::PostCapture() {
	// Nothing to do here
	return 0;
}

int FfmpegEngineCamera::Capture(Image &image) {
	//if(!mCanCapture)
	//    return -1;
	
	AVSharedPacket packet;
	bool gotData = false;
	// TODO: make those configurable
	const int BUFFER_WAIT_MILLISECONDS = 100;
	const int BUFFER_RETRIES = 50;
	
	for(int i = 0; i < BUFFER_RETRIES; i++) {
		if(mWaitingImages.extract_back(packet)) {
			gotData = true;
			mWaitingImages.clear(); // just using the most recent image for analysis
			break;
		}
		
		this_thread::sleep_for(chrono::milliseconds(BUFFER_WAIT_MILLISECONDS));
	}
	
	if(!gotData) {
		Error("Timeout while waiting for ffmpeg buffer");
		return -1;
	}
	
	if(!packet.buf) {
		Error("Empty packet from ffmpeg");
		return -1;
	}
	
	/* Request a writeable buffer of the target image */
	uint8_t* directbuffer = image.WriteBuffer(width, height, colours, subpixelorder);
	if(!directbuffer) {
		Error("Failed requesting writeable buffer for the captured image.");
		return -1;
	}
#if 1
	// Windows BMP files begin with a 54-byte header
	const size_t BMP_HEADER_SIZE = 54;
	if(image.Size() + BMP_HEADER_SIZE != (size_t)packet.size) {
		Error("Packet size from ffmpeg doesn't match");
		return -1;
	}

	memcpy(directbuffer, packet.data + BMP_HEADER_SIZE, image.Size());
#else
	int content_length = packet.size;
	if(!image.DecodeJpeg(packet.buf, content_length, colours, ZM_SUBPIX_ORDER_DEFAULT_FOR_COLOUR(colours))) {
		Error("Cannot decode jpeg");
		return -1;
	}
#endif

	Debug(Logger::DEBUG9, "Image transferred");
	
	return 0;
}

// this comes in the main thread
int FfmpegEngineCamera::CaptureAndRecord(Image &image, bool recording, char* event_file) {
	//if(!mCanCapture)
	//    return -1;
	ProcessVideo(recording, event_file);
	
	return Capture(image);
}

// this comes in the main thread
// just passing segments around lists, it should not do long operations
// anything longer will be enqueued for DoBackgroundWork
void FfmpegEngineCamera::ProcessVideo(bool recording, char* event_file) {
	//lock_type lock(mSegmentsMutex);
	
	auto now = chrono::system_clock::now();
	static const Logger::Level debugLevel = Logger::DEBUG4;
	
	if(recording) {
		if(!mCurrentStore) {
			// new recording started
			Debug(debugLevel, "Starting new store %s", event_file);
			mCurrentStore = shared_ptr<SegmentStore>(new SegmentStore(event_file, now));
			mWaitingSegments.move_to(mCurrentStore->m_queue);
		} else {
			// verify if name changed
			if(event_file && strcmp(event_file, mCurrentStore->m_filename.c_str()) != 0) {
				// TODO: copy pertaining segments from queue to the current store
				//       if the start time overlaps
				Debug(debugLevel, "Ending store %s", mCurrentStore->m_filename.c_str());
				mCurrentStore->m_end = now;
				mWaitingStores.push_back(mCurrentStore);
				Debug(debugLevel, "Starting new store %s", event_file);
				mCurrentStore = shared_ptr<SegmentStore>(new SegmentStore(event_file, now));
			}
			
			mWaitingSegments.move_to(mCurrentStore->m_queue);
		}
	} else { // not recording
		if(mCurrentStore.get()) {
			Debug(debugLevel, "Ending store %s", mCurrentStore->m_filename.c_str());
			mWaitingSegments.move_to(mCurrentStore->m_queue);
			mCurrentStore->m_end = now;
			mWaitingStores.push_back(mCurrentStore);
			mCurrentStore = NULL;
		} else {
			mWaitingSegments.clear(); // just make sure no doubles
		}
	}

	// all if/else branches above must clear mWaitingSegments
	// we cannot clear it here, because the ffmpeg thread can insert just after
	// move_to(...), but before arriving here
}

void FfmpegEngineCamera::DoBackgroundWork() {
	DebugScope(Logger::DEBUG9);
	
	shared_ptr<SegmentStore> store;
	while(mWaitingStores.extract_front(store)) {
		if(store->m_queue.empty()) {
			Info("No segments accumulated for target: %s", store->m_filename.c_str());
			continue;
		}
		
		Debug(Logger::DEBUG4, "concatenating segments for %s", store->m_filename.c_str());
		string listFileName = string() << store->m_filename << "." << "txt";
		
		{
			ofstream os(listFileName);
			for(auto it = store->m_queue.begin(); it != store->m_queue.end(); ++it) {
				auto segment = (*it);
				os << "file '" << segment->filename() << "'" << endl;
			}
		}
		
		// using concat demuxer, out of process for now
		// TODO: see cutting small sections in https://trac.ffmpeg.org/wiki/Seeking
		string cmd = string() << "ffmpeg -f concat -safe 0 -i " << listFileName
				<< " -c copy " << store->m_filename;
		
		Debug(Logger::DEBUG4, "executing: %s", cmd.c_str());
		int ret = std::system(cmd.c_str());
		Debug(Logger::DEBUG4, "ffmpeg returned with code %d", ret);
		
		__file_deletion_queue.push_back(listFileName);
	}
}

// callback, this comes in the ffmpeg thread
int FfmpegEngineCamera::ffcb_new_image(AVFormatContext *s, AVPacket *pkt) {
	AVSharedPacket packet;
	packet.move_from(*pkt); // don't allocate a new buffer
	mWaitingImages.push_back(std::move(packet));
	return 0;
}

// callback, this comes in the ffmpeg thread
int FfmpegEngineCamera::ffcb_new_segment(AVFormatContext *s, const char* filename, double duration) {
	chrono::system_clock::time_point tp = chrono::system_clock::now();
	Debug(Logger::DEBUG9, "ffcb_new_segment(%s)", filename);
	
	auto us = chrono::duration_cast<chrono::microseconds>(chrono::duration<double>(duration));
	tp -= us;
	auto ms = chrono::duration_cast<chrono::milliseconds>(us);
	shared_ptr<Segment> sptr(new TempSegment(filename, tp, ms));
	//lock_type lock(mSegmentsMutex);
	mWaitingSegments.push_back(sptr);
	mLiveSegmentsMRU.push_back(sptr);
	
	while(mLiveSegmentsMRU.size() > mNumberOfLiveSegments)
		mLiveSegmentsMRU.pop_front();
	
	return 0;
}


// -----------------------------------------------------------------------------
// code copied from the ffmpeg project is in namespaces, according to the origin
// -----------------------------------------------------------------------------
namespace libavformat_avformat_h {
#define AV_FRAME_FILENAME_FLAGS_MULTIPLE 1 ///< Allow multiple %d
}

namespace libavformat_aviobuf_c {

int ff_get_line(AVIOContext *s, char *buf, int maxlen)
{
    int i = 0;
    char c;

    do {
        c = avio_r8(s);
        if (c && i < maxlen-1)
            buf[i++] = c;
    } while (c != '\n' && c != '\r' && c);
    if (c == '\r' && avio_r8(s) != '\n' && !avio_feof(s))
        avio_skip(s, -1);

    buf[i] = 0;
    return i;
}

} // namespace libavformat_aviobuf_c

namespace libavformat_utils_c {
    
char *ff_data_to_hex(char *buff, const uint8_t *src, int s, int lowercase)
{
    int i;
    static const char hex_table_uc[16] = { '0', '1', '2', '3',
                                           '4', '5', '6', '7',
                                           '8', '9', 'A', 'B',
                                           'C', 'D', 'E', 'F' };
    static const char hex_table_lc[16] = { '0', '1', '2', '3',
                                           '4', '5', '6', '7',
                                           '8', '9', 'a', 'b',
                                           'c', 'd', 'e', 'f' };
    const char *hex_table = lowercase ? hex_table_lc : hex_table_uc;

    for (i = 0; i < s; i++) {
        buff[i * 2]     = hex_table[src[i] >> 4];
        buff[i * 2 + 1] = hex_table[src[i] & 0xF];
    }

    return buff;
}

void avpriv_set_pts_info(AVStream *s, int pts_wrap_bits,
                         int pts_num, unsigned int pts_den)
{
    AVRational new_tb;
    if (av_reduce(&new_tb.num, &new_tb.den, pts_num, pts_den, INT_MAX)) {
        if (new_tb.num != pts_num)
            av_log(NULL, AV_LOG_DEBUG,
                   "st:%d removing common factor %d from timebase\n",
                   s->index, pts_num / new_tb.num);
    } else
        av_log(NULL, AV_LOG_WARNING,
               "st:%d has too large timebase, reducing\n", s->index);

    if (new_tb.num <= 0 || new_tb.den <= 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Ignoring attempt to set invalid timebase %d/%d for st:%d\n",
               new_tb.num, new_tb.den,
               s->index);
        return;
    }
    s->time_base     = new_tb;
    av_codec_set_pkt_timebase(s->codec, new_tb);
    s->pts_wrap_bits = pts_wrap_bits;
}

int av_get_frame_filename2(char *buf, int buf_size, const char *path, int number, int flags)
{
    const char *p;
    char *q, buf1[20], c;
    int nd, len, percentd_found;

    q = buf;
    p = path;
    percentd_found = 0;
    for (;;) {
        c = *p++;
        if (c == '\0')
            break;
        if (c == '%') {
            do {
                nd = 0;
                while (av_isdigit(*p))
                    nd = nd * 10 + *p++ - '0';
                c = *p++;
            } while (av_isdigit(c));

            switch (c) {
            case '%':
                goto addchar;
            case 'd':
                if (!(flags & AV_FRAME_FILENAME_FLAGS_MULTIPLE) && percentd_found)
                    goto fail;
                percentd_found = 1;
                if (number < 0)
                    nd += 1;
                snprintf(buf1, sizeof(buf1), "%0*d", nd, number);
                len = strlen(buf1);
                if ((q - buf + len) > buf_size - 1)
                    goto fail;
                memcpy(q, buf1, len);
                q += len;
                break;
            default:
                goto fail;
            }
        } else {
addchar:
            if ((q - buf) < buf_size - 1)
                *q++ = c;
        }
    }
    if (!percentd_found)
        goto fail;
    *q = '\0';
    return 0;
fail:
    *q = '\0';
    return -1;
}

} // namespace libavformat_utils_c

namespace libavformat_internal_h {
    
/**
 * Wrap errno on rename() error.
 *
 * @param oldpath source path
 * @param newpath destination path
 * @return        0 or AVERROR on failure
 */
static inline int ff_rename(const char *oldpath, const char *newpath, void *logctx)
{
    int ret = 0;
    if (rename(oldpath, newpath) == -1) {
        ret = AVERROR(errno);
        if (logctx)
            av_log(logctx, AV_LOG_ERROR, "failed to rename file %s to %s\n", oldpath, newpath);
    }
    return ret;
}

} // namespace libavformat_internal_h

namespace libavutil_internal_h {
	
/**
 * Return NULL if CONFIG_SMALL is true, otherwise the argument
 * without modification. Used to disable the definition of strings
 * (for example AVCodec long_names).
 */
#if CONFIG_SMALL
#   define NULL_IF_CONFIG_SMALL(x) NULL
#else
#   define NULL_IF_CONFIG_SMALL(x) x
#endif

#if HAVE_PRAGMA_DEPRECATED
#    if defined(__ICL) || defined (__INTEL_COMPILER)
#        define FF_DISABLE_DEPRECATION_WARNINGS __pragma(warning(push)) __pragma(warning(disable:1478))
#        define FF_ENABLE_DEPRECATION_WARNINGS  __pragma(warning(pop))
#    elif defined(_MSC_VER)
#        define FF_DISABLE_DEPRECATION_WARNINGS __pragma(warning(push)) __pragma(warning(disable:4996))
#        define FF_ENABLE_DEPRECATION_WARNINGS  __pragma(warning(pop))
#    else
#        define FF_DISABLE_DEPRECATION_WARNINGS _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#        define FF_ENABLE_DEPRECATION_WARNINGS  _Pragma("GCC diagnostic warning \"-Wdeprecated-declarations\"")
#    endif
#else
#    define FF_DISABLE_DEPRECATION_WARNINGS
#    define FF_ENABLE_DEPRECATION_WARNINGS
#endif

} // libavutil_internal_h

namespace libavformat_mux_c {
    
//#include "ffmpeg/libavutil/internal.h"

int ff_write_chained(AVFormatContext *dst, int dst_stream, AVPacket *pkt,
                     AVFormatContext *src, int interleave)
{
    AVPacket local_pkt;
    int ret;

    local_pkt = *pkt;
    local_pkt.stream_index = dst_stream;
    if (pkt->pts != AV_NOPTS_VALUE)
        local_pkt.pts = av_rescale_q(pkt->pts,
                                     src->streams[pkt->stream_index]->time_base,
                                     dst->streams[dst_stream]->time_base);
    if (pkt->dts != AV_NOPTS_VALUE)
        local_pkt.dts = av_rescale_q(pkt->dts,
                                     src->streams[pkt->stream_index]->time_base,
                                     dst->streams[dst_stream]->time_base);
    if (pkt->duration)
        local_pkt.duration = av_rescale_q(pkt->duration,
                                          src->streams[pkt->stream_index]->time_base,
                                          dst->streams[dst_stream]->time_base);

    if (interleave) ret = av_interleaved_write_frame(dst, &local_pkt);
    else            ret = av_write_frame(dst, &local_pkt);
    pkt->buf = local_pkt.buf;
    pkt->side_data       = local_pkt.side_data;
    pkt->side_data_elems = local_pkt.side_data_elems;
#if FF_API_DESTRUCT_PACKET
FF_DISABLE_DEPRECATION_WARNINGS
    pkt->destruct = local_pkt.destruct;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    return ret;
}

} // namespace libavformat_mux_c

namespace libavformat_hlsenc_c { // from ffmpeg 3.2 for the latest features
    
//#include "ffmpeg/libavutil/internal.h"
    
using namespace libavformat_aviobuf_c;
using namespace libavformat_internal_h;
using namespace libavformat_mux_c;
using namespace libavformat_utils_c;

#define KEYSIZE 16
#define LINE_BUFFER_SIZE 1024

typedef struct HLSSegment {
    char filename[1024];
    char sub_filename[1024];
    double duration; /* in seconds */
    int discont;
    int64_t pos;
    int64_t size;

    char key_uri[LINE_BUFFER_SIZE + 1];
    char iv_string[KEYSIZE*2 + 1];

    struct HLSSegment *next;
} HLSSegment;

typedef enum HLSFlags {
    // Generate a single media file and use byte ranges in the playlist.
    HLS_SINGLE_FILE = (1 << 0),
    HLS_DELETE_SEGMENTS = (1 << 1),
    HLS_ROUND_DURATIONS = (1 << 2),
    HLS_DISCONT_START = (1 << 3),
    HLS_OMIT_ENDLIST = (1 << 4),
    HLS_SPLIT_BY_TIME = (1 << 5),
    HLS_APPEND_LIST = (1 << 6),
    HLS_PROGRAM_DATE_TIME = (1 << 7),
    HLS_SECOND_LEVEL_SEGMENT_INDEX = (1 << 8), // include segment index in segment filenames when use_localtime  e.g.: %%03d
    HLS_SECOND_LEVEL_SEGMENT_DURATION = (1 << 9), // include segment duration (microsec) in segment filenames when use_localtime  e.g.: %%09t
    HLS_SECOND_LEVEL_SEGMENT_SIZE = (1 << 10), // include segment size (bytes) in segment filenames when use_localtime  e.g.: %%014s
} HLSFlags;

typedef enum {
    PLAYLIST_TYPE_NONE,
    PLAYLIST_TYPE_EVENT,
    PLAYLIST_TYPE_VOD,
    PLAYLIST_TYPE_NB,
} PlaylistType;

typedef struct HLSContext {
    const AVClass *clazs;  // Class for private options.
    unsigned number;
    int64_t sequence;
    int64_t start_sequence;
    AVOutputFormat *oformat;
    AVOutputFormat *vtt_oformat;

    AVFormatContext *avf;
    AVFormatContext *vtt_avf;

    float time;            // Set by a private option.
    float init_time;       // Set by a private option.
    int max_nb_segments;   // Set by a private option.
    int  wrap;             // Set by a private option.
    uint32_t flags;        // enum HLSFlags
    uint32_t pl_type;      // enum PlaylistType
    char *segment_filename;

    int use_localtime;      ///< flag to expand filename with localtime
    int use_localtime_mkdir;///< flag to mkdir dirname in timebased filename
    int allowcache;
    int64_t recording_time;
    int has_video;
    int has_subtitle;
    double dpp;           // duration per packet
    int64_t start_pts;
    int64_t end_pts;
    double duration;      // last segment duration computed so far, in seconds
    int64_t start_pos;    // last segment starting position
    int64_t size;         // last segment size
    int64_t max_seg_size; // every segment file max size
    int nb_entries;
    int discontinuity_set;
    int discontinuity;

    HLSSegment *segments;
    HLSSegment *last_segment;
    HLSSegment *old_segments;

    char *basename;
    char *vtt_basename;
    char *vtt_m3u8_name;
    char *baseurl;
    char *format_options_str;
    char *vtt_format_options_str;
    char *subtitle_filename;
    AVDictionary *format_options;

    char *key_info_file;
    char key_file[LINE_BUFFER_SIZE + 1];
    char key_uri[LINE_BUFFER_SIZE + 1];
    char key_string[KEYSIZE*2 + 1];
    char iv_string[KEYSIZE*2 + 1];
    AVDictionary *vtt_format_options;

    char *method;

    double initial_prog_date_time;
    char current_segment_final_filename_fmt[1024]; // when renaming segments
} HLSContext;

static int get_int_from_double(double val)
{
    return (int)((val - (int)val) >= 0.001) ? (int)(val + 1) : (int)val;
}

static int mkdir_p(const char *path) {
    int ret = 0;
    char *temp = av_strdup(path);
    char *pos = temp;
    char tmp_ch = '\0';

    if (!path || !temp) {
        return -1;
    }

    if (!strncmp(temp, "/", 1) || !strncmp(temp, "\\", 1)) {
        pos++;
    } else if (!strncmp(temp, "./", 2) || !strncmp(temp, ".\\", 2)) {
        pos += 2;
    }

    for ( ; *pos != '\0'; ++pos) {
        if (*pos == '/' || *pos == '\\') {
            tmp_ch = *pos;
            *pos = '\0';
            ret = mkdir(temp, S_IRWXU | S_IRWXG);
            *pos = tmp_ch;
        }
    }

    if ((*(pos - 1) != '/') || (*(pos - 1) != '\\')) {
        ret = mkdir(temp, S_IRWXU | S_IRWXG);
    }

    av_free(temp);
    return ret;
}

static int replace_int_data_in_filename(char *buf, int buf_size, const char *filename, char placeholder, int64_t number)
{
    const char *p;
    char *q, buf1[20], c;
    int nd, len, addchar_count;
    int found_count = 0;

    q = buf;
    p = filename;
    for (;;) {
        c = *p;
        if (c == '\0')
            break;
        if (c == '%' && *(p+1) == '%')  // %%
            addchar_count = 2;
        else if (c == '%' && (av_isdigit(*(p+1)) || *(p+1) == placeholder)) {
            nd = 0;
            addchar_count = 1;
            while (av_isdigit(*(p + addchar_count))) {
                nd = nd * 10 + *(p + addchar_count) - '0';
                addchar_count++;
            }

            if (*(p + addchar_count) == placeholder) {
                len = snprintf(buf1, sizeof(buf1), "%0*" PRId64, (number < 0) ? nd : nd++, number);
                if (len < 1)  // returned error or empty buf1
                    goto fail;
                if ((q - buf + len) > buf_size - 1)
                    goto fail;
                memcpy(q, buf1, len);
                q += len;
                p += (addchar_count + 1);
                addchar_count = 0;
                found_count++;
            }

        } else
            addchar_count = 1;

        while (addchar_count--)
            if ((q - buf) < buf_size - 1)
                *q++ = *p++;
            else
                goto fail;
    }
    *q = '\0';
    return found_count;
fail:
    *q = '\0';
    return -1;
}

static int hls_delete_old_segments(HLSContext *hls) {

    HLSSegment *segment, *previous_segment = NULL;
    float playlist_duration = 0.0f;
    int ret = 0, path_size, sub_path_size;
    char *dirname = NULL, *p, *sub_path;
    char *path = NULL;

    segment = hls->segments;
    while (segment) {
        playlist_duration += segment->duration;
        segment = segment->next;
    }

    segment = hls->old_segments;
    while (segment) {
        playlist_duration -= segment->duration;
        previous_segment = segment;
        segment = previous_segment->next;
        if (playlist_duration <= -previous_segment->duration) {
            previous_segment->next = NULL;
            break;
        }
    }

    if (segment && !hls->use_localtime_mkdir) {
        if (hls->segment_filename) {
            dirname = av_strdup(hls->segment_filename);
        } else {
            dirname = av_strdup(hls->avf->filename);
        }
        if (!dirname) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        p = (char *)av_basename(dirname);
        *p = '\0';
    }

    while (segment) {
        av_log(hls, AV_LOG_DEBUG, "deleting old segment %s\n",
                                  segment->filename);
        path_size =  (hls->use_localtime_mkdir ? 0 : strlen(dirname)) + strlen(segment->filename) + 1;
        path = (char*)av_malloc(path_size);
        if (!path) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        if (hls->use_localtime_mkdir)
            av_strlcpy(path, segment->filename, path_size);
        else { // segment->filename contains basename only
        av_strlcpy(path, dirname, path_size);
        av_strlcat(path, segment->filename, path_size);
        }

        if (unlink(path) < 0) {
            av_log(hls, AV_LOG_ERROR, "failed to delete old segment %s: %s\n",
                                     path, strerror(errno));
        }

        if ((segment->sub_filename[0] != '\0')) {
            sub_path_size = strlen(segment->sub_filename) + 1 + (dirname ? strlen(dirname) : 0);
            sub_path = (char*)av_malloc(sub_path_size);
            if (!sub_path) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            av_strlcpy(sub_path, dirname, sub_path_size);
            av_strlcat(sub_path, segment->sub_filename, sub_path_size);
            if (unlink(sub_path) < 0) {
                av_log(hls, AV_LOG_ERROR, "failed to delete old segment %s: %s\n",
                                         sub_path, strerror(errno));
            }
            av_free(sub_path);
        }
        av_freep(&path);
        previous_segment = segment;
        segment = previous_segment->next;
        av_free(previous_segment);
    }

fail:
    av_free(path);
    av_free(dirname);

    return ret;
}

static int hls_encryption_start(AVFormatContext *s)
{
    HLSContext *hls = (HLSContext *)s->priv_data;
    int ret;
    AVIOContext *pb;
    uint8_t key[KEYSIZE];

    if ((ret = avio_open2(&pb, hls->key_info_file, AVIO_FLAG_READ,
                           &s->interrupt_callback, NULL)) < 0) {
        av_log(hls, AV_LOG_ERROR,
                "error opening key info file %s\n", hls->key_info_file);
        return ret;
    }

    ff_get_line(pb, hls->key_uri, sizeof(hls->key_uri));
    hls->key_uri[strcspn(hls->key_uri, "\r\n")] = '\0';

    ff_get_line(pb, hls->key_file, sizeof(hls->key_file));
    hls->key_file[strcspn(hls->key_file, "\r\n")] = '\0';

    ff_get_line(pb, hls->iv_string, sizeof(hls->iv_string));
    hls->iv_string[strcspn(hls->iv_string, "\r\n")] = '\0';

    avio_close(pb);

    if (!*hls->key_uri) {
        av_log(hls, AV_LOG_ERROR, "no key URI specified in key info file\n");
        return AVERROR(EINVAL);
    }

    if (!*hls->key_file) {
        av_log(hls, AV_LOG_ERROR, "no key file specified in key info file\n");
        return AVERROR(EINVAL);
    }

    if ((ret = avio_open2(&pb, hls->key_file, AVIO_FLAG_READ,
                           &s->interrupt_callback, NULL)) < 0) {
        av_log(hls, AV_LOG_ERROR, "error opening key file %s\n", hls->key_file);
        return ret;
    }

    ret = avio_read(pb, key, sizeof(key));
    avio_close(pb);
    if (ret != sizeof(key)) {
        av_log(hls, AV_LOG_ERROR, "error reading key file %s\n", hls->key_file);
        if (ret >= 0 || ret == AVERROR_EOF)
            ret = AVERROR(EINVAL);
        return ret;
    }
    ff_data_to_hex(hls->key_string, key, sizeof(key), 0);

    return 0;
}

#if 0
static int read_chomp_line(AVIOContext *s, char *buf, int maxlen)
{
    int len = ff_get_line(s, buf, maxlen);
    while (len > 0 && av_isspace(buf[len - 1]))
        buf[--len] = '\0';
    return len;
}
#endif

static int hls_mux_init(AVFormatContext *s)
{
    HLSContext *hls = (HLSContext *)s->priv_data;
    AVFormatContext *oc;
    AVFormatContext *vtt_oc = NULL;
    unsigned int i; int ret;

    ret = avformat_alloc_output_context2(&hls->avf, hls->oformat, NULL, NULL);
    if (ret < 0)
        return ret;
    oc = hls->avf;

    oc->oformat            = hls->oformat;
    oc->interrupt_callback = s->interrupt_callback;
    oc->max_delay          = s->max_delay;
    oc->opaque             = s->opaque;
    av_dict_copy(&oc->metadata, s->metadata, 0);

    if(hls->vtt_oformat) {
        ret = avformat_alloc_output_context2(&hls->vtt_avf, hls->vtt_oformat, NULL, NULL);
        if (ret < 0)
            return ret;
        vtt_oc          = hls->vtt_avf;
        vtt_oc->oformat = hls->vtt_oformat;
        av_dict_copy(&vtt_oc->metadata, s->metadata, 0);
    }

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st;
        AVFormatContext *loc;
        if (s->streams[i]->codec->codec_type == AVMEDIA_TYPE_SUBTITLE)
            loc = vtt_oc;
        else
            loc = oc;

        if (!(st = avformat_new_stream(loc, NULL)))
            return AVERROR(ENOMEM);
        avcodec_copy_context(st->codec, s->streams[i]->codec);
        st->sample_aspect_ratio = s->streams[i]->sample_aspect_ratio;
        st->time_base = s->streams[i]->time_base;
    }
    hls->start_pos = 0;

    return 0;
}

static HLSSegment *find_segment_by_filename(HLSSegment *segment, const char *filename)
{
    while (segment) {
        if (!av_strcasecmp(segment->filename,filename))
            return segment;
        segment = segment->next;
    }
    return (HLSSegment *) NULL;
}

/* Create a new segment and append it to the segment list */
static int hls_append_segment(struct AVFormatContext *s, HLSContext *hls, double duration,
                              int64_t pos, int64_t size)
{
    HLSSegment *en = (HLSSegment *)av_malloc(sizeof(*en));
    const char  *filename;
    int ret;

    if (!en)
        return AVERROR(ENOMEM);

    if ((hls->flags & (HLS_SECOND_LEVEL_SEGMENT_SIZE | HLS_SECOND_LEVEL_SEGMENT_DURATION)) &&
        strlen(hls->current_segment_final_filename_fmt)) {
        char * old_filename = av_strdup(hls->avf->filename);  // %%s will be %s after strftime
        if (!old_filename) {
            av_free(en);
            return AVERROR(ENOMEM);
        }
        av_strlcpy(hls->avf->filename, hls->current_segment_final_filename_fmt, sizeof(hls->avf->filename));
        if (hls->flags & HLS_SECOND_LEVEL_SEGMENT_SIZE) {
            char * filename = av_strdup(hls->avf->filename);  // %%s will be %s after strftime
            if (!filename) {
                av_free(old_filename);
                av_free(en);
                return AVERROR(ENOMEM);
            }
            if (replace_int_data_in_filename(hls->avf->filename, sizeof(hls->avf->filename),
                filename, 's', pos + size) < 1) {
                av_log(hls, AV_LOG_ERROR,
                       "Invalid second level segment filename template '%s', "
                        "you can try to remove second_level_segment_size flag\n",
                       filename);
                av_free(filename);
                av_free(old_filename);
                av_free(en);
                return AVERROR(EINVAL);
            }
            av_free(filename);
        }
        if (hls->flags & HLS_SECOND_LEVEL_SEGMENT_DURATION) {
            char * filename = av_strdup(hls->avf->filename);  // %%t will be %t after strftime
            if (!filename) {
                av_free(old_filename);
                av_free(en);
                return AVERROR(ENOMEM);
            }
            if (replace_int_data_in_filename(hls->avf->filename, sizeof(hls->avf->filename),
                filename, 't',  (int64_t)round(1000000 * duration)) < 1) {
                av_log(hls, AV_LOG_ERROR,
                       "Invalid second level segment filename template '%s', "
                        "you can try to remove second_level_segment_time flag\n",
                       filename);
                av_free(filename);
                av_free(old_filename);
                av_free(en);
                return AVERROR(EINVAL);
            }
            av_free(filename);
        }
        ff_rename(old_filename, hls->avf->filename, hls);
        av_free(old_filename);
    }


    filename = av_basename(hls->avf->filename);

    if (hls->use_localtime_mkdir) {
        filename = hls->avf->filename;
    }
    if (find_segment_by_filename(hls->segments, filename)
        || find_segment_by_filename(hls->old_segments, filename)) {
        av_log(hls, AV_LOG_WARNING, "Duplicated segment filename detected: %s\n", filename);
    }
    av_strlcpy(en->filename, filename, sizeof(en->filename));

    if(hls->has_subtitle)
        av_strlcpy(en->sub_filename, av_basename(hls->vtt_avf->filename), sizeof(en->sub_filename));
    else
        en->sub_filename[0] = '\0';

    en->duration = duration;
    en->pos      = pos;
    en->size     = size;
    en->next     = NULL;
    en->discont  = 0;

    if (hls->discontinuity) {
        en->discont = 1;
        hls->discontinuity = 0;
    }

    if (hls->key_info_file) {
        av_strlcpy(en->key_uri, hls->key_uri, sizeof(en->key_uri));
        av_strlcpy(en->iv_string, hls->iv_string, sizeof(en->iv_string));
    }

    if (!hls->segments)
        hls->segments = en;
    else
        hls->last_segment->next = en;

    hls->last_segment = en;
    // TODO hls->avf->start_time_realtime
    ret = ffcb_new_segment(s, en->filename, en->duration);
    if(ret < 0)
        return ret;

    // EVENT or VOD playlists imply sliding window cannot be used
    if (hls->pl_type != PLAYLIST_TYPE_NONE)
        hls->max_nb_segments = 0;

    if (hls->max_nb_segments && hls->nb_entries >= hls->max_nb_segments) {
        en = hls->segments;
        hls->initial_prog_date_time += en->duration;
        hls->segments = en->next;
        if (en && hls->flags & HLS_DELETE_SEGMENTS &&
                !(hls->flags & HLS_SINGLE_FILE || hls->wrap)) {
            en->next = hls->old_segments;
            hls->old_segments = en;
            if ((ret = hls_delete_old_segments(hls)) < 0)
                return ret;
        } else
            av_free(en);
    } else
        hls->nb_entries++;

    if (hls->max_seg_size > 0) {
        return 0;
    }
    hls->sequence++;

    return 0;
}

static void hls_free_segments(HLSSegment *p)
{
    HLSSegment *en;

    while(p) {
        en = p;
        p = p->next;
        av_free(en);
    }
}

static void set_http_options(AVDictionary **options, HLSContext *c)
{
    if (c->method)
        av_dict_set(options, "method", c->method, 0);
}

static int hls_window(AVFormatContext *s, int last)
{
    HLSContext *hls = (HLSContext *)s->priv_data;
    HLSSegment *en;
    int target_duration = 0;
    int ret = 0;
    AVIOContext *out = NULL;
    AVIOContext *sub_out = NULL;
    char temp_filename[1024];
    int64_t sequence = FFMAX(hls->start_sequence, hls->sequence - hls->nb_entries);
    int version = 3;
    const char *proto = avio_find_protocol_name(s->filename);
    int use_rename = proto && !strcmp(proto, "file");
    static unsigned warned_non_file;
    char *key_uri = NULL;
    char *iv_string = NULL;
    AVDictionary *options = NULL;
    double prog_date_time = hls->initial_prog_date_time;
    int byterange_mode = (hls->flags & HLS_SINGLE_FILE) || (hls->max_seg_size > 0);

    if (byterange_mode) {
        version = 4;
        sequence = 0;
    }

    if (!use_rename && !warned_non_file++)
        av_log(s, AV_LOG_ERROR, "Cannot use rename on non file protocol, this may lead to races and temporary partial files\n");

    set_http_options(&options, hls);
    snprintf(temp_filename, sizeof(temp_filename), use_rename ? "%s.tmp" : "%s", s->filename);
    if ((ret = avio_open2(&out, temp_filename, AVIO_FLAG_WRITE,
                          &s->interrupt_callback, NULL)) < 0)
        goto fail;

    for (en = hls->segments; en; en = en->next) {
        if (target_duration <= en->duration)
            target_duration = get_int_from_double(en->duration);
    }

    hls->discontinuity_set = 0;
    avio_printf(out, "#EXTM3U\n");
    avio_printf(out, "#EXT-X-VERSION:%d\n", version);
    if (hls->allowcache == 0 || hls->allowcache == 1) {
        avio_printf(out, "#EXT-X-ALLOW-CACHE:%s\n", hls->allowcache == 0 ? "NO" : "YES");
    }
    avio_printf(out, "#EXT-X-TARGETDURATION:%d\n", target_duration);
    avio_printf(out, "#EXT-X-MEDIA-SEQUENCE:%" PRId64 "\n", sequence);
    if (hls->pl_type == PLAYLIST_TYPE_EVENT) {
        avio_printf(out, "#EXT-X-PLAYLIST-TYPE:EVENT\n");
    } else if (hls->pl_type == PLAYLIST_TYPE_VOD) {
        avio_printf(out, "#EXT-X-PLAYLIST-TYPE:VOD\n");
    }

    av_log(s, AV_LOG_VERBOSE, "EXT-X-MEDIA-SEQUENCE:%" PRId64 "\n",
           sequence);
    if((hls->flags & HLS_DISCONT_START) && sequence==hls->start_sequence && hls->discontinuity_set==0 ){
        avio_printf(out, "#EXT-X-DISCONTINUITY\n");
        hls->discontinuity_set = 1;
    }
    for (en = hls->segments; en; en = en->next) {
        if (hls->key_info_file && (!key_uri || strcmp(en->key_uri, key_uri) ||
                                    av_strcasecmp(en->iv_string, iv_string))) {
            avio_printf(out, "#EXT-X-KEY:METHOD=AES-128,URI=\"%s\"", en->key_uri);
            if (*en->iv_string)
                avio_printf(out, ",IV=0x%s", en->iv_string);
            avio_printf(out, "\n");
            key_uri = en->key_uri;
            iv_string = en->iv_string;
        }

        if (en->discont) {
            avio_printf(out, "#EXT-X-DISCONTINUITY\n");
        }

        if (hls->flags & HLS_ROUND_DURATIONS)
            avio_printf(out, "#EXTINF:%ld,\n",  lrint(en->duration));
        else
            avio_printf(out, "#EXTINF:%f,\n", en->duration);
        if (byterange_mode)
             avio_printf(out, "#EXT-X-BYTERANGE:%" PRIi64 "@%" PRIi64 "\n",
                         en->size, en->pos);
        if (hls->flags & HLS_PROGRAM_DATE_TIME) {
            time_t tt, wrongsecs;
            int milli;
            struct tm *tm, tmpbuf;
            char buf0[128], buf1[128];
            tt = (int64_t)prog_date_time;
            milli = av_clip(lrint(1000*(prog_date_time - tt)), 0, 999);
            tm = localtime_r(&tt, &tmpbuf);
            strftime(buf0, sizeof(buf0), "%Y-%m-%dT%H:%M:%S", tm);
            if (!strftime(buf1, sizeof(buf1), "%z", tm) || buf1[1]<'0' ||buf1[1]>'2') {
                int tz_min, dst = tm->tm_isdst;
                tm = gmtime_r(&tt, &tmpbuf);
                tm->tm_isdst = dst;
                wrongsecs = mktime(tm);
                tz_min = (abs(wrongsecs - tt) + 30) / 60;
                snprintf(buf1, sizeof(buf1),
                         "%c%02d%02d",
                         wrongsecs <= tt ? '+' : '-',
                         tz_min / 60,
                         tz_min % 60);
            }
            avio_printf(out, "#EXT-X-PROGRAM-DATE-TIME+:%s.%03d%s\n", buf0, milli, buf1);
            prog_date_time += en->duration;
        }
        if (hls->baseurl)
            avio_printf(out, "%s", hls->baseurl);
        avio_printf(out, "%s\n", en->filename);
    }

    if (last && (hls->flags & HLS_OMIT_ENDLIST)==0)
        avio_printf(out, "#EXT-X-ENDLIST\n");

    if( hls->vtt_m3u8_name ) {
        if ((ret = avio_open2(&sub_out, hls->vtt_m3u8_name, AVIO_FLAG_WRITE,
                          &s->interrupt_callback, NULL)) < 0)
            goto fail;
        avio_printf(sub_out, "#EXTM3U\n");
        avio_printf(sub_out, "#EXT-X-VERSION:%d\n", version);
        if (hls->allowcache == 0 || hls->allowcache == 1) {
            avio_printf(sub_out, "#EXT-X-ALLOW-CACHE:%s\n", hls->allowcache == 0 ? "NO" : "YES");
        }
        avio_printf(sub_out, "#EXT-X-TARGETDURATION:%d\n", target_duration);
        avio_printf(sub_out, "#EXT-X-MEDIA-SEQUENCE:%" PRId64 "\n", sequence);

        av_log(s, AV_LOG_VERBOSE, "EXT-X-MEDIA-SEQUENCE:%" PRId64 "\n",
               sequence);

        for (en = hls->segments; en; en = en->next) {
            avio_printf(sub_out, "#EXTINF:%f,\n", en->duration);
            if (byterange_mode)
                 avio_printf(sub_out, "#EXT-X-BYTERANGE:%" PRIi64 "@%" PRIi64 "\n",
                         en->size, en->pos);
            if (hls->baseurl)
                avio_printf(sub_out, "%s", hls->baseurl);
            avio_printf(sub_out, "%s\n", en->sub_filename);
        }

        if (last)
            avio_printf(sub_out, "#EXT-X-ENDLIST\n");

    }

fail:
    av_dict_free(&options);
    avio_closep(&out);
    avio_closep(&sub_out);
    if (ret >= 0 && use_rename)
        ff_rename(temp_filename, s->filename, s);
    return ret;
}

static int hls_start(AVFormatContext *s)
{
    HLSContext *c = (HLSContext *)s->priv_data;
    AVFormatContext *oc = c->avf;
    AVFormatContext *vtt_oc = c->vtt_avf;
    AVDictionary *options = NULL;
    char *filename, iv_string[KEYSIZE*2 + 1];
    int err = 0;

    if (c->flags & HLS_SINGLE_FILE) {
        av_strlcpy(oc->filename, c->basename,
                   sizeof(oc->filename));
        if (c->vtt_basename)
            av_strlcpy(vtt_oc->filename, c->vtt_basename,
                  sizeof(vtt_oc->filename));
    } else if (c->max_seg_size > 0) {
        if (av_get_frame_filename2(oc->filename, sizeof(oc->filename),
            c->basename, c->wrap ? c->sequence % c->wrap : c->sequence,
            AV_FRAME_FILENAME_FLAGS_MULTIPLE) < 0) {
                av_log(oc, AV_LOG_ERROR, "Invalid segment filename template '%s', you can try to use -use_localtime 1 with it\n", c->basename);
                return AVERROR(EINVAL);
        }
    } else {
        if (c->use_localtime) {
            /*
            time_t now0;
            struct tm *tm, tmpbuf;
            time(&now0);
            tm = localtime_r(&now0, &tmpbuf);
            if (!strftime(oc->filename, sizeof(oc->filename), c->basename, tm)) {
            */
            // TODO oc->start_time_realtime
            if (!strftime_f_now(oc->filename, sizeof(oc->filename), c->basename)) {
                av_log(oc, AV_LOG_ERROR, "Could not get segment filename with use_localtime\n");
                return AVERROR(EINVAL);
            }
            if (c->flags & HLS_SECOND_LEVEL_SEGMENT_INDEX) {
                char * filename = av_strdup(oc->filename);  // %%d will be %d after strftime
                if (!filename)
                    return AVERROR(ENOMEM);
                if (replace_int_data_in_filename(oc->filename, sizeof(oc->filename),
                    filename, 'd', c->wrap ? c->sequence % c->wrap : c->sequence) < 1) {
                    av_log(c, AV_LOG_ERROR,
                           "Invalid second level segment filename template '%s', "
                            "you can try to remove second_level_segment_index flag\n",
                           filename);
                    av_free(filename);
                    return AVERROR(EINVAL);
                }
                av_free(filename);
            }
            if (c->flags & (HLS_SECOND_LEVEL_SEGMENT_SIZE | HLS_SECOND_LEVEL_SEGMENT_DURATION)) {
                av_strlcpy(c->current_segment_final_filename_fmt, oc->filename,
                           sizeof(c->current_segment_final_filename_fmt));
                if (c->flags & HLS_SECOND_LEVEL_SEGMENT_SIZE) {
                    char * filename = av_strdup(oc->filename);  // %%s will be %s after strftime
                    if (!filename)
                        return AVERROR(ENOMEM);
                    if (replace_int_data_in_filename(oc->filename, sizeof(oc->filename), filename, 's', 0) < 1) {
                        av_log(c, AV_LOG_ERROR,
                               "Invalid second level segment filename template '%s', "
                                "you can try to remove second_level_segment_size flag\n",
                               filename);
                        av_free(filename);
                        return AVERROR(EINVAL);
                    }
                    av_free(filename);
                }
                if (c->flags & HLS_SECOND_LEVEL_SEGMENT_DURATION) {
                    char * filename = av_strdup(oc->filename);  // %%t will be %t after strftime
                    if (!filename)
                        return AVERROR(ENOMEM);
                    if (replace_int_data_in_filename(oc->filename, sizeof(oc->filename), filename, 't', 0) < 1) {
                        av_log(c, AV_LOG_ERROR,
                               "Invalid second level segment filename template '%s', "
                                "you can try to remove second_level_segment_time flag\n",
                               filename);
                        av_free(filename);
                        return AVERROR(EINVAL);
                    }
                    av_free(filename);
                }
            }
            if (c->use_localtime_mkdir) {
                const char *dir;
                char *fn_copy = av_strdup(oc->filename);
                if (!fn_copy) {
                    return AVERROR(ENOMEM);
                }
                dir = av_dirname(fn_copy);
                if (mkdir_p(dir) == -1 && errno != EEXIST) {
                    av_log(oc, AV_LOG_ERROR, "Could not create directory %s with use_localtime_mkdir\n", dir);
                    av_free(fn_copy);
                    return AVERROR(errno);
                }
                av_free(fn_copy);
            }
        } else if (av_get_frame_filename2(oc->filename, sizeof(oc->filename),
                                  c->basename, c->wrap ? c->sequence % c->wrap : c->sequence,
                                  AV_FRAME_FILENAME_FLAGS_MULTIPLE) < 0) {
            av_log(oc, AV_LOG_ERROR, "Invalid segment filename template '%s' you can try to use -use_localtime 1 with it\n", c->basename);
            return AVERROR(EINVAL);
        }
        if( c->vtt_basename) {
            if (av_get_frame_filename2(vtt_oc->filename, sizeof(vtt_oc->filename),
                              c->vtt_basename, c->wrap ? c->sequence % c->wrap : c->sequence,
                              AV_FRAME_FILENAME_FLAGS_MULTIPLE) < 0) {
                av_log(vtt_oc, AV_LOG_ERROR, "Invalid segment filename template '%s'\n", c->vtt_basename);
                return AVERROR(EINVAL);
            }
       }
    }
    c->number++;

    set_http_options(&options, c);

    if (c->key_info_file) {
        if ((err = hls_encryption_start(s)) < 0)
            goto fail;
        if ((err = av_dict_set(&options, "encryption_key", c->key_string, 0))
                < 0)
            goto fail;
        err = av_strlcpy(iv_string, c->iv_string, sizeof(iv_string));
        if (!err)
            snprintf(iv_string, sizeof(iv_string), "%032" PRIx64, c->sequence);
        if ((err = av_dict_set(&options, "encryption_iv", iv_string, 0)) < 0)
           goto fail;

        filename = av_asprintf("crypto:%s", oc->filename);
        if (!filename) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
        err = avio_open2(&oc->pb, filename, AVIO_FLAG_WRITE,
                         &s->interrupt_callback, &options);
        av_free(filename);
        av_dict_free(&options);
        if (err < 0)
            return err;
    } else
        if ((err = avio_open2(&oc->pb, oc->filename, AVIO_FLAG_WRITE,
                          &s->interrupt_callback, NULL)) < 0)
            goto fail;
    if (c->vtt_basename) {
        set_http_options(&options, c);
        if ((err = avio_open2(&vtt_oc->pb, vtt_oc->filename, AVIO_FLAG_WRITE,
                         &s->interrupt_callback, NULL)) < 0)
            goto fail;
    }
    av_dict_free(&options);

    /* We only require one PAT/PMT per segment. */
    if (oc->oformat->priv_class && oc->priv_data) {
        char period[21];

        snprintf(period, sizeof(period), "%d", (INT_MAX / 2) - 1);

        av_opt_set(oc->priv_data, "mpegts_flags", "resend_headers", 0);
        av_opt_set(oc->priv_data, "sdt_period", period, 0);
        av_opt_set(oc->priv_data, "pat_period", period, 0);
    }

    if (c->vtt_basename) {
        err = avformat_write_header(vtt_oc,NULL);
        if (err < 0)
            return err;
    }
    
    return 0;
fail:
    av_dict_free(&options);

    return err;
}

static const char * get_default_pattern_localtime_fmt(void)
{
    char b[21];
    time_t t = time(NULL);
    struct tm *p, tmbuf;
    p = localtime_r(&t, &tmbuf);
    // no %s support when strftime returned error or left format string unchanged
    return (!strftime(b, sizeof(b), "%s", p) || !strcmp(b, "%s")) ? "-%Y%m%d%H%M%S.ts" : "-%s.ts";
}

static int hls_write_header(AVFormatContext *s)
{
    HLSContext *hls = (HLSContext *)s->priv_data;
    unsigned int ret, i;
    char *p;
    const char *pattern = "%d.ts";
    const char *pattern_localtime_fmt = get_default_pattern_localtime_fmt();
    const char *vtt_pattern = "%d.vtt";
    AVDictionary *options = NULL;
    int basename_size;
    int vtt_basename_size;

    hls->sequence       = hls->start_sequence;
    hls->recording_time = (hls->init_time ? hls->init_time : hls->time) * AV_TIME_BASE;
    hls->start_pts      = AV_NOPTS_VALUE;
    hls->current_segment_final_filename_fmt[0] = '\0';

    if (hls->flags & HLS_PROGRAM_DATE_TIME) {
        time_t now0;
        time(&now0);
        hls->initial_prog_date_time = now0;
    }

    if (hls->format_options_str) {
        ret = av_dict_parse_string(&hls->format_options, hls->format_options_str, "=", ":", 0);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Could not parse format options list '%s'\n", hls->format_options_str);
            goto fail;
        }
    }

    for (i = 0; i < s->nb_streams; i++) {
        hls->has_video +=
            s->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO;
        hls->has_subtitle +=
            s->streams[i]->codec->codec_type == AVMEDIA_TYPE_SUBTITLE;
    }

    if (hls->has_video > 1)
        av_log(s, AV_LOG_WARNING,
               "More than a single video stream present, "
               "expect issues decoding it.\n");

    hls->oformat = av_guess_format("mpegts", NULL, NULL);

    if (!hls->oformat) {
        ret = AVERROR_MUXER_NOT_FOUND;
        goto fail;
    }

    if(hls->has_subtitle) {
        hls->vtt_oformat = av_guess_format("webvtt", NULL, NULL);
        if (!hls->oformat) {
            ret = AVERROR_MUXER_NOT_FOUND;
            goto fail;
        }
    }

    if (hls->segment_filename) {
        hls->basename = av_strdup(hls->segment_filename);
        if (!hls->basename) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    } else {
        if (hls->flags & HLS_SINGLE_FILE)
            pattern = ".ts";

        if (hls->use_localtime) {
            basename_size = strlen(s->filename) + strlen(pattern_localtime_fmt) + 1;
        } else {
            basename_size = strlen(s->filename) + strlen(pattern) + 1;
        }
        hls->basename = (char*)av_malloc(basename_size);
        if (!hls->basename) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        av_strlcpy(hls->basename, s->filename, basename_size);

        p = strrchr(hls->basename, '.');
        if (p)
            *p = '\0';
        if (hls->use_localtime) {
            av_strlcat(hls->basename, pattern_localtime_fmt, basename_size);
        } else {
            av_strlcat(hls->basename, pattern, basename_size);
        }
    }
    if (!hls->use_localtime) {
        if (hls->flags & HLS_SECOND_LEVEL_SEGMENT_DURATION) {
             av_log(hls, AV_LOG_ERROR,
                    "second_level_segment_duration hls_flag requires use_localtime to be true\n");
             ret = AVERROR(EINVAL);
             goto fail;
        }
        if (hls->flags & HLS_SECOND_LEVEL_SEGMENT_SIZE) {
             av_log(hls, AV_LOG_ERROR,
                    "second_level_segment_size hls_flag requires use_localtime to be true\n");
             ret = AVERROR(EINVAL);
             goto fail;
        }
        if (hls->flags & HLS_SECOND_LEVEL_SEGMENT_INDEX) {
            av_log(hls, AV_LOG_ERROR,
                   "second_level_segment_index hls_flag requires use_localtime to be true\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }
    } else {
        const char *proto = avio_find_protocol_name(hls->basename);
        int segment_renaming_ok = proto && !strcmp(proto, "file");

        if ((hls->flags & HLS_SECOND_LEVEL_SEGMENT_DURATION) && !segment_renaming_ok) {
             av_log(hls, AV_LOG_ERROR,
                    "second_level_segment_duration hls_flag works only with file protocol segment names\n");
             ret = AVERROR(EINVAL);
             goto fail;
        }
        if ((hls->flags & HLS_SECOND_LEVEL_SEGMENT_SIZE) && !segment_renaming_ok) {
             av_log(hls, AV_LOG_ERROR,
                    "second_level_segment_size hls_flag works only with file protocol segment names\n");
             ret = AVERROR(EINVAL);
             goto fail;
        }
    }
    if(hls->has_subtitle) {

        if (hls->flags & HLS_SINGLE_FILE)
            vtt_pattern = ".vtt";
        vtt_basename_size = strlen(s->filename) + strlen(vtt_pattern) + 1;
        hls->vtt_basename = (char*)av_malloc(vtt_basename_size);
        if (!hls->vtt_basename) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        hls->vtt_m3u8_name = (char*)av_malloc(vtt_basename_size);
        if (!hls->vtt_m3u8_name ) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        av_strlcpy(hls->vtt_basename, s->filename, vtt_basename_size);
        p = strrchr(hls->vtt_basename, '.');
        if (p)
            *p = '\0';

        if( hls->subtitle_filename ) {
            strcpy(hls->vtt_m3u8_name, hls->subtitle_filename);
        } else {
            strcpy(hls->vtt_m3u8_name, hls->vtt_basename);
            av_strlcat(hls->vtt_m3u8_name, "_vtt.m3u8", vtt_basename_size);
        }
        av_strlcat(hls->vtt_basename, vtt_pattern, vtt_basename_size);
    }

    if ((ret = hls_mux_init(s)) < 0)
        goto fail;
#if 0
    if (hls->flags & HLS_APPEND_LIST) {
        parse_playlist(s, s->filename);
        hls->discontinuity = 1;
        if (hls->init_time > 0) {
            av_log(s, AV_LOG_WARNING, "append_list mode does not support hls_init_time,"
                   " hls_init_time value will have no effect\n");
            hls->init_time = 0;
            hls->recording_time = hls->time * AV_TIME_BASE;
        }
    }
#endif
    if ((ret = hls_start(s)) < 0)
        goto fail;

    av_dict_copy(&options, hls->format_options, 0);
    ret = avformat_write_header(hls->avf, &options);
    if (av_dict_count(options)) {
        av_log(s, AV_LOG_ERROR, "Some of provided format options in '%s' are not recognized\n", hls->format_options_str);
        ret = AVERROR(EINVAL);
        goto fail;
    }
    //av_assert0(s->nb_streams == hls->avf->nb_streams);
    for (i = 0; i < s->nb_streams; i++) {
        AVStream *inner_st;
        AVStream *outer_st = s->streams[i];

        if (hls->max_seg_size > 0) {
            if ((outer_st->codec->codec_type == AVMEDIA_TYPE_VIDEO) &&
                (outer_st->codec->bit_rate > hls->max_seg_size)) {
                av_log(s, AV_LOG_WARNING, "Your video bitrate is bigger than hls_segment_size, "
                       "(%" PRId32 " > %" PRId64 "), the result maybe not be what you want.",
                       outer_st->codec->bit_rate, hls->max_seg_size);
            }
        }

        if (outer_st->codec->codec_type != AVMEDIA_TYPE_SUBTITLE)
            inner_st = hls->avf->streams[i];
        else if (hls->vtt_avf)
            inner_st = hls->vtt_avf->streams[0];
        else {
            /* We have a subtitle stream, when the user does not want one */
            inner_st = NULL;
            continue;
        }
        avpriv_set_pts_info(outer_st, inner_st->pts_wrap_bits, inner_st->time_base.num, inner_st->time_base.den);
    }
fail:

    av_dict_free(&options);
    if (ret < 0) {
        av_freep(&hls->basename);
        av_freep(&hls->vtt_basename);
        if (hls->avf)
            avformat_free_context(hls->avf);
        if (hls->vtt_avf)
            avformat_free_context(hls->vtt_avf);

    }
    return ret;
}

static int hls_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    HLSContext *hls = (HLSContext *)s->priv_data;
    AVFormatContext *oc = NULL;
    AVStream *st = s->streams[pkt->stream_index];
    int64_t end_pts = hls->recording_time * hls->number;
    int is_ref_pkt = 1;
    int ret, can_split = 1;
    int stream_index = 0;

    if (hls->sequence - hls->nb_entries > hls->start_sequence && hls->init_time > 0) {
        /* reset end_pts, hls->recording_time at end of the init hls list */
        int init_list_dur = hls->init_time * hls->nb_entries * AV_TIME_BASE;
        int after_init_list_dur = (hls->sequence - hls->nb_entries ) * hls->time * AV_TIME_BASE;
        hls->recording_time = hls->time * AV_TIME_BASE;
        end_pts = init_list_dur + after_init_list_dur ;
    }

    if( st->codec->codec_type == AVMEDIA_TYPE_SUBTITLE ) {
        oc = hls->vtt_avf;
        stream_index = 0;
    } else {
        oc = hls->avf;
        stream_index = pkt->stream_index;
    }
    if (hls->start_pts == AV_NOPTS_VALUE) {
        hls->start_pts = pkt->pts;
        hls->end_pts   = pkt->pts;
    }

    if (hls->has_video) {
        can_split = st->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
                    ((pkt->flags & AV_PKT_FLAG_KEY) || (hls->flags & HLS_SPLIT_BY_TIME));
        is_ref_pkt = st->codec->codec_type == AVMEDIA_TYPE_VIDEO;
    }
    if (pkt->pts == AV_NOPTS_VALUE)
        is_ref_pkt = can_split = 0;

    if (is_ref_pkt)
        hls->duration = (double)(pkt->pts - hls->end_pts)
                                   * st->time_base.num / st->time_base.den;

    if (can_split && av_compare_ts(pkt->pts - hls->start_pts, st->time_base,
                                   end_pts, AV_TIME_BASE_Q) >= 0) {
        int64_t new_start_pos;
        av_write_frame(oc, NULL); /* Flush any buffered data */

        new_start_pos = avio_tell(hls->avf->pb);
        hls->size = new_start_pos - hls->start_pos;
        ret = hls_append_segment(s, hls, hls->duration, hls->start_pos, hls->size);
        hls->start_pos = new_start_pos;
        if (ret < 0)
            return ret;

        hls->end_pts = pkt->pts;
        hls->duration = 0;

        if (hls->flags & HLS_SINGLE_FILE) {
            if (hls->avf->oformat->priv_class && hls->avf->priv_data)
                av_opt_set(hls->avf->priv_data, "mpegts_flags", "resend_headers", 0);
            hls->number++;
        } else if (hls->max_seg_size > 0) {
            if (hls->avf->oformat->priv_class && hls->avf->priv_data)
                av_opt_set(hls->avf->priv_data, "mpegts_flags", "resend_headers", 0);
            if (hls->start_pos >= hls->max_seg_size) {
                hls->sequence++;
                avio_closep(&oc->pb);
                if (hls->vtt_avf)
                    avio_closep(&hls->vtt_avf->pb);
                ret = hls_start(s);
                hls->start_pos = 0;
                /* When split segment by byte, the duration is short than hls_time,
                 * so it is not enough one segment duration as hls_time, */
                hls->number--;
            }
            hls->number++;
        } else {
            avio_closep(&oc->pb);
            if (hls->vtt_avf)
                avio_close(hls->vtt_avf->pb);

            ret = hls_start(s);
        }

        if (ret < 0)
            return ret;

        if( st->codec->codec_type == AVMEDIA_TYPE_SUBTITLE )
            oc = hls->vtt_avf;
        else
        oc = hls->avf;

        if ((ret = hls_window(s, 0)) < 0)
            return ret;
    }

    ret = ff_write_chained(oc, stream_index, pkt, s, 0);

    return ret;
}

static int hls_write_trailer(struct AVFormatContext *s)
{
    HLSContext *hls = (HLSContext *)s->priv_data;
    AVFormatContext *oc = hls->avf;
    AVFormatContext *vtt_oc = hls->vtt_avf;

    av_write_trailer(oc);
    if (oc->pb) {
        hls->size = avio_tell(hls->avf->pb) - hls->start_pos;
        avio_closep(&oc->pb);
        /* after av_write_trailer, then duration + 1 duration per packet */
        hls_append_segment(s, hls, hls->duration + hls->dpp, hls->start_pos, hls->size);
    }

    if (vtt_oc) {
        if (vtt_oc->pb)
            av_write_trailer(vtt_oc);
        hls->size = avio_tell(hls->vtt_avf->pb) - hls->start_pos;
        avio_closep(&vtt_oc->pb);
    }
    av_freep(&hls->basename);
    avformat_free_context(oc);

    hls->avf = NULL;
    hls_window(s, 1);

    if (vtt_oc) {
        av_freep(&hls->vtt_basename);
        av_freep(&hls->vtt_m3u8_name);
        avformat_free_context(vtt_oc);
    }

    hls_free_segments(hls->segments);
    hls_free_segments(hls->old_segments);
    return 0;
}

#define OFFSET(x) offsetof(HLSContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    {"start_number",  "set first number in the sequence",        OFFSET(start_sequence),AV_OPT_TYPE_INT64,  {.i64 = 0},     0, (double)INT64_MAX, E},
    {"hls_time",      "set segment length in seconds",           OFFSET(time),    AV_OPT_TYPE_FLOAT,  {.dbl = 2},     0, FLT_MAX, E},
    {"hls_init_time", "set segment length in seconds at init list",           OFFSET(init_time),    AV_OPT_TYPE_FLOAT,  {.dbl = 0},     0, FLT_MAX, E},
    {"hls_list_size", "set maximum number of playlist entries",  OFFSET(max_nb_segments),    AV_OPT_TYPE_INT,    {.i64 = 5},     0, INT_MAX, E},
    {"hls_ts_options","set hls mpegts list of options for the container format used for hls", OFFSET(format_options_str), AV_OPT_TYPE_STRING, {.str = NULL},  0, 0,    E},
    {"hls_vtt_options","set hls vtt list of options for the container format used for hls", OFFSET(vtt_format_options_str), AV_OPT_TYPE_STRING, {.str = NULL},  0, 0,    E},
    {"hls_wrap",      "set number after which the index wraps",  OFFSET(wrap),    AV_OPT_TYPE_INT,    {.i64 = 0},     0, INT_MAX, E},
    {"hls_allow_cache", "explicitly set whether the client MAY (1) or MUST NOT (0) cache media segments", OFFSET(allowcache), AV_OPT_TYPE_INT, {.i64 = -1}, INT_MIN, INT_MAX, E},
    {"hls_base_url",  "url to prepend to each playlist entry",   OFFSET(baseurl), AV_OPT_TYPE_STRING, {.str = NULL},  0, 0,       E},
    {"hls_segment_filename", "filename template for segment files", OFFSET(segment_filename),   AV_OPT_TYPE_STRING, {.str = NULL},            0,       0,         E},
    {"hls_segment_size", "maximum size per segment file, (in bytes)",  OFFSET(max_seg_size),    AV_OPT_TYPE_INT,    {.i64 = 0},               0,       INT_MAX,   E},
    {"hls_key_info_file",    "file with key URI and key file path", OFFSET(key_info_file),      AV_OPT_TYPE_STRING, {.str = NULL},            0,       0,         E},
    {"hls_subtitle_path",     "set path of hls subtitles", OFFSET(subtitle_filename), AV_OPT_TYPE_STRING, {.str = NULL},  0, 0,    E},
    {"hls_flags",     "set flags affecting HLS playlist and media file generation", OFFSET(flags), AV_OPT_TYPE_FLAGS, {.i64 = 0 }, 0, UINT_MAX, E, "flags"},
    {"single_file",   "generate a single media file indexed with byte ranges", 0, AV_OPT_TYPE_CONST, {.i64 = HLS_SINGLE_FILE }, 0, UINT_MAX,   E, "flags"},
    {"delete_segments", "delete segment files that are no longer part of the playlist", 0, AV_OPT_TYPE_CONST, {.i64 = HLS_DELETE_SEGMENTS }, 0, UINT_MAX,   E, "flags"},
    {"round_durations", "round durations in m3u8 to whole numbers", 0, AV_OPT_TYPE_CONST, {.i64 = HLS_ROUND_DURATIONS }, 0, UINT_MAX,   E, "flags"},
    {"discont_start", "start the playlist with a discontinuity tag", 0, AV_OPT_TYPE_CONST, {.i64 = HLS_DISCONT_START }, 0, UINT_MAX,   E, "flags"},
    {"omit_endlist", "Do not append an endlist when ending stream", 0, AV_OPT_TYPE_CONST, {.i64 = HLS_OMIT_ENDLIST }, 0, UINT_MAX,   E, "flags"},
    {"split_by_time", "split the hls segment by time which user set by hls_time", 0, AV_OPT_TYPE_CONST, {.i64 = HLS_SPLIT_BY_TIME }, 0, UINT_MAX,   E, "flags"},
    {"append_list", "append the new segments into old hls segment list", 0, AV_OPT_TYPE_CONST, {.i64 = HLS_APPEND_LIST }, 0, UINT_MAX,   E, "flags"},
    {"program_date_time", "add EXT-X-PROGRAM-DATE-TIME", 0, AV_OPT_TYPE_CONST, {.i64 = HLS_PROGRAM_DATE_TIME }, 0, UINT_MAX,   E, "flags"},
    {"second_level_segment_index", "include segment index in segment filenames when use_localtime", 0, AV_OPT_TYPE_CONST, {.i64 = HLS_SECOND_LEVEL_SEGMENT_INDEX }, 0, UINT_MAX,   E, "flags"},
    {"second_level_segment_duration", "include segment duration in segment filenames when use_localtime", 0, AV_OPT_TYPE_CONST, {.i64 = HLS_SECOND_LEVEL_SEGMENT_DURATION }, 0, UINT_MAX,   E, "flags"},
    {"second_level_segment_size", "include segment size in segment filenames when use_localtime", 0, AV_OPT_TYPE_CONST, {.i64 = HLS_SECOND_LEVEL_SEGMENT_SIZE }, 0, UINT_MAX,   E, "flags"},
    {"use_localtime", "set filename expansion with strftime at segment creation", OFFSET(use_localtime), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, 1, E },
    {"use_localtime_mkdir", "create last directory component in strftime-generated filename", OFFSET(use_localtime_mkdir), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, 1, E },
    {"hls_playlist_type", "set the HLS playlist type", OFFSET(pl_type), AV_OPT_TYPE_INT, {.i64 = PLAYLIST_TYPE_NONE }, 0, PLAYLIST_TYPE_NB-1, E, "pl_type" },
    {"event", "EVENT playlist", 0, AV_OPT_TYPE_CONST, {.i64 = PLAYLIST_TYPE_EVENT }, INT_MIN, INT_MAX, E, "pl_type" },
    {"vod", "VOD playlist", 0, AV_OPT_TYPE_CONST, {.i64 = PLAYLIST_TYPE_VOD }, INT_MIN, INT_MAX, E, "pl_type" },
    {"method", "set the HTTP method", OFFSET(method), AV_OPT_TYPE_STRING, {.str = NULL},  0, 0,    E},

    { NULL },
};

static const AVClass zm_hls_class = {
    .class_name = "zm-hls muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};


class AVOutputFormat_ZMC_HLS: public AVOutputFormat {
public:
    AVOutputFormat_ZMC_HLS() {
        name           = "zm-hls",
        long_name      = NULL_IF_CONFIG_SMALL("Apple HTTP Live Streaming"),
        extensions     = "zm-m3u8,m3u8",
        priv_data_size = sizeof(HLSContext),
        audio_codec    = AV_CODEC_ID_AAC,
        video_codec    = AV_CODEC_ID_H264,
        subtitle_codec = AV_CODEC_ID_WEBVTT,
        flags          = AVFMT_NOFILE | AVFMT_ALLOW_FLUSH,
        write_header   = hls_write_header,
        write_packet   = hls_write_packet,
        write_trailer  = hls_write_trailer,
        priv_class     = &zm_hls_class;
    }
} zm_hls_muxer;

} // namespace libavformat_hlsenc_c

namespace libavformat_img2enc_c {
    
typedef struct VideoMuxData {
    const AVClass *clazs;  /**< Class for private options. */
    int img_number;
    int is_pipe;
    int split_planes;       /**< use independent file for each Y, U, V plane */
    char path[1024];
    int update;
    int use_strftime;
    const char *muxer;
} VideoMuxData;

static int zma_write_header(AVFormatContext *s)
{
    VideoMuxData *img = (VideoMuxData *)s->priv_data;
    AVStream *st = s->streams[0];
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(st->codec->pix_fmt);

    av_strlcpy(img->path, s->filename, sizeof(img->path));

    /* find format */
    if (s->oformat->flags & AVFMT_NOFILE)
        img->is_pipe = 0;
    else
        img->is_pipe = 1;

    if (st->codec->codec_id == AV_CODEC_ID_GIF) {
        img->muxer = "gif";
    } else if (st->codec->codec_id == AV_CODEC_ID_RAWVIDEO) {
        const char *str = strrchr(img->path, '.');
        img->split_planes =     str
                             && !av_strcasecmp(str + 1, "y")
                             && s->nb_streams == 1
                             && desc
                             &&(desc->flags & AV_PIX_FMT_FLAG_PLANAR)
                             && desc->nb_components >= 3;
    }
    return 0;
}

static int zma_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    return ffcb_new_image(s, pkt);

    VideoMuxData *img = (VideoMuxData *)s->priv_data;
    AVIOContext *pb[4];
    char filename[1024];
    AVCodecContext *codec = s->streams[pkt->stream_index]->codec;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(codec->pix_fmt);
    int i;

    if (!img->is_pipe) {
        if (img->update) {
            av_strlcpy(filename, img->path, sizeof(filename));
        } else if (img->use_strftime) {
            /*time_t now0;
            struct tm *tm, tmpbuf;
            time(&now0);
            tm = localtime_r(&now0, &tmpbuf);
            if (!strftime(filename, sizeof(filename), img->path, tm)) {*/
            // see how to convert pts to true time
            if (!strftime_f_now(filename, sizeof(filename), img->path) ) {
                av_log(s, AV_LOG_ERROR, "Could not get frame filename with strftime\n");
                return AVERROR(EINVAL);
            }
        } else if (av_get_frame_filename(filename, sizeof(filename), img->path, img->img_number) < 0 &&
                   img->img_number > 1) {
            av_log(s, AV_LOG_ERROR,
                   "Could not get frame filename number %d from pattern '%s' (either set updatefirst or use a pattern like %%03d within the filename pattern)\n",
                   img->img_number, img->path);
            return AVERROR(EINVAL);
        }

        if (img->use_strftime) {
            const char *dir;
            char *fn_copy = av_strdup(filename);
            if (!fn_copy) {
                return AVERROR(ENOMEM);
            }
            dir = av_dirname(fn_copy);
            if (libavformat_hlsenc_c::mkdir_p(dir) == -1 && errno != EEXIST) {
                av_log(s, AV_LOG_ERROR, "Could not create directory %s with use_localtime_mkdir\n", dir);
                av_free(fn_copy);
                return AVERROR(errno);
            }
            av_free(fn_copy);
        }

        for (i = 0; i < 4; i++) {
            if (avio_open2(&pb[i], filename, AVIO_FLAG_WRITE,
                           &s->interrupt_callback, NULL) < 0) {
                av_log(s, AV_LOG_ERROR, "Could not open file : %s\n", filename);
                return AVERROR(EIO);
            }

            if (!img->split_planes || i+1 >= desc->nb_components)
                break;
            filename[strlen(filename) - 1] = "UVAx"[i];
        }
    } else {
        pb[0] = s->pb;
    }

    if (img->split_planes) {
        int ysize = codec->width * codec->height;
        int usize = FF_CEIL_RSHIFT(codec->width, desc->log2_chroma_w) * FF_CEIL_RSHIFT(codec->height, desc->log2_chroma_h);
        if (desc->comp[0].depth_minus1 >= 8) {
            ysize *= 2;
            usize *= 2;
        }
        avio_write(pb[0], pkt->data                , ysize);
        avio_write(pb[1], pkt->data + ysize        , usize);
        avio_write(pb[2], pkt->data + ysize + usize, usize);
        avio_closep(&pb[1]);
        avio_closep(&pb[2]);
        if (desc->nb_components > 3) {
            avio_write(pb[3], pkt->data + ysize + 2*usize, ysize);
            avio_closep(&pb[3]);
        }
    } else if (img->muxer) {
        int ret;
        AVStream *st;
        AVPacket pkt2 = {0};
        AVFormatContext *fmt = NULL;

        av_assert0(!img->split_planes);

        ret = avformat_alloc_output_context2(&fmt, NULL, img->muxer, s->filename);
        if (ret < 0)
            return ret;
        st = avformat_new_stream(fmt, NULL);
        if (!st) {
            avformat_free_context(fmt);
            return AVERROR(ENOMEM);
        }
        st->id = pkt->stream_index;

        fmt->pb = pb[0];
        if ((ret = av_copy_packet(&pkt2, pkt))                            < 0 ||
            (ret = av_dup_packet(&pkt2))                                  < 0 ||
            (ret = avcodec_copy_context(st->codec, s->streams[0]->codec)) < 0 ||
            (ret = avformat_write_header(fmt, NULL))                      < 0 ||
            (ret = av_interleaved_write_frame(fmt, &pkt2))                < 0 ||
            (ret = av_write_trailer(fmt))                                 < 0) {
            av_free_packet(&pkt2);
            avformat_free_context(fmt);
            return ret;
        }
        av_free_packet(&pkt2);
        avformat_free_context(fmt);
    } else {
        avio_write(pb[0], pkt->data, pkt->size);
    }
    avio_flush(pb[0]);
    if (!img->is_pipe) {
        avio_closep(&pb[0]);
    }

    img->img_number++;
    return 0;
}

#undef OFFSET
#define OFFSET(x) offsetof(VideoMuxData, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption muxoptions[] = {
    { "updatefirst",  "continuously overwrite one file", OFFSET(update),  AV_OPT_TYPE_INT, { .i64 = 0 }, 0,       1, ENC },
    { "update",       "continuously overwrite one file", OFFSET(update),  AV_OPT_TYPE_INT, { .i64 = 0 }, 0,       1, ENC },
    { "start_number", "set first number in the sequence", OFFSET(img_number), AV_OPT_TYPE_INT,  { .i64 = 1 }, 0, INT_MAX, ENC },
    { "strftime",     "use strftime for filename", OFFSET(use_strftime), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, ENC },
    { NULL },
};

#if 1 //CONFIG_IMAGE2_MUXER
static const AVClass img2mux_class = {
    .class_name = "image2 muxer for zma",
    .item_name  = av_default_item_name,
    .option     = muxoptions,
    .version    = LIBAVUTIL_VERSION_INT,
};

class AVOutputFormat_ZMA: public AVOutputFormat {
public:
	AVOutputFormat_ZMA() {
		name           = "image2zma",
		long_name      = NULL_IF_CONFIG_SMALL("image2 sequence for zma"),
		extensions     = "bmp,dpx,jls,jpeg,jpg,ljpg,pam,pbm,pcx,pgm,pgmyuv,png,"
		                  "ppm,sgi,tga,tif,tiff,jp2,j2c,j2k,xwd,sun,ras,rs,im1,im8,im24,"
		                  "sunras,xbm,xface,pix,y",
		priv_data_size = sizeof(VideoMuxData),
		video_codec    = AV_CODEC_ID_MJPEG,
		write_header   = zma_write_header,
		write_packet   = zma_write_packet,
		flags          = AVFMT_NOTIMESTAMPS | AVFMT_NODIMENSIONS | AVFMT_NOFILE,
		priv_class     = &img2mux_class;
	}

	void replace(AVOutputFormat* dest) {
		//dest->name = name;
		dest->extensions = extensions;
		dest->priv_data_size = priv_data_size;
		dest->video_codec = video_codec;
		dest->write_header = write_header;
		dest->write_packet = write_packet;
		dest->flags = flags;
		dest->priv_class = priv_class;
	}

} zma_image2_muxer;
#endif

#if 0 // CONFIG_IMAGE2PIPE_MUXER
AVOutputFormat ff_image2pipe_muxer = {
    .name           = "image2pipe",
    .long_name      = NULL_IF_CONFIG_SMALL("piped image2 sequence"),
    .priv_data_size = sizeof(VideoMuxData),
    .video_codec    = AV_CODEC_ID_MJPEG,
    .write_header   = write_header,
    .write_packet   = write_packet,
    .flags          = AVFMT_NOTIMESTAMPS | AVFMT_NODIMENSIONS
};
#endif
} // namespace libavformat_img2enc_c

// -----------------------------------------------------------------------------
// on this thread we concatenate segments and delete temporary files

static void backgroundThreadFunction() {
	try {
		DebugScope(Logger::DEBUG9);

		bool keepgoing = true;
		bool destructorWait = true;
		string filename;

		while(keepgoing) {
			keepgoing = false;

			if(__file_deletion_queue.extract_front(filename)) {
				keepgoing = true;
				if(std::remove(filename.c_str()) != 0) {
					int err = errno;
					switch(err) {
						case EBUSY:
							Warning("cannot delete busy file: %s", filename.c_str());
							__file_deletion_queue.push_back(std::move(filename));
							break;

						default:
							Error("cannot delete file, error %d: %s", err, filename.c_str());
							break;
					}
				} else {
					// delete succeeded, removing empty directory tree
					size_t pos = filename.find_last_of('/');
					if(string::npos != pos) {
						filename.erase(pos);
						__file_deletion_queue.push_back(std::move(filename));
					}
				}
			}

			if(!__cameras_map.empty()) {
				keepgoing = true;
				lock_guard<mutex> lock(__background_mutex);

				for(auto it = __cameras_map.begin(); it != __cameras_map.end(); ++it) {
					it->second->DoBackgroundWork();
				}
			} else if(destructorWait) {
				// no more cameras, waiting for the last destructor
				keepgoing = true;
				destructorWait = false;
				// wait for the destructor to enqueue files for deletion
				this_thread::sleep_for(chrono::milliseconds(500));
			}

			this_thread::sleep_for(chrono::milliseconds(100));
		} // while keepgoing
	} catch(std::exception& e) {
		Error("Background thread exception: %s", e.what());
	} catch(...) {
		Error("Background thread unhandled exception");
	}
}

// -----------------------------------------------------------------------------

static bool ffmpegInitEngine() {

	if(logDebugging())
		av_log_set_level(AV_LOG_DEBUG);
	else
		av_log_set_level(AV_LOG_QUIET);

	av_register_all();

	//AVOutputFormat* hlsFormat = av_guess_format("HLS", NULL, NULL);
	//Debug(9, "Test HLS export: %s", hlsFormat->name);
	//AVOutputFormat* segFormat = av_guess_format("SEGMENT", NULL, NULL);
	//Debug(9, "Test SEG export: %s", segFormat->name);
	AVOutputFormat* imgFormat = av_guess_format("IMAGE2", NULL, NULL);
	//Debug(9, "Test IMG export: %s", imgFormat->name);

	// since "image2" is hardcoded in the libs when searching for codecs, we'll
	// have to replace the existing registered format, instead of registering ours
	if(imgFormat)
		libavformat_img2enc_c::zma_image2_muxer.replace(imgFormat);
	else
		av_register_output_format(&libavformat_img2enc_c::zma_image2_muxer);

	// for the hls encoder we just register a new one, with a different name
	av_register_output_format(&libavformat_hlsenc_c::zm_hls_muxer);

	avformat_network_init();

	return true;
}

// -----------------------------------------------------------------------------
class ffmpeg_exception: public std::exception {
	int m_code;
public:
	ffmpeg_exception(int p_code): m_code(p_code) {}
	virtual const char* what() const noexcept { return "ffmpeg exit"; }
	int code() const { return m_code; }
};

static void ffmpegThreadFunction() {
	try {
		std::unique_ptr<char*[] > argv(new char* [__ffmpeg_argv.size()]);
		for(size_t i = 0; i < __ffmpeg_argv.size(); i++) {
			argv[i] = (char*) __ffmpeg_argv[i].c_str();
		}

		// the main() function inside ffmpeg.c was modified to ffmpeg_main
		ffmpeg_main(__ffmpeg_argv.size(), argv.get());
	} catch(ffmpeg_exception& e) {
		Info("ffmpeg thread exited with code %d", e.code());
	} catch(exception& e) {
		Error("ffmpeg thread exited with exception: %s", e.what());
	} catch(...) {
		Error("Unhandled exception in ffmpeg main");
	}
}

static FfmpegEngineCamera* getCamera(AVFormatContext* s) {
	AVDictionaryEntry *entry = av_dict_get(s->metadata, META_KEY, NULL, 0);
	if(!entry)
		return NULL;

	int monitor_id = -1;
	if(sscanf(entry->value, "%d", &monitor_id) != 1)
		return NULL;

	if(monitor_id < 0)
		return NULL;
	//__cameras_map.find(entry->value);
	return find(__cameras_map, monitor_id);
}

// -----------------------------------------------------------------------------
// callbacks from the engine
// warning: they are not in the original code
// 
// TODO: make these acceptable for inclusion in ffmpeg upstream
// 
// for example in ffmpeg/cmdutils.c void register_exit(void (*cb)(int ret)) 
// may add cb in a list of functions to be called in sequence before exit
// 
// for output callbacks, for example add a registration function
// void av_register_output_callback(const char* muxer_name, const char* purpose, funcptr);
// 
// -----------------------------------------------------------------------------

// called from ffmpeg/cmdutils.c exit_program()
void ffcb_exit(int code) {
	// throw inside thread, before it calls exit(), to get a chance to clean up
	throw ffmpeg_exception(code);
}

// called from inside this file for now, libavformat_img2enc_c::zma_write_packet
int ffcb_new_image(AVFormatContext *ctx, AVPacket *pkt) {
	FfmpegEngineCamera* camera = getCamera(ctx);
	if(!camera)
		return -1;

	return camera->ffcb_new_image(ctx, pkt);
}

// called from inside this file for now, libavformat_hlsenc_c::hls_append_segment
int ffcb_new_segment(AVFormatContext *ctx, const char* filename, double duration) {
	FfmpegEngineCamera* camera = getCamera(ctx);
	if(!camera)
		return -1;

	return camera->ffcb_new_segment(ctx, filename, duration);
}

