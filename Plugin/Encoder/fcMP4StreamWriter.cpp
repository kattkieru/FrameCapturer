#include "pch.h"
#include <openh264/codec_api.h>
#include "fcMP4Internal.h"
#include "fcMP4StreamWriter.h"

#ifdef fcDebugLog
    #undef fcDebugLog
    #define fcDebugLog(...)
#endif

#define fcMP464BitLength


namespace {

class Box
{
public:
    Box(BinaryStream& stream) : m_stream(stream) {}

    template<class F>
    void operator()(u32 name, const F &f)
    {
        size_t offset = m_stream.tellp();
        m_stream << u32(0) << name; // reserve

        f();

        size_t pos = m_stream.tellp();
        u32 box_size = (u32)(pos - offset);
        m_stream.seekp(offset);
        m_stream << u32_be(box_size);
        m_stream.seekp(pos);
    }

private:
    BinaryStream& m_stream;
};


time_t fcGetMacTime()
{
    return time(0) + 2082844800;
}

} // namespace


fcMP4StreamWriter::fcMP4StreamWriter(BinaryStream& stream, const fcMP4Config &conf)
    : m_stream(stream)
    , m_conf(conf)
    , m_mdat_begin(0)
{
    mp4Begin();
}

fcMP4StreamWriter::~fcMP4StreamWriter()
{
    mp4End();
}

void fcMP4StreamWriter::mp4Begin()
{
    BinaryStream& os = m_stream;
    os  << u32_be(0x18)
        << u32_be('ftyp')
        << u32_be('mp42')
        << u32_be(0x00)
        << u32_be('mp42')
        << u32_be('isom')
        << u32_be(0x8)
        << u32_be('free');

    m_mdat_begin = os.tellp();

    os  << u32_be(0x1)
        << u32_be('mdat')
#ifdef fcMP464BitLength
        << u64(0) // 64bit mdat length
#endif // fcMP464BitLength
        ;
}

void fcMP4StreamWriter::addFrame(const fcFrameData& frame)
{
    if (frame.data.empty()) { return; }

    BinaryStream& os = m_stream;

    fcFrameInfo info;
    info.file_offset = os.tellp();
    info.timestamp = frame.timestamp;

    // video frame
    if (frame.type == fcFrameType_H264) {
        const auto& h264 = (const fcH264Frame&)frame;

        if (h264.h264_type == fcH264FrameType_I) {
            m_iframe_ids.push_back((uint32_t)m_video_frame_info.size() + 1);
        }

        h264.eachNALs([&](const char *data, int size) {
            const int offset = 4; // 0x00000001

            fcH264NALHeader nalh(data[4]);
            if (nalh.nal_unit_type == NAL_SPS) {
                m_sps.assign(&data[offset], &data[offset] + (size - offset));
            }
            else if (nalh.nal_unit_type == NAL_PPS) {
                m_pps.assign(&data[offset], &data[offset] + (size - offset));
            }
            else {
                os << u32_be(size - offset);
                os.write(&data[offset], size - offset);
                info.size += (size - offset) + 4;
            }

        });

        m_video_frame_info.emplace_back(info);
    }
    // audio frame
    else if (frame.type == fcFrameType_AAC) {
        const int offset = 0;
        const size_t size = frame.data.size();
        const char *data = frame.data.ptr();

        os.write(data, size - offset);
        info.size = (size - offset);

        m_audio_frame_info.emplace_back(info);
    }
}

void fcMP4StreamWriter::setAACHeader(const Buffer& aacheader)
{
    u8 *ptr = (u8*)aacheader.ptr();
    m_audio_header.assign(ptr, ptr + aacheader.size());
}

