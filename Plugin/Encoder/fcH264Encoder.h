﻿#ifndef fcMP4Encoder_h
#define fcMP4Encoder_h


struct fcH264EncoderConfig
{
    int width;
    int height;
    int target_bitrate;
    int max_framerate;

    fcH264EncoderConfig() : width(), height(), target_bitrate(), max_framerate() {}
};

class fcIH264Encoder
{
public:
    virtual ~fcIH264Encoder() {}
    virtual const char* getEncoderInfo() = 0;
    virtual bool encode(fcH264Frame& dst, const fcI420Image& image, fcTime timestamp, bool force_keyframe = false) = 0;
};

bool fcDownloadOpenH264(fcDownloadCallback cb);
bool fcLoadOpenH264Module();

fcIH264Encoder* fcCreateOpenH264Encoder(const fcH264EncoderConfig& conf);
fcIH264Encoder* fcCreateNVH264Encoder(const fcH264EncoderConfig& conf);
fcIH264Encoder* fcCreateAMDH264Encoder(const fcH264EncoderConfig& conf);

#endif // fcMP4Encoder_h
