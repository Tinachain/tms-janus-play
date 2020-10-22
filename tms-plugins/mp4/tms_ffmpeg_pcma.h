#define ALAW_BIT_RATE 64000   // alaw比特率
#define ALAW_SAMPLE_RATE 8000 // alaw采样率

#ifndef TMS_PCMA_H
#define TMS_PCMA_H

#include "tms_ffmpeg_mp4.h"
#include "tms_ffmpeg_stream.h"
/**
 * PCMA编码器 
 */
typedef struct PCMAEnc
{
  AVCodec *codec;
  AVCodecContext *cctx;
  int nb_samples;
  AVFrame *frame;
  AVPacket packet;
} PCMAEnc;

/**
 * 重采样 
 */
typedef struct Resampler
{
  SwrContext *swrctx;
  int max_nb_samples; // 重采样缓冲区最大采样数
  int linesize;       // 声道平面尺寸
  uint8_t **data;     // 重采样缓冲区
} Resampler;

int tms_init_pcma_encoder(PCMAEnc *encoder);
int tms_init_audio_resampler(AVCodecContext *input_codec_context,
                             AVCodecContext *output_codec_context,
                             Resampler *resampler);

/* 初始化音频编码器（转换为pcma格式） */
int tms_init_pcma_encoder(PCMAEnc *encoder)
{
  AVCodec *c;
  c = avcodec_find_encoder(AV_CODEC_ID_PCM_ALAW);
  if (!c)
  {
    JANUS_LOG(LOG_VERB, "没有找到alaw编码器\n");
    return -1;
  }
  int i = 0;
  while (c->sample_fmts[i] != -1)
  {
    JANUS_LOG(LOG_VERB, "PCMA编码器，sample_fmts[%d] %s\n", i, av_get_sample_fmt_name(c->sample_fmts[i]));
    i++;
  }

  AVCodecContext *cctx;
  cctx = avcodec_alloc_context3(c);
  if (!cctx)
  {
    JANUS_LOG(LOG_VERB, "分配alaw编码器上下文失败\n");
    return -1;
  }
  /* put sample parameters */
  cctx->bit_rate = ALAW_BIT_RATE;
  cctx->sample_fmt = c->sample_fmts[0];
  cctx->sample_rate = ALAW_SAMPLE_RATE;
  cctx->channel_layout = AV_CH_LAYOUT_MONO;
  cctx->channels = av_get_channel_layout_nb_channels(cctx->channel_layout);

  /* open it */
  if (avcodec_open2(cctx, c, NULL) < 0)
  {
    JANUS_LOG(LOG_VERB, "打开编码器失败\n");
    return -1;
  }

  encoder->codec = c;
  encoder->cctx = cctx;

  return 0;
}
/**
 * Initialize the audio resampler based on the input and encoder codec settings.
 * If the input and encoder sample formats differ, a conversion is required
 * libswresample takes care of this, but requires initialization.
 * @param      input_codec_context  Codec context of the input file
 * @param      output_codec_context Codec context of the encoder file
 * @param[out] resample_context     Resample context for the required conversion
 * @return Error code (0 if successful)
 */
int tms_init_audio_resampler(AVCodecContext *input_codec_context,
                             AVCodecContext *output_codec_context,
                             Resampler *resampler)
{
  int error;

  SwrContext **resample_context = &resampler->swrctx;

  /*
  * Create a resampler context for the conversion.
  * Set the conversion parameters.
  * Default channel layouts based on the number of channels
  * are assumed for simplicity (they are sometimes not detected
  * properly by the demuxer and/or decoder).
  */
  *resample_context = swr_alloc_set_opts(NULL,
                                         av_get_default_channel_layout(output_codec_context->channels),
                                         output_codec_context->sample_fmt,
                                         output_codec_context->sample_rate,
                                         av_get_default_channel_layout(input_codec_context->channels),
                                         input_codec_context->sample_fmt,
                                         input_codec_context->sample_rate,
                                         0, NULL);
  if (!*resample_context)
  {
    JANUS_LOG(LOG_VERB, "Could not allocate resample context\n");
    return AVERROR(ENOMEM);
  }
  /* Open the resampler with the specified parameters. */
  if ((error = swr_init(*resample_context)) < 0)
  {
    JANUS_LOG(LOG_VERB, "Could not open resample context\n");
    swr_free(resample_context);
    return error;
  }

  resampler->data = av_calloc(1, sizeof(**resampler->data));

  return 0;
}
/**
 * 执行音频重采样
 */
