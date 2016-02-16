﻿#include "pch.h"
#include "FrameCapturer.h"

#include <libyuv/libyuv.h>
#include "fcFoundation.h"
#include "fcThreadPool.h"
#include "GraphicsDevice/fcGraphicsDevice.h"
#include "fcMP4Internal.h"
#include "fcMP4File.h"
#include "fcH264Encoder.h"
#include "fcAACEncoder.h"
#include "fcMP4Muxer.h"
#include "fcMP4StreamWriter.h"
#ifdef fcWindows
    #pragma comment(lib, "yuv.lib")
#endif

#define fcMP4DefaultMaxBuffers 4


class fcMP4Context : public fcIMP4Context
{
public:
    fcMP4Context(fcMP4Config &conf, fcIGraphicsDevice *dev);
    ~fcMP4Context();
    void release() override;

    bool addVideoFrameTexture(void *tex, uint64_t timestamp) override;
    bool addVideoFramePixels(void *pixels, fcColorSpace c, uint64_t timestamps) override;
    bool addAudioFrame(const float *samples, int num_samples, uint64_t timestamp) override;
    void clearFrame() override;
    bool writeFile(const char *path, int begin_frame, int end_frame) override;
    int  writeMemory(void *buf, int begin_frame, int end_frame) override;

    int getFrameCount() override;
    void getFrameData(void *tex, int frame) override;
    int getExpectedDataSize(int begin_frame, int end_frame) override;
    void eraseFrame(int begin_frame, int end_frame) override;

private:
    void enqueueVideoTask(const std::function<void()> &f);
    void enqueueAudioTask(const std::function<void()> &f);
    void processVideoTasks();
    void processAudioTasks();

    fcVideoFrame&   getTempraryVideoFrame();
    void            returnTempraryVideoFrame(fcVideoFrame& v);
    fcAudioFrame&   getTempraryAudioFrame();
    void            returnTempraryAudioFrame(fcAudioFrame& v);

    void resetEncoders();
    void scrape(bool is_tasks_running);
    void waitAllTasksFinished();
    void addVideoFrameTask(fcH264Frame& o_fdata, fcVideoFrame &raw_buffer, bool rgba2i420);
    void write(std::ostream& os, int begin_frame, int end_frame);

private:
    fcMP4Config m_conf;
    fcIGraphicsDevice *m_dev;

    std::vector<fcVideoFrame> m_tmp_video_frames;
    std::vector<fcAudioFrame> m_tmp_audio_frames;
    std::vector<fcVideoFrame*> m_tmp_video_frames_unused;
    std::vector<fcAudioFrame*> m_tmp_audio_frames_unused;
    std::list<fcH264Frame>  m_h264_frames;
    std::list<fcAACFrame>   m_aac_frames;

    std::unique_ptr<fcIH264Encoder> m_h264_encoder;
    std::unique_ptr<fcIAACEncoder> m_aac_encoder;
    std::unique_ptr<fcMP4Muxer> m_muxer;

    std::atomic_int m_video_active_task_count;
    std::thread m_video_worker;
    std::mutex m_video_queue_mutex;
    std::condition_variable m_video_condition;
    std::deque<std::function<void()>> m_video_tasks;

    std::atomic_int m_audio_active_task_count;
    std::thread m_audio_worker;
    std::mutex m_audio_queue_mutex;
    std::condition_variable m_audio_condition;
    std::deque<std::function<void()>> m_audio_tasks;

    bool m_stop;

    std::fstream m_ofile;
    StdOStream m_stream;
    std::unique_ptr<fcMP4StreamWriter> m_writer;
};