void fcMP4StreamWriter::mp4End()
{
    const char audio_track_name[] = "UTJ Sound Media Handler";
    const char video_track_name[] = "UTJ Video Media Handler";
    const char video_compression_name[31] = "AVC Coding";

    const fcMP4Config& c = m_conf;
    const u32 ctime = (u32)fcGetMacTime();
    const u32 video_unit_duration = 1000; // millisec
    const u32 audio_unit_duration = 1000; // millisec
    u32 video_duration = 0;
    u32 audio_duration = 0;

    std::vector<fcSampleToChunk> video_samples_to_chunk;
    std::vector<fcSampleToChunk> audio_samples_to_chunk;
    std::vector<fcOffsetValue> video_decode_times;
    std::vector<fcOffsetValue> audio_decode_times;
    std::vector<u64> video_chunks;
    std::vector<u64> audio_chunks;

    if (m_iframe_ids.empty()) {
        m_iframe_ids.push_back(1);
    }

    // compute decode times
    auto compute_decode_times = [](
        std::vector<fcFrameInfo>& frame_info,
        std::vector<fcOffsetValue>& decode_times) -> u32
    {
        u32 total_duration_ms = 0;
        for (size_t i = 1; i < frame_info.size(); ++i) {
            auto& prev = frame_info[i - 1];
            auto& cur = frame_info[i];
            uint32_t duration = (cur.timestamp - prev.timestamp) / 1000000; // nanosec to millisec
            total_duration_ms += duration;

            if (!decode_times.empty() && decode_times.back().value == duration) {
                decode_times.back().count++;
            }
            else {
                fcOffsetValue ov;
                ov.count = 1;
                ov.value = duration;
                decode_times.emplace_back(ov);
            }
        }
        return total_duration_ms;
    };
    video_duration = compute_decode_times(m_video_frame_info, video_decode_times);
    audio_duration = compute_decode_times(m_audio_frame_info, audio_decode_times);


    // compute chunk data
    auto compute_chunk_data = [](
        std::vector<fcFrameInfo>& frame_info,
        std::vector<u64>& chunks,
        std::vector<fcSampleToChunk>& samples_to_chunk)
    {
        for (size_t i = 0; i < frame_info.size(); ++i) {
            auto* cur = &frame_info[i];
            auto* prev = i > 0 ? &frame_info[i - 1] : nullptr;

            if (!prev || prev->file_offset + prev->size != cur->file_offset)
            {
                chunks.push_back(cur->file_offset);

                fcSampleToChunk stc;
                stc.first_chunk_ID = (uint32_t)chunks.size();
                stc.samples_per_chunk = 1;
                stc.sample_description_ID = 1;
                samples_to_chunk.emplace_back(stc);
            }
            else {
                samples_to_chunk.back().samples_per_chunk++;
            }
        }
    };
    compute_chunk_data(m_video_frame_info, video_chunks, video_samples_to_chunk);
    compute_chunk_data(m_audio_frame_info, audio_chunks, audio_samples_to_chunk);


    //------------------------------------------------------
    // moov section
    //------------------------------------------------------

    BinaryStream& bs = m_stream;
    Box box = Box(bs);
    size_t mdat_end = bs.tellp();

    u32 track_index = 0;

    box(u32_be('moov'), [&]() {

        //------------------------------------------------------
        // header
        //------------------------------------------------------
        box(u32_be('mvhd'), [&]() {
            bs << u32(0);                       // version and flags (none)
            bs << u32_be(ctime);                // creation time
            bs << u32_be(ctime);                // modified time
            bs << u32_be(video_unit_duration);  // time base (milliseconds = 1000)
            bs << u32_be(video_duration);       // duration (in time base units)
            bs << u32_be(0x00010000);           // fixed point playback speed 1.0
            bs << u16_be(0x0100);               // fixed point vol 1.0
            bs << u64(0);                       // reserved (10 bytes)
            bs << u16(0);
            bs << u32_be(0x00010000) << u32_be(0x00000000) << u32_be(0x00000000); // window matrix row 1 (1.0, 0.0, 0.0)
            bs << u32_be(0x00000000) << u32_be(0x00010000) << u32_be(0x00000000); // window matrix row 2 (0.0, 1.0, 0.0)
            bs << u32_be(0x00000000) << u32_be(0x00000000) << u32_be(0x40000000); // window matrix row 3 (0.0, 0.0, 16384.0)
            bs << u32(0);   // prevew start time (time base units)
            bs << u32(0);   // prevew duration (time base units)
            bs << u32(0);   // still poster frame (timestamp of frame)
            bs << u32(0);   // selection(?) start time (time base units)
            bs << u32(0);   // selection(?) duration (time base units)
            bs << u32(0);   // current time (0, time base units)
            bs << u32_be(!m_audio_frame_info.empty() ? 3 : 2);// next free track id (1-based rather than 0-based)
        });

        //------------------------------------------------------
        // audio track
        //------------------------------------------------------
        if (!m_audio_frame_info.empty()) {
            ++track_index;

            Buffer dd_buf; // decoder descriptor
            {
                Buffer add_buf; //  audio decoder descriptor
                BufferStream dd(dd_buf);
                BufferStream add(add_buf);
                add << u8(64)
                    << u8(0x15)         // stream/type flags.  always 0x15 for my purposes.
                    << u8(0)            // buffer size, just set it to 1536 for both mp3 and aac
                    << u16_be(0x600)
                    << u32_be(c.audio_bitrate) // max bit rate (cue bill 'o reily meme for these two)
                    << u32_be(c.audio_bitrate) // avg bit rate
                    << u8(0x5)          //decoder specific descriptor type
                    << u8(m_audio_header.size() - 2);
                add.write(&m_audio_header[2], m_audio_header.size() - 2);

                dd << u16(0);   // es id
                dd << u8(0);    // stream priority
                dd << u8(4);    // descriptor type
                dd << u8(add_buf.size());
                dd.write(add_buf.ptr(), add_buf.size());
                dd << u8(0x6);  // config descriptor type
                dd << u8(1);    // len
                dd << u8(2);    // SL value(? always 2)
            }

            box(u32_be('trak'), [&]() {
                box(u32_be('tkhd'), [&]() {
                    bs << u32_be(0x00000006);   // version (0) and flags (0xF)
                    bs << u32_be(ctime);        // creation time
                    bs << u32_be(ctime);        // modified time
                    bs << u32_be(track_index);  // track ID
                    bs << u32(0);               // reserved
                    bs << u32_be(audio_duration);// duration (in time base units)
                    bs << u64(0);               // reserved
                    bs << u16(0);               // video layer (0)
                    bs << u16_be(0);            // quicktime alternate track id
                    bs << u16_be(0x0100);       // volume
                    bs << u16(0);               // reserved
                    bs << u32_be(0x00010000) << u32_be(0x00000000) << u32_be(0x00000000); // window matrix row 1 (1.0, 0.0, 0.0)
                    bs << u32_be(0x00000000) << u32_be(0x00010000) << u32_be(0x00000000); // window matrix row 2 (0.0, 1.0, 0.0)
                    bs << u32_be(0x00000000) << u32_be(0x00000000) << u32_be(0x40000000); // window matrix row 3 (0.0, 0.0, 16384.0)
                    bs << u32(0);               // video width (fixed point)
                    bs << u32(0);               // video height (fixed point)
                });
                box(u32_be('mdia'), [&]() {
                    box(u32_be('mdhd'), [&]() {
                        bs << u32(0);                       // version and flags (none)
                        bs << u32_be(ctime);                // creation time
                        bs << u32_be(ctime);                // modified time
                        bs << u32_be(c.audio_sample_rate);  // time scale
                        bs << u32_be(audio_unit_duration);
                        bs << u32_be(0x55C40000);
                    }); // mdhd
                    box(u32_be('hdlr'), [&]() {
                        bs << u32(0);           // version and flags (none)
                        bs << u32(0);           // quicktime type (none)
                        bs << u32_be('soun');   // media type
                        bs << u32(0);           // manufacturer reserved
                        bs << u32(0);           // quicktime component reserved flags
                        bs << u32(0);           // quicktime component reserved mask
                        bs.write(audio_track_name, strlen(audio_track_name) + 1); //track name
                    }); // hdlr
                    box(u32_be('minf'), [&]() {
                        box(u32_be('smhd'), [&]() {
                            bs << u32(0); // version and flags (none)
                            bs << u32(0); // balance (fixed point)
                        });
                        box(u32_be('dinf'), [&]() {
                            box(u32_be('dref'), [&]() {
                                bs << u32(0);       // version and flags (none)
                                bs << u32_be(1);    // count
                                box(u32_be('url '), [&]() {
                                    bs << u32_be(0x00000001); // version (0) and flags (1)
                                }); // url
                            }); // dref
                        }); // dinf
                        box(u32_be('stbl'), [&]() {
                            box(u32_be('stsd'), [&]() {
                                bs << u32(0);       //version and flags (none)
                                bs << u32_be(1);    //count
                                box(u32_be('mp4a'), [&]() {
                                    bs << u32(0);       // reserved (6 bytes)
                                    bs << u16(0);
                                    bs << u16_be(1);    // dref index
                                    bs << u16(0);       // quicktime encoding version
                                    bs << u16(0);       // quicktime encoding revision
                                    bs << u32(0);       // quicktime audio encoding vendor
                                    bs << u16(2);       // channels (ignored)
                                    bs << u16_be(16);   // sample size
                                    bs << u16(0);       // quicktime audio compression id
                                    bs << u16(0);       // quicktime audio packet size
                                    bs << u32_be(c.audio_sample_rate << 16); // sample rate (fixed point)
                                    box(u32_be('esds'), [&]() {
                                        bs << u32(0);   // version and flags (none)
                                        bs << u8(3);    // ES descriptor type
                                        bs << u8(dd_buf.size());
                                        bs.write(dd_buf.ptr(), dd_buf.size());
                                    }); // esds
                                }); // mp4a
                            }); // stsd

                            box(u32_be('stts'), [&]() {
                                bs << u32(0);   // version and flags (none)
                                bs << u32_be(audio_decode_times.size());
                                for (auto& v : audio_decode_times) {
                                    bs << u32_be(v.count) << u32_be(v.value);
                                }
                            });

                            box(u32_be('stsc'), [&]() {
                                bs << u32(0);   // version and flags (none)
                                bs << u32_be(audio_samples_to_chunk.size());
                                for (auto& v : audio_samples_to_chunk) {
                                    bs << u32_be(v.first_chunk_ID) << u32_be(v.samples_per_chunk) << u32(u32_be(1));
                                }
                            });

                            box(u32_be('stsz'), [&]() {
                                bs << u32(0);   // version and flags (none)
                                bs << u32(0);   // block size for all (0 if differing sizes)
                                bs << u32_be(m_audio_frame_info.size());
                                for (auto& v : m_audio_frame_info) {
                                    bs << u32_be(v.size);
                                }
                            });

                            if (!audio_chunks.empty() && audio_chunks.back() > 0xFFFFFFFFLL)
                            {
                                box(u32_be('co64'), [&]() {
                                    bs << u32(0); // version and flags (none)
                                    bs << u32_be(audio_chunks.size());
                                    for (auto &v : audio_chunks) {
                                        bs << u64_be(v);
                                    }
                                });
                            }
                            else
                            {
                                box(u32_be('stco'), [&]() {
                                    bs << u32(0); // version and flags (none)
                                    bs << u32_be(audio_chunks.size());
                                    for (auto &v : audio_chunks) {
                                        bs << u32_be(v);
                                    }
                                });
                            }
                        }); // stbl
                    }); // minf
                }); // mdia
            }); // trak
        }

        //------------------------------------------------------
        // video track
        //------------------------------------------------------
        if (!m_video_frame_info.empty()) {
            ++track_index;
            box(u32_be('trak'), [&]() {
                box(u32_be('tkhd'), [&]() {
                    bs << u32_be(0x00000006);       // version (0) and flags (0x6)
                    bs << u32_be(ctime);            // creation time
                    bs << u32_be(ctime);            // modified time
                    bs << u32_be(track_index);      // track ID
                    bs << u32(0);                   // reserved
                    bs << u32_be(video_duration);   // duration (in time base units)
                    bs << u64(0);                   // reserved
                    bs << u16(0);                   // video layer (0)
                    bs << u16(0);                   // quicktime alternate track id (0)
                    bs << u16(0);                   // track audio volume (this is video, so 0)
                    bs << u16(0);                   // reserved
                    bs << u32_be(0x00010000) << u32_be(0x00000000) << u32_be(0x00000000); //window matrix row 1 (1.0, 0.0, 0.0)
                    bs << u32_be(0x00000000) << u32_be(0x00010000) << u32_be(0x00000000); //window matrix row 2 (0.0, 1.0, 0.0)
                    bs << u32_be(0x00000000) << u32_be(0x00000000) << u32_be(0x40000000); //window matrix row 3 (0.0, 0.0, 16384.0)
                    bs << u32_be(c.video_width << 16);  // video width (fixed point)
                    bs << u32_be(c.video_height << 16); // video height (fixed point)
                }); // tkhd

                box(u32_be('mdia'), [&]() {
                    box(u32_be('mdhd'), [&]() {
                        bs << u32(0);           // version and flags (none)
                        bs << u32_be(ctime);    // creation time
                        bs << u32_be(ctime);    // modified time
                        bs << u32_be(video_unit_duration);  // time scale
                        bs << u32_be(video_duration);
                        bs << u32_be(0x55c40000);
                    }); // mdhd
                    box(u32_be('hdlr'), [&]() {
                        bs << u32(0);           // version and flags (none)
                        bs << u32(0);           // quicktime type (none)
                        bs << u32_be('vide');   // media type
                        bs << u32(0);           // manufacturer reserved
                        bs << u32(0);           // quicktime component reserved flags
                        bs << u32(0);           // quicktime component reserved mask
                        bs.write(video_track_name, strlen(video_track_name) + 1); //track name
                    }); // hdlr
                    box(u32_be('minf'), [&]() {
                        box(u32_be('vmhd'), [&]() {
                            bs << u32_be(0x00000001); //version (0) and flags (1)
                            bs << u16(0);       // quickdraw graphic mode (copy = 0)
                            bs << u16(0);       // quickdraw red value
                            bs << u16(0);       // quickdraw green value
                            bs << u16(0);       // quickdraw blue value
                        }); // 
                        box(u32_be('dinf'), [&]() {
                            box(u32_be('dref'), [&]() {
                                bs << u32(0); //version and flags (none)
                                bs << u32_be(1); //count
                                box(u32_be('url '), [&]() {
                                    bs << u32_be(0x00000001); //version (0) and flags (1)
                                }); // dref
                            }); // dref
                        }); // dinf

                        box(u32_be('stbl'), [&]() {
                            box(u32_be('stsd'), [&]() {
                                bs << u32(0);       // version and flags (none)
                                bs << u32_be(1);    // count
                                box(u32_be('avc1'), [&]() {
                                    bs << u32(0);               //reserved 6 bytes
                                    bs << u16(0);
                                    bs << u16_be(1);            // index
                                    bs << u16(0);               // encoding version
                                    bs << u16(0);               // encoding revision level
                                    bs << u32(0);               // encoding vendor
                                    bs << u32(0);               // temporal quality
                                    bs << u32(0);               // spatial quality
                                    bs << u16_be(c.video_width);    // video_width
                                    bs << u16_be(c.video_height);   // video_height
                                    bs << u32_be(0x00480000);   // fixed point video_width pixel resolution (72.0)
                                    bs << u32_be(0x00480000);   // fixed point video_height pixel resolution (72.0)
                                    bs << u32(0);               // quicktime video data size 
                                    bs << u16_be(1);            // frame count(?)
                                    bs << u8(strlen(video_compression_name)); // compression name length
                                    bs.write(video_compression_name, sizeof(video_compression_name)); // 31 bytes for the name
                                    bs << u16(0);               // 
                                    bs << u16(0xFFFF);          // quicktime video color table id (none = -1)
                                    box(u32_be('avcC'), [&]() {
                                        bs << u8(1);            // version
                                        bs << u8(0x42);         // h264 profile ID
                                        bs << u8(0xc0);         // h264 compatible profiles
                                        bs << u8(0x14);         // h264 level
                                        bs << u8(0xff);         // reserved
                                        bs << u8(0xe1);         // first half-byte = no clue. second half = sps count
                                        bs << u16_be(m_sps.size()); // sps size
                                        bs.write(&m_sps[0], m_sps.size()); // sps data
                                        bs << u8(1); // pps count
                                        bs << u16_be(m_pps.size()); // pps size
                                        bs.write(&m_pps[0], m_pps.size()); // pps data
                                    }); // 
                                }); // avc1
                            }); // stsd

                            box(u32_be('stts'), [&]() {
                                bs << u32(0); // version and flags (none)
                                bs << u32_be(video_decode_times.size());
                                for (auto& v : video_decode_times)
                                {
                                    bs << u32_be(v.count);
                                    bs << u32_be(v.value);
                                }
                            }); // stts

                            if (m_iframe_ids.size())
                            {
                                box(u32_be('stss'), [&]() {
                                    bs << u32(0); // version and flags (none)
                                    bs << u32_be(m_iframe_ids.size());
                                    for (u32 v : m_iframe_ids) {
                                        bs << u32_be(v);
                                    }
                                }); // stss
                            }

                            box(u32_be('stsc'), [&]() {
                                bs << u32(0); // version and flags (none)
                                bs << u32_be(video_samples_to_chunk.size());
                                for (auto& v : video_samples_to_chunk)
                                {
                                    bs << u32_be(v.first_chunk_ID);
                                    bs << u32_be(v.samples_per_chunk);
                                    bs << u32_be(v.sample_description_ID);
                                }
                            }); // stsc

                            box(u32_be('stsz'), [&]() {
                                bs << u32(0); // version and flags (none)
                                bs << u32(0); // block size for all (0 if differing sizes)
                                bs << u32_be(m_video_frame_info.size());
                                for (auto& v : m_video_frame_info) {
                                    bs << u32_be(v.size);
                                }
                            }); // stsz

                            if (!video_chunks.empty() && video_chunks.back() > 0xFFFFFFFFLL) {
                                box(u32_be('co64'), [&]() {
                                    bs << u32(0); // version and flags (none)
                                    bs << u32_be(video_chunks.size());
                                    for (auto& v : video_chunks) {
                                        bs << u64_be(v);
                                    }
                                }); // co64
                            }
                            else {
                                box(u32_be('stco'), [&]() {
                                    bs << u32(0); // version and flags (none)
                                    bs << u32_be(video_chunks.size());
                                    for (auto& v : video_chunks) {
                                        bs << u32_be(v);
                                    }
                                }); // stco
                            }
                        }); // stbl
                    }); // minf
                }); // mdia
            }); // trak
        }
    }); // moov

#ifdef fcMP464BitLength
    {
        // 64bit mdat length
        u64 mdat_size = u64_be(mdat_end - m_mdat_begin);
        bs.seekp(m_mdat_begin + 8);
        bs.write(&mdat_size, sizeof(mdat_size));
    }
#else
    {
        // 32bit mdat length
        u32 mdat_size = u32_be(mdat_end - m_mdat_begin);
        bs.seekp(m_mdat_begin);
        bs.write(&mdat_size, sizeof(mdat_size));
    }
#endif

    fcDebugLog("fcMP4Stream::mp4End() done.");
}