int tms_audio_resample(Resampler *resampler, AVFrame *frame, PCMAEnc *encoder)
{
  int ret = 0;

  int nb_resample_samples = av_rescale_rnd(swr_get_delay(resampler->swrctx, frame->sample_rate) + frame->nb_samples, encoder->cctx->sample_rate, frame->sample_rate, AV_ROUND_UP);

  /* 分配缓冲区 */
  if (nb_resample_samples > resampler->max_nb_samples)
  {
    if (resampler->max_nb_samples > 0)
      av_freep(&resampler->data[0]);

    ret = av_samples_alloc(resampler->data, &resampler->linesize, 1, nb_resample_samples, encoder->cctx->sample_fmt, 0);
    if (ret < 0)
    {
      JANUS_LOG(LOG_VERB, "Could not allocate destination samples\n");
      goto end;
    }
    resampler->max_nb_samples = nb_resample_samples;
  }

  ret = swr_convert(resampler->swrctx, resampler->data, nb_resample_samples, (const uint8_t **)frame->data, frame->nb_samples);
  if (ret < 0)
  {
    JANUS_LOG(LOG_VERB, "Could not allocate destination samples\n");
    goto end;
  }

  encoder->nb_samples = nb_resample_samples;

end:
  return ret;
}
/**
 * Initialize one data packet for reading or writing.
 * @param packet Packet to be initialized
 */
void tms_init_pcma_packet(AVPacket *packet)
{
  av_init_packet(packet);
  /* Set the packet data and size so that it is recognized as being empty. */
  packet->data = NULL;
  packet->size = 0;
}
/**
 * @return Error code (0 if successful)
 */
int tms_init_pcma_frame(PCMAEnc *encoder, Resampler *resampler)
{
  int error;

  AVFrame *frame;
  AVCodecContext *cctx = encoder->cctx;
  int nb_samples = encoder->nb_samples;

  /* Create a new frame to store the audio samples. */
  if (!(frame = av_frame_alloc()))
  {
    JANUS_LOG(LOG_VERB, "Could not allocate encoder frame\n");
    return AVERROR_EXIT;
  }
  /* Set the frame's parameters, especially its size and format.
     * av_frame_get_buffer needs this to allocate memory for the
     * audio samples of the frame.
     * Default channel layouts based on the number of channels
     * are assumed for simplicity. */
  frame->nb_samples = nb_samples;
  frame->channel_layout = cctx->channel_layout;
  frame->format = cctx->sample_fmt;
  frame->sample_rate = cctx->sample_rate;
  /* Allocate the samples of the created frame. This call will make
     * sure that the audio frame can hold as many samples as specified. */
  if ((error = av_frame_get_buffer(frame, 0)) < 0)
  {
    JANUS_LOG(LOG_VERB, "Could not allocate encoder frame samples (error '%s')\n",
              av_err2str(error));
    av_frame_free(&frame);
    return error;
  }

  memcpy(frame->data[0], *(resampler->data), nb_samples * 2);

  encoder->frame = frame;

  return 0;
}