fcMP4Context::fcMP4Context(fcMP4Config &conf, fcIGraphicsDevice *dev)
    : m_conf(conf)
    , m_dev(dev)
    , m_video_active_task_count(0)
    , m_audio_active_task_count(0)
    , m_stop(false)
    , m_stream(m_ofile)
{
    if (m_conf.video_max_buffers == 0) {
        m_conf.video_max_buffers = fcMP4DefaultMaxBuffers;
    }

    // allocate temporary buffers and start encoder threads
    if (m_conf.video) {
        m_tmp_video_frames.resize(m_conf.video_max_buffers);
        for (auto& v : m_tmp_video_frames) {
            v.allocate(m_conf.video_width, m_conf.video_height);
            m_tmp_video_frames_unused.push_back(&v);
        }

        m_video_worker = std::thread([this]() { processVideoTasks(); });
    }
    if (m_conf.audio) {
        m_tmp_audio_frames.resize(m_conf.video_max_buffers);
        for (auto& v : m_tmp_audio_frames) {
            m_tmp_audio_frames_unused.push_back(&v);
        }

        m_audio_worker = std::thread([this]() { processAudioTasks(); });
    }

    m_muxer.reset(new fcMP4Muxer());
    m_ofile.open("test.mp4", std::ios::out | std::ios::binary);
    m_writer.reset(new fcMP4StreamWriter(m_stream, m_conf));

    resetEncoders();
}

fcMP4Context::~fcMP4Context()
{
    // stop encoder threads
    m_stop = true;
    if (m_conf.video) {
        m_video_condition.notify_all();
        m_video_worker.join();
    }
    if (m_conf.audio) {
        m_audio_condition.notify_all();
        m_audio_worker.join();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    m_writer.reset();
}

void fcMP4Context::resetEncoders()
{
    waitAllTasksFinished();

    // create h264 encoder
    m_h264_encoder.reset();
    if (m_conf.video) {
        fcH264EncoderConfig h264conf;
        h264conf.width = m_conf.video_width;
        h264conf.height = m_conf.video_height;
        h264conf.target_framerate = m_conf.video_framerate;
        h264conf.target_bitrate = m_conf.video_bitrate;

        fcIH264Encoder *enc = nullptr;
        // try to create hardware encoder
        if (m_conf.video_use_hardware_encoder_if_possible) {
            enc = fcCreateNVH264Encoder(h264conf);
            if (enc == nullptr) {
                enc = fcCreateAMDH264Encoder(h264conf);
            }
        }
        if (enc == nullptr) {
            // fall back to software encoder (OpenH264)
            enc = fcCreateOpenH264Encoder(h264conf);
        }
        m_h264_encoder.reset(enc);
    }

    // create aac encoder
    m_aac_encoder.reset();
    if (m_conf.audio) {
        fcAACEncoderConfig aacconf;
        aacconf.sampling_rate = m_conf.audio_sample_rate;
        aacconf.num_channels = m_conf.audio_num_channels;
        aacconf.target_bitrate = m_conf.audio_bitrate;

        m_aac_encoder.reset(fcCreateFAACEncoder(aacconf));
        if (m_aac_encoder && m_writer) {
            m_writer->setAACHeader(m_aac_encoder->getHeader());
        }
    }
}

void fcMP4Context::enqueueVideoTask(const std::function<void()> &f)
{
    {
        std::unique_lock<std::mutex> lock(m_video_queue_mutex);
        m_video_tasks.push_back(std::function<void()>(f));
    }
    m_video_condition.notify_one();
}

void fcMP4Context::enqueueAudioTask(const std::function<void()> &f)
{
    {
        std::unique_lock<std::mutex> lock(m_audio_queue_mutex);
        m_audio_tasks.push_back(std::function<void()>(f));
    }
    m_audio_condition.notify_one();
}

void fcMP4Context::waitAllTasksFinished()
{
    while (m_video_active_task_count > 0 || m_audio_active_task_count > 0) {
        std::this_thread::yield();
    }
}


fcVideoFrame& fcMP4Context::getTempraryVideoFrame()
{
    fcVideoFrame *ret = nullptr;

    // wait if all temporaries are in use
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_video_queue_mutex);
            if (!m_tmp_video_frames_unused.empty()) {
                ret = m_tmp_video_frames_unused.back();
                m_tmp_video_frames_unused.pop_back();
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return *ret;
}

void fcMP4Context::returnTempraryVideoFrame(fcVideoFrame& v)
{
    std::unique_lock<std::mutex> lock(m_video_queue_mutex);
    m_tmp_video_frames_unused.push_back(&v);
}

fcAudioFrame& fcMP4Context::getTempraryAudioFrame()
{
    fcAudioFrame *ret = nullptr;

    // wait if all temporaries are in use
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_audio_queue_mutex);
            if (!m_tmp_audio_frames_unused.empty()) {
                ret = m_tmp_audio_frames_unused.back();
                m_tmp_audio_frames_unused.pop_back();
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return *ret;
}

void fcMP4Context::returnTempraryAudioFrame(fcAudioFrame& v)
{
    std::unique_lock<std::mutex> lock(m_audio_queue_mutex);
    m_tmp_audio_frames_unused.push_back(&v);
}


void fcMP4Context::processVideoTasks()
{
    while (!m_stop)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_video_queue_mutex);
            while (!m_stop && m_video_tasks.empty()) {
                m_video_condition.wait(lock);
            }
            if (m_stop) { return; }

            task = m_video_tasks.front();
            m_video_tasks.pop_front();
        }
        task();
    }
}