/* 输出音频帧调试信息 */
static void tms_dump_audio_frame(AVFrame *frame, TmsPlayContext *play)
{
  // 	uint8_t *frame_dat = frame->extended_data[0];
  // 	ast_debug(2, "frame前8个字节 %02x %02x %02x %02x %02x %02x %02x %02x \n", frame_dat[0], frame_dat[1], frame_dat[2], frame_dat[3], frame_dat[4], frame_dat[5], frame_dat[6], frame_dat[7]);

  const char *frame_fmt = av_get_sample_fmt_name(frame->format);

  JANUS_LOG(LOG_VERB, "从音频包 #%d 中读取音频帧 #%d, format = %s , sample_rate = %d , channels = %d , nb_samples = %d, pts = %ld, best_effort_timestamp = %ld\n", play->nb_audio_packets, play->nb_audio_frames, frame_fmt, frame->sample_rate, frame->channels, frame->nb_samples, frame->pts, frame->best_effort_timestamp);
}
/* 添加音频帧发送延时 */
static void tms_add_audio_frame_send_delay(AVFrame *frame, TmsPlayContext *play)
{
  int64_t pts = av_rescale(frame->pts, AV_TIME_BASE, frame->sample_rate);
  int64_t elapse = av_gettime_relative() - play->start_time_us - play->pause_duration_us;
  JANUS_LOG(LOG_VERB, "计算音频帧 #%d 发送延时 elapse = %ld pts = %ld delay = %ld\n", play->nb_audio_frames, elapse, pts, pts - elapse);
  if (pts > elapse)
  {
    usleep(pts - elapse);
  }
}
/* 处理音频媒体包 */
int tms_handle_audio_packet(TmsPlayContext *play, TmsInputStream *ist, Resampler *resampler, PCMAEnc *pcma_enc, AVPacket *pkt, AVFrame *frame)
{
  int ret = 0;

  play->nb_audio_packets++;
  /* 将媒体包发送给解码器 */
  if ((ret = avcodec_send_packet(ist->dec_ctx, pkt)) < 0)
  {
    JANUS_LOG(LOG_VERB, "读取音频包 #%d 失败 %s\n", play->nb_audio_packets, av_err2str(ret));
    return -1;
  }
  int nb_packet_frames = 0;
  JANUS_LOG(LOG_VERB, "读取音频包 #%d size= %d \n", play->nb_audio_packets, pkt->size);

  while (1)
  {
    /* 从解码器获取音频帧 */
    ret = avcodec_receive_frame(ist->dec_ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    {
      if (nb_packet_frames == 0)
      {
        JANUS_LOG(LOG_VERB, "未从音频包 #%d 中读取音频帧，原因：%s\n", play->nb_audio_packets, av_err2str(ret));
      }
      break;
    }
    else if (ret < 0)
    {
      JANUS_LOG(LOG_VERB, "读取音频帧 #%d 错误 %s\n", play->nb_audio_frames + 1, av_err2str(ret));
      return -1;
    }
    nb_packet_frames++;
    play->nb_audio_frames++;
    tms_dump_audio_frame(frame, play);

    /* 添加发送间隔 */
    tms_add_audio_frame_send_delay(frame, play);

    /* 对获得的音频帧执行重采样 */
    ret = tms_audio_resample(resampler, frame, pcma_enc);
    if (ret < 0)
    {
      return -1;
    }
    /* 重采样后的媒体帧 */
    ret = tms_init_pcma_frame(pcma_enc, resampler);
    if (ret < 0)
    {
      return -1;
    }
    play->nb_pcma_frames++;

    /* 音频帧送编码器准备编码 */
    if ((ret = avcodec_send_frame(pcma_enc->cctx, pcma_enc->frame)) < 0)
    {
      JANUS_LOG(LOG_VERB, "音频帧发送编码器错误\n");
      return -1;
    }

    /* 要通过rtp输出的包 */
    tms_init_pcma_packet(&pcma_enc->packet);

    while (1)
    {
      ret = avcodec_receive_packet(pcma_enc->cctx, &pcma_enc->packet);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      {
        break;
      }
      else if (ret < 0)
      {
        JANUS_LOG(LOG_VERB, "Error encoding audio frame\n");
        return -1;
      }
      /* 计算时间戳，单位毫秒（milliseconds） a*b/c */
      // audio_rtp_ctx->cur_timestamp += av_rescale(pcma_enc->nb_samples, AV_TIME_BASE, RTP_PCMA_TIME_BASE) / 1000;
      // if (!player->first_rtcp_auido)
      // {
      //   tms_audio_rtcp_first_sr(player, audio_rtp_ctx);
      // }
      /* 通过rtp发送音频 */
      // tms_rtp_send_audio(audio_rtp_ctx, pcma_enc, player);
    }
    av_packet_unref(&pcma_enc->packet);
    av_frame_free(&pcma_enc->frame);
  }

  return 0;
}

#endif