void fcMP4Context::processAudioTasks()
{
    while (!m_stop)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_audio_queue_mutex);
            while (!m_stop && m_audio_tasks.empty()) {
                m_audio_condition.wait(lock);
            }
            if (m_stop) { return; }

            task = m_audio_tasks.front();
            m_audio_tasks.pop_front();
        }
        task();
    }
}


void fcMP4Context::release()
{
    delete this;
}

void fcMP4Context::scrape(bool updating)
{
    // 最大容量か最大フレーム数が設定されている場合、それらを超過したフレームをここで切り捨てる。
    // 切り捨てるフレームがパレットを持っている場合パレットの移動も行う。

    // 実行中のタスクが更新中のデータを間引くのはマズいので、更新中は最低でもタスク数分は残す
    int min_frames = updating ? std::max<int>(m_conf.video_max_buffers, 1) : 1;

    // todo
}

void fcMP4Context::addVideoFrameTask(fcH264Frame& h264, fcVideoFrame &raw, bool rgba2i420)
{
    // 必要であれば RGBA -> I420 変換
    int width = m_conf.video_width;
    int frame_size = m_conf.video_width * m_conf.video_height;
    uint8 *y = (uint8*)raw.i420.y;
    uint8 *u = (uint8*)raw.i420.u;
    uint8 *v = (uint8*)raw.i420.v;
    if (rgba2i420) {
        libyuv::ABGRToI420(
            (uint8*)&raw.rgba[0], width * 4,
            y, width,
            u, width >> 1,
            v, width >> 1,
            m_conf.video_width, m_conf.video_height );
    }

    // I420 のピクセルデータを H264 へエンコード
    m_h264_encoder->encode(h264, raw.i420, raw.timestamp);
    h264.timestamp = raw.timestamp;

    if (m_writer) {
        m_writer->addFrame(h264);
    }
}


bool fcMP4Context::addVideoFrameTexture(void *tex, uint64_t timestamp)
{
    if (!m_h264_encoder) { return false; }

    fcVideoFrame& raw = getTempraryVideoFrame();
    raw.timestamp = timestamp!=0 ? timestamp : GetCurrentTimeNanosec();

    // フレームバッファの内容取得
    if (!m_dev->readTexture(&raw.rgba[0], raw.rgba.size(), tex, m_conf.video_width, m_conf.video_height, fcTextureFormat_ARGB32))
    {
        returnTempraryVideoFrame(raw);
        return false;
    }

    // h264 データを生成
    m_h264_frames.push_back(fcH264Frame());
    fcH264Frame& h264 = m_h264_frames.back();
    ++m_video_active_task_count;
    enqueueVideoTask([this, &h264, &raw](){
        h264.clear();
        addVideoFrameTask(h264, raw, true);
        returnTempraryVideoFrame(raw);
        --m_video_active_task_count;
    });

    scrape(true);
    return true;
}

bool fcMP4Context::addVideoFramePixels(void *pixels, fcColorSpace cs, uint64_t timestamp)
{
    if (!m_h264_encoder) { return false; }

    fcVideoFrame& raw = getTempraryVideoFrame();
    raw.timestamp = timestamp != 0 ? timestamp : GetCurrentTimeNanosec();

    bool rgba2i420 = true;
    if (cs == fcColorSpace_RGBA) {
        memcpy(raw.rgba.ptr(), pixels, raw.rgba.size());
    }
    else if (cs == fcColorSpace_I420) {
        rgba2i420 = false;

        int frame_size = m_conf.video_width * m_conf.video_height;
        const uint8_t *src_y = (const uint8_t*)pixels;
        const uint8_t *src_u = src_y + frame_size;
        const uint8_t *src_v = src_u + (frame_size >> 2);
        memcpy(raw.i420.y, src_y, frame_size);
        memcpy(raw.i420.u, src_u, frame_size >> 2);
        memcpy(raw.i420.v, src_v, frame_size >> 2);
    }

    // h264 データを生成
    m_h264_frames.push_back(fcH264Frame());
    fcH264Frame& h264 = m_h264_frames.back();
    ++m_video_active_task_count;
    enqueueVideoTask([this, &h264, &raw, rgba2i420](){
        h264.clear();
        addVideoFrameTask(h264, raw, rgba2i420);
        returnTempraryVideoFrame(raw);
        --m_video_active_task_count;
    });

    scrape(true);
    return true;
}

bool fcMP4Context::addAudioFrame(const float *samples, int num_samples, uint64_t timestamp)
{
    if (!m_aac_encoder) { return false; }

    fcAudioFrame& raw = getTempraryAudioFrame();
    raw.timestamp = timestamp != 0 ? timestamp : GetCurrentTimeNanosec();
    raw.data = Buffer(samples, sizeof(float)*num_samples);

    m_aac_frames.push_back(fcAACFrame());
    fcAACFrame& aac = m_aac_frames.back();

    // aac encode
    ++m_audio_active_task_count;
    enqueueAudioTask([this, &aac, &raw](){
        aac.clear();

        // apply audio_scale
        if (m_conf.audio_scale != 1.0f) {
            float *samples = (float*)raw.data.ptr();
            size_t num_samples = raw.data.size() / sizeof(float);
            for (size_t i = 0; i < num_samples; ++i) {
                samples[i] *= m_conf.audio_scale;
            }
        }

        m_aac_encoder->encode(aac, (float*)raw.data.ptr(), raw.data.size() / sizeof(float));
        aac.timestamp = raw.timestamp;

        if (m_writer) {
            m_writer->addFrame(aac);
        }

        returnTempraryAudioFrame(raw);
        --m_audio_active_task_count;
    });

    return true;
}

void fcMP4Context::clearFrame()
{
    waitAllTasksFinished();
    m_h264_frames.clear();
    m_aac_frames.clear();
    resetEncoders();
}


static inline void adjust_frame(int &begin_frame, int &end_frame, int max_frame)
{
    begin_frame = std::max<int>(begin_frame, 0);
    if (end_frame < 0) {
        end_frame = max_frame;
    }
    else {
        end_frame = std::min<int>(end_frame, max_frame);
    }
}


void fcMP4Context::write(std::ostream &os, int begin_frame, int end_frame)
{
    addAudioFrame(nullptr, 0, 0); // flush
    waitAllTasksFinished();
    scrape(false);

    char tmp_h264_filename[256];
    char tmp_aac_filename[256];
    char tmp_mp4_filename[256];

    uint64_t now = (uint64_t)::time(nullptr);
    sprintf(tmp_h264_filename, "%llu.h264", now);
    sprintf(tmp_aac_filename, "%llu.aac", now);
    sprintf(tmp_mp4_filename, "%llu.mp4", now);

    fcMP4Muxer::Params params;
    params.frame_rate = m_conf.video_framerate;
    params.out_mp4_path = tmp_mp4_filename;
    if(m_conf.video) {
        params.in_h264_path = tmp_h264_filename;
        std::ofstream tmp_h264(tmp_h264_filename, std::ios::binary);

        adjust_frame(begin_frame, end_frame, (int)m_h264_frames.size());
        auto begin = m_h264_frames.begin();
        auto end = m_h264_frames.begin();
        std::advance(begin, begin_frame);
        std::advance(end, end_frame);
        for (auto i = begin; i != end; ++i) {
            tmp_h264.write(&i->data[0], i->data.size());
        }
    }
    if (m_conf.audio) {
        params.in_aac_path = tmp_aac_filename;
        std::ofstream tmp_aac(tmp_aac_filename, std::ios::binary);

        for (const auto& aac : m_aac_frames) {
            tmp_aac.write(aac.data.ptr(), aac.data.size());
        }
    }

    m_muxer->mux(params);

    {
        char buf[1024];
        std::ifstream tmp_mp4(tmp_mp4_filename, std::ios::binary);
        while (!tmp_mp4.eof()) {
            tmp_mp4.read(buf, sizeof(buf));
            os.write(buf, tmp_mp4.gcount());
        }
    }
#ifdef fcMaster
    std::remove(tmp_aac_filename);
    std::remove(tmp_h264_filename);
    std::remove(tmp_mp4_filename);
#endif // fcMaster
}

bool fcMP4Context::writeFile(const char *path, int begin_frame, int end_frame)
{
    std::ofstream os(path, std::ios::binary);
    if (!os) { return false; }
    write(os, begin_frame, end_frame);
    os.close();
    return true;
}

int fcMP4Context::writeMemory(void *buf, int begin_frame, int end_frame)
{
    std::ostringstream os(std::ios::binary);
    write(os, begin_frame, end_frame);

    std::string s = os.str();
    memcpy(buf, &s[0], s.size());
    return (int)s.size();
}


int fcMP4Context::getFrameCount()
{
    return m_h264_frames.size();
}

void fcMP4Context::getFrameData(void *tex, int frame)
{
    if (frame >= m_h264_frames.size()) { return; }
    waitAllTasksFinished();
    scrape(false);

    fcH264Frame *fdata = nullptr;
    {
        auto it = m_h264_frames.begin();
        std::advance(it, frame);
        fdata = &(*it);
    }

    fcVideoFrame raw;
    raw.allocate(m_conf.video_width, m_conf.video_height);
    // todo: decode
    m_dev->writeTexture(tex, m_conf.video_width, m_conf.video_height, fcTextureFormat_ARGB32, &raw.rgba[0], raw.rgba.size());
}


int fcMP4Context::getExpectedDataSize(int begin_frame, int end_frame)
{
    adjust_frame(begin_frame, end_frame, (int)m_h264_frames.size());
    auto begin = m_h264_frames.begin();
    auto end = m_h264_frames.begin();
    std::advance(begin, begin_frame);
    std::advance(end, end_frame);

    size_t size = 2048; // mp4 metadata
    for (auto i = begin; i != end; ++i) {
        size += i->data.size();
    }
    return (int)size;
}

void fcMP4Context::eraseFrame(int begin_frame, int end_frame)
{
    waitAllTasksFinished();
    scrape(false);

    adjust_frame(begin_frame, end_frame, (int)m_h264_frames.size());
    auto begin = m_h264_frames.begin();
    auto end = m_h264_frames.begin();
    std::advance(begin, begin_frame);
    std::advance(end, end_frame);
    m_h264_frames.erase(begin, end);
    // todo: remake IDR frame if needed
}


fcCLinkage fcExport fcIMP4Context* fcMP4CreateContextImpl(fcMP4Config &conf, fcIGraphicsDevice *dev)
{
    if (fcLoadOpenH264Module()) {
        fcLoadFAACModule();
        return new fcMP4Context(conf, dev);
    }
    return nullptr;
}


std::string g_fcModulePath;

fcCLinkage fcExport void fcMP4SetModulePathImpl(const char *path)
{
    g_fcModulePath = path;
}

fcCLinkage fcExport bool fcMP4DownloadCodecImpl(fcDownloadCallback cb)
{
    return fcDownloadOpenH264(cb);
}
