/*
   This file is part of harvid

   Copyright (C) 2007-2013 Robin Gareus <robin@gareus.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdint.h>     /* uint8_t */
#include <stdlib.h>     /* calloc et al.*/
#include <string.h>     /* memset */
#include <pthread.h>

#include "ffcompat.h"
#include <libswscale/swscale.h>

#include "vinfo.h"
#include "ffdecoder.h"

/* xj5 seek modes */
enum {  SEEK_ANY, ///< directly seek to givenvideo frame
        SEEK_KEY, ///< seek to next keyframe after given frame.
        SEEK_CONTINUOUS, ///< seek to keframe before this frame and advance to current frame.
        SEEK_LIVESTREAM, ///< decode until next keyframe in a live-stream and set initial PTS offset; later decode cont. until PTS match
};


/* ffmpeg source */
typedef struct {
  /* file specific decoder settings */
  int   want_ignstart; //< set before calling ff_open_movie()
  int   want_genpts;
  int   seekflags;
  /* Video File Info */
  int   movie_width;  ///< original file geometry
  int   movie_height; ///< original file geometry
  int   out_width;  ///< aspect scaled geometry
  int   out_height; ///< aspect scaled geometry

  double duration;
  double framerate;
  double file_frame_offset;
  long   frames;
  char  *current_file;
  /* helper variables */
  double tpf;
  int64_t avprev;
  int64_t stream_pts_offset;
  /* */
  uint8_t *internal_buffer; //< if !NULL this buffer is free()d on destroy
  uint8_t *buffer;
  int   buf_width;  ///< current geometry for allocated buffer
  int   buf_height; ///< current geometry for allocated buffer
  int   videoStream;
  int   render_fmt;  //< pFrame/buffer output format (RGB24)
  /* ffmpeg internals*/
  AVPacket          packet;
  AVFormatContext   *pFormatCtx;
  AVCodecContext    *pCodecCtx;
  AVFrame           *pFrame;
  AVFrame           *pFrameFMT;
  struct SwsContext *pSWSCtx;
} ffst;

#include <time.h>
#include <math.h>
#include <getopt.h>
#include <sys/time.h>
#include <unistd.h>

/* Option flags and global variables */
extern int want_quiet;
extern int want_verbose;

static pthread_mutex_t avcodec_lock;
static const AVRational c1_Q = { 1, 1 };

//#define SCALE_UP  ///< positive pixel-aspect scales up X axis - else positive pixel-aspect scales down Y-Axis.

//--------------------------------------------
// Manage video file
//--------------------------------------------

static void ff_getbuffersize(void *ptr, size_t *s) {
  ffst *ff=(ffst*)ptr;
  *s=(avpicture_get_size(ff->render_fmt, ff->out_width, ff->out_height));
  //*s=(avpicture_get_size(ff->render_fmt, ff->movie_width, ff->movie_height));
}

static void render_empty_frame(ffst *ff, uint8_t* buf, int w, int h, int xoff, int ys) {
  fprintf(stderr, "render_empty_frame NYI\n"); // TODO
}


float ff_get_aspectratio(void *ptr) {
  ffst *ff=(ffst*)ptr;
  float aspect_ratio;
  if (ff->pCodecCtx->sample_aspect_ratio.num == 0)
    aspect_ratio = 0;
  else
    aspect_ratio = av_q2d(ff->pCodecCtx->sample_aspect_ratio)
                   * (float)ff->pCodecCtx->width / (float)ff->pCodecCtx->height;
  if (aspect_ratio <= 0.0)
    aspect_ratio = (float)ff->pCodecCtx->width / (float)ff->pCodecCtx->height;
  return (aspect_ratio);
}

void ff_init_moviebuffer(void *ptr) {
  size_t numBytes;
  ffst *ff=(ffst*)ptr;

  float aspect_ratio = ff_get_aspectratio(ptr);
  if (ff->out_height <0  && ff->out_width >0) ff->out_height = (int) floorf((float)ff->out_width/aspect_ratio);
  else if (ff->out_height >0  && ff->out_width <0) ff->out_width = (int) floorf((float)ff->out_height*aspect_ratio);

  #ifdef SCALE_UP
  if (ff->out_width  <0 ) ff->out_width  = (int) floorf((float)ff->pCodecCtx->height * aspect_ratio);
  if (ff->out_height <0 ) ff->out_height = ff->pCodecCtx->height;
  #else
  if (ff->out_width  <0 ) ff->out_width = ff->pCodecCtx->width ;
  if (ff->out_height <0 )  ff->out_height  = (int) floorf((float)ff->pCodecCtx->width / aspect_ratio);
  #endif

  if (ff->buf_width == ff->out_width && ff->buf_height == ff->out_height) {
    return;
  }

  if (ff->internal_buffer) free(ff->internal_buffer);
  ff_getbuffersize(ff, &numBytes);
  ff->internal_buffer=(uint8_t *) calloc(numBytes, sizeof(uint8_t));
  ff->buffer = ff->internal_buffer;
  ff->buf_width = ff->out_width;
  ff->buf_height = ff->out_height;
  if (!ff->buffer) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  if (!ff->pFrameFMT) {
    // XXX can this really happen?
    fprintf(stderr, "could not initialize output frame/scaling.\n");
    exit(1);
  }
  avpicture_fill((AVPicture *)ff->pFrameFMT, ff->buffer, ff->render_fmt, ff->out_width, ff->out_height);
}

void ff_initialize (void) {
  if (want_verbose) fprintf(stdout, "FFMPEG: registering codecs.\n");
  av_register_all();
  avcodec_register_all();
  pthread_mutex_init(&avcodec_lock, NULL);
  if(!want_verbose) av_log_set_level(AV_LOG_QUIET);
}

void ff_cleanup (void) {
  pthread_mutex_destroy(&avcodec_lock);
}

int ff_close_movie(void *ptr) {
  ffst *ff=(ffst*)ptr;
  if(ff->current_file) free(ff->current_file);
  ff->current_file=NULL;

  if (!ff->pFrameFMT) return(-1);
  ff_set_bufferptr(ff, ff->internal_buffer); // restore allocated movie-buffer..
  if (ff->internal_buffer) free(ff->internal_buffer); // done in pFrameFMT?
  if (ff->pFrameFMT) av_free(ff->pFrameFMT);
  if (ff->pFrame) av_free(ff->pFrame);
  ff->buffer=NULL;ff->pFrameFMT=ff->pFrame=NULL;
  pthread_mutex_lock(&avcodec_lock);
  avcodec_close(ff->pCodecCtx);
  avformat_close_input(&ff->pFormatCtx);
  pthread_mutex_unlock(&avcodec_lock);
  if (ff->pSWSCtx) sws_freeContext(ff->pSWSCtx);
  return (0);
}

void ff_get_framerate(void *ptr, TimecodeRate *fr) {
  ffst *ff=(ffst*)ptr;
  AVStream *av_stream;
  if (!ff->current_file || !ff->pFormatCtx) {
    return;
  }
  av_stream = ff->pFormatCtx->streams[ff->videoStream];

  if(av_stream->r_frame_rate.den && av_stream->r_frame_rate.num) {
    fr->num = av_stream->r_frame_rate.num;
    fr->den = av_stream->r_frame_rate.den;
 // if ((ff->framerate < 4 || ff->framerate > 100 ) && (av_stream->time_base.num && av_stream->time_base.den)) {
 //   fr->num = av_stream->time_base.den
 //   fr->den = av_stream->time_base.num;
 // }
  } else {
    fr->num = av_stream->time_base.den;
    fr->den = av_stream->time_base.num;
  }

  fr->drop=0;
  if ((ff->framerate == 29.97) || (ff->framerate == 30000.0/1001.0))
    fr->drop=1;
}


void ff_set_framerate(ffst *ff) {
  AVStream *av_stream;
  av_stream = ff->pFormatCtx->streams[ff->videoStream];

  if(av_stream->r_frame_rate.den && av_stream->r_frame_rate.num) {
    ff->framerate = av_q2d(av_stream->r_frame_rate);
    if ((ff->framerate < 4 || ff->framerate > 100 ) && (av_stream->time_base.num && av_stream->time_base.den))
      ff->framerate = 1.0/av_q2d(av_stream->time_base);
  }
}

int ff_open_movie(void *ptr, char *file_name, int render_fmt) {
  int i;
  AVCodec *pCodec;
  ffst *ff = (ffst*) ptr;

  if (ff->pFrameFMT) {
    if (ff->current_file && !strcmp(file_name, ff->current_file)) return(0);
    /* close currently open movie */
    if (!want_quiet)
      fprintf(stderr,"replacing current video file buffer\n");
    ff_close_movie(ff);
  }

  // initialize values
  ff->pFormatCtx = NULL;
  ff->pFrameFMT = NULL;
  ff->movie_width  = 320;
  ff->movie_height = 180;
  ff->buf_width = ff->buf_height = 0;
  ff->movie_height = 180;
  ff->framerate = ff->duration = ff->frames = 1;
  ff->file_frame_offset = 0.0;
  ff->videoStream=-1;
  ff->tpf=1.0;
  ff->avprev=0;
  ff->stream_pts_offset=AV_NOPTS_VALUE;
  ff->render_fmt=render_fmt;

  /* Open video file */
  if(avformat_open_input(&ff->pFormatCtx, file_name, NULL, NULL) <0)
  {
    fprintf( stderr, "Cannot open video file %s\n", file_name);
    return (-1);
  }
#if 1 /// XXX http is not neccesarily a live-stream!
  // TODO: ff_seeflags(sf) API to set/get value
  if (!strncmp(file_name, "http://", 7)) {
    ff->seekflags = SEEK_LIVESTREAM;
  } else {
    ff->seekflags = SEEK_CONTINUOUS;
  }
  // TODO: live-stream: remember first pts as offset! -> don't use multiple-decoders for the same stream ?!
#endif

  pthread_mutex_lock(&avcodec_lock);
  /* Retrieve stream information */
  if(avformat_find_stream_info(ff->pFormatCtx, NULL)<0) {
    fprintf( stderr, "Cannot find stream information in file %s\n", file_name);
    avformat_close_input(&ff->pFormatCtx);
    pthread_mutex_unlock(&avcodec_lock);
    return (-1);
  }
  pthread_mutex_unlock(&avcodec_lock);

  if (want_verbose) av_dump_format(ff->pFormatCtx, 0, file_name, 0);

  /* Find the first video stream */
  for(i=0; i<ff->pFormatCtx->nb_streams; i++)
#if LIBAVFORMAT_BUILD > 0x350000
    if(ff->pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO)
#elif LIBAVFORMAT_BUILD > 4629
    if(ff->pFormatCtx->streams[i]->codec->codec_type==CODEC_TYPE_VIDEO)
#else
    if(ff->pFormatCtx->streams[i]->codec.codec_type==CODEC_TYPE_VIDEO)
#endif
    {
      ff->videoStream=i;
      break;
    }

  if(ff->videoStream==-1) {
    fprintf( stderr, "Cannot find a video stream in file %s\n", file_name);
    avformat_close_input(&ff->pFormatCtx);
    return (-1);
  }

  ff_set_framerate(ff);

  {
  AVStream *avs = ff->pFormatCtx->streams[ff->videoStream];

#if 0 // DEBUG duration
  printf("DURATION frames from AVstream: %"PRIi64"\n", avs->nb_frames);
  printf("DURATION duration from AVstream: %"PRIi64"\n", avs->duration /* * av_q2d(avs->r_frame_rate) * av_q2d(avs->time_base)*/);
  printf("DURATION duration from FormatContext: %lu\n", ff->frames = ff->pFormatCtx->duration * ff->framerate / AV_TIME_BASE);
#endif

  if (avs->nb_frames != 0) {
    ff->frames = avs->nb_frames;
  } else if (avs->duration != avs->duration && avs->duration != 0) // ???
    ff->frames = avs->duration * av_q2d(avs->r_frame_rate) * av_q2d(avs->time_base);
  else {
    ff->frames = ff->pFormatCtx->duration * ff->framerate / AV_TIME_BASE;
  }
  ff->duration = (double) avs->duration * av_q2d(avs->time_base);
  }
  ff->tpf = 1.0/(av_q2d(ff->pFormatCtx->streams[ff->videoStream]->time_base)*ff->framerate);
  ff->file_frame_offset =  ff->framerate*((double) ff->pFormatCtx->start_time/ (double) AV_TIME_BASE);

  if (want_verbose) {
    fprintf(stdout, "frame rate: %g\n", ff->framerate);
    fprintf(stdout, "length in seconds: %g\n", ff->duration);
    fprintf(stdout, "total frames: %ld\n", ff->frames);
    fprintf(stdout, "start offset: %.0f [frames]\n", ff->file_frame_offset);
  }

  // Get a pointer to the codec context for the video stream
#if LIBAVFORMAT_BUILD > 4629
  ff->pCodecCtx=ff->pFormatCtx->streams[ff->videoStream]->codec;
#else
  ff->pCodecCtx=&(ff->pFormatCtx->streams[ff->videoStream]->codec);
#endif

// FIXME: don't scale here - announce aspect ratio
// out_width/height remains in aspect 1:1
#ifdef SCALE_UP
  ff->movie_width = (int) floorf((float)ff->pCodecCtx->height * ff_get_aspectratio(ff));
  ff->movie_height = ff->pCodecCtx->height;
#else
  ff->movie_width = ff->pCodecCtx->width;
  ff->movie_height = (int) floorf((float)ff->pCodecCtx->width / ff_get_aspectratio(ff));
#endif

  // somewhere around LIBAVFORMAT_BUILD  4630
#ifdef AVFMT_FLAG_GENPTS
  if (ff->want_genpts) {
    ff->pFormatCtx->flags|=AVFMT_FLAG_GENPTS;
//  ff->pFormatCtx->flags|=AVFMT_FLAG_IGNIDX;
  }
#endif

  if (want_verbose)
    fprintf( stderr, "movie size:  %ix%i px\n", ff->movie_width, ff->movie_height);

  // Find the decoder for the video stream
  pCodec=avcodec_find_decoder(ff->pCodecCtx->codec_id);
  if(pCodec==NULL) {
    fprintf( stderr, "Cannot find a codec for file: %s\n", file_name);
    avformat_close_input(&ff->pFormatCtx);
    return(-1);
  }

  // Open codec
  pthread_mutex_lock(&avcodec_lock);
  if(avcodec_open2(ff->pCodecCtx, pCodec, NULL)<0) {
    fprintf( stderr, "Cannot open the codec for file %s\n", file_name);
    pthread_mutex_unlock(&avcodec_lock);
    avformat_close_input(&ff->pFormatCtx);
    return(-1);
  }
  pthread_mutex_unlock(&avcodec_lock);

  if (!(ff->pFrame=avcodec_alloc_frame())) {
    fprintf( stderr, "Cannot allocate video frame buffer\n");
    avcodec_close(ff->pCodecCtx);
    avformat_close_input(&ff->pFormatCtx);
    return(-1);
  }

  if (!(ff->pFrameFMT=avcodec_alloc_frame())) {
    fprintf( stderr, "Cannot allocate display frame buffer\n");
    av_free(ff->pFrame);
    avcodec_close(ff->pCodecCtx);
    avformat_close_input(&ff->pFormatCtx);
    return(-1);
  }

  ff->out_width=ff->out_height=-1;
  ff_init_moviebuffer(ff);

  ff->current_file=strdup(file_name);
  return(0);
}

void reset_video_head(ffst *ff, AVPacket *packet) {
	int             frameFinished=0;
fprintf(stderr, "DEBUG: resetting decoder - seek/playhead rewind.\n");
#if LIBAVFORMAT_BUILD < 4617
	av_seek_frame(ff->pFormatCtx, ff->videoStream, 0);
#else
	av_seek_frame(ff->pFormatCtx, ff->videoStream, 0, AVSEEK_FLAG_BACKWARD);
#endif
	avcodec_flush_buffers(ff->pCodecCtx);

	while (!frameFinished) {
		av_read_frame(ff->pFormatCtx, packet);
		if(packet->stream_index==ff->videoStream)
#if LIBAVCODEC_VERSION_MAJOR < 52 || ( LIBAVCODEC_VERSION_MAJOR == 52 && LIBAVCODEC_VERSION_MINOR < 21)
		avcodec_decode_video(ff->pCodecCtx, ff->pFrame, &frameFinished, packet->data, packet->size);
#else
		avcodec_decode_video2(ff->pCodecCtx, ff->pFrame, &frameFinished, packet);
#endif
		if(packet->data) av_free_packet(packet);
	}
	ff->avprev=0;
}

// TODO: set this high (>1000) if transport stopped and to a low value (<100) if transport is running.
#define MAX_CONT_FRAMES (1000)

int my_seek_frame (ffst *ff, AVPacket *packet, int64_t timestamp) {
  AVStream *v_stream;
  int rv=1;
  int64_t mtsb = 0;
  int frameFinished;
  int nolivelock = 0;
  static int ffdebug = 0;

  if (ff->videoStream < 0) return (0);
  v_stream = ff->pFormatCtx->streams[ff->videoStream];

  if (ff->want_ignstart)  // timestamps in the file start counting at ..->start_time
    timestamp+= (int64_t) rint(ff->framerate*((double)ff->pFormatCtx->start_time / (double)AV_TIME_BASE));

  // TODO: assert  0 < timestamp + ts_offset - (..->start_time)   < length
	
#if LIBAVFORMAT_BUILD > 4629 // verify this version
  timestamp=av_rescale_q(timestamp,c1_Q,v_stream->time_base);
  timestamp=av_rescale_q(timestamp,c1_Q,v_stream->r_frame_rate); //< timestamp/=framerate;
#endif

#if LIBAVFORMAT_BUILD < 4617
  rv= av_seek_frame(ff->pFormatCtx, ff->videoStream, timestamp / framerate * 1000000LL);
#else
  if (ff->seekflags==SEEK_ANY) {
    rv= av_seek_frame(ff->pFormatCtx, ff->videoStream, timestamp, AVSEEK_FLAG_ANY | AVSEEK_FLAG_BACKWARD) ;
    avcodec_flush_buffers(ff->pCodecCtx);
  } else if (ff->seekflags==SEEK_KEY) {
    rv= av_seek_frame(ff->pFormatCtx, ff->videoStream, timestamp, AVSEEK_FLAG_BACKWARD) ;
    avcodec_flush_buffers(ff->pCodecCtx);
  } else if (ff->seekflags==SEEK_LIVESTREAM) {
  } else /* SEEK_CONTINUOUS */ if (ff->avprev >= timestamp || ((ff->avprev + 32*ff->tpf) < timestamp) ) {
    // NOTE: only seek if last-frame is less then 32 frames behind
    // else read continuously until we get there :D
    // FIXME 32: use keyframes interval of video file or cmd-line-arg as threshold.
    // TODO: do we know if there is a keyframe inbetween now (my_avprev)
    // and the frame to seek to?? - if so rather seek to that frame than read until then.
    // and if no keyframe in between my_avprev and ts - prevent backwards seeks even if
    // timestamp-my_avprev > threshold! - Oh well.

    // seek to keyframe *BEFORE* this frame
    rv= av_seek_frame(ff->pFormatCtx, ff->videoStream, timestamp, AVSEEK_FLAG_BACKWARD) ;
    avcodec_flush_buffers(ff->pCodecCtx);
  }
#endif

  ff->avprev = timestamp;
  if (rv < 0) return (0); // seek failed.

read_frame:
  nolivelock++;
  if(av_read_frame(ff->pFormatCtx, packet)<0) {
    if (!want_quiet) printf("Reached movie end\n");
    return (0);
  }
#if LIBAVFORMAT_BUILD >=4616
  if (av_dup_packet(packet) < 0) {
    printf("can not allocate packet\n");
    goto read_frame;
  }
#endif
  if(packet->stream_index!=ff->videoStream) {
//  fprintf(stderr, "Not a video frame\n");
    if (packet->data) av_free_packet(packet);
// check livelock here ?!
    goto read_frame;
  }
  /* backwards compatible - no cont. seeking (seekmode ANY or KEY ; cmd-arg: -K, -k)
   * do we want a AVSEEK_FLAG_ANY + SEEK_CONTINUOUS option ?? not now.  */
#if LIBAVFORMAT_BUILD < 4617
  return (1);
#endif

  if (   ff->seekflags!=SEEK_CONTINUOUS
      && ff->seekflags!=SEEK_LIVESTREAM
     ) return (1);

  mtsb = packet->pts;
  if (mtsb == AV_NOPTS_VALUE) {
    mtsb = packet->dts;
    if (ffdebug==0) { ffdebug=1; if (!want_quiet) fprintf(stderr,"WARNING: video file does not report pts information.\n         resorting to ffmpeg decompression timestamps.\n         consider to transcode the file or use the --genpts option.\n"); }
  }
  if (mtsb == AV_NOPTS_VALUE) {
    if (ffdebug<2) { ffdebug=2; if (!want_quiet) fprintf(stderr,"ERROR: neither the video file nor the ffmpeg decoder were able to\n       provide a video frame timestamp."); }
    av_free_packet(packet);
    return (0);
  }

#if 1 // experimental
  /* remember live-stream PTS offset */
  if (   ff->seekflags==SEEK_LIVESTREAM
      && mtsb != AV_NOPTS_VALUE
      && ff->stream_pts_offset == AV_NOPTS_VALUE
      && packet->flags&AV_PKT_FLAG_KEY)
  {
    ff->stream_pts_offset = mtsb;
  }

  if (ff->seekflags==SEEK_LIVESTREAM) {
    if (ff->stream_pts_offset != AV_NOPTS_VALUE)
      mtsb -= ff->stream_pts_offset;
    else
      mtsb = AV_NOPTS_VALUE;
  }
#endif

  if (mtsb >= timestamp) {
    return (1); // ok!
  }

  /* skip to next frame */

#if LIBAVCODEC_VERSION_MAJOR < 52 || ( LIBAVCODEC_VERSION_MAJOR == 52 && LIBAVCODEC_VERSION_MINOR < 21)
  avcodec_decode_video(ff->pCodecCtx, ff->pFrame, &frameFinished, packet->data, packet->size);
#else
  avcodec_decode_video2(ff->pCodecCtx, ff->pFrame, &frameFinished, packet);
#endif
  av_free_packet(packet);
  if (!frameFinished) goto read_frame;
  if (nolivelock < MAX_CONT_FRAMES) goto read_frame;
  reset_video_head(ff, packet);
  return (0); // seek failed.
}

/**
 * seeks to frame and decodes and scales video frame
 *
 * @arg ptr handle / ff-data structure
 * @arg frame video frame to seek to
 * @arg buf  unused - see ff_get_bufferptr() - soon: optional buffer-pointer to copy data into
 * @arg w  unused - target width -> out_height parameter when opening file
 * @arg h  unused - target height -> out_height parameter when opening file
 * @arg xoff unused - soon: x-offset of this frame to target buffer
 * @arg xw unused -  really unused
 * @arg ys unused -  soon: y-stride (aka width of container)
 */
int ff_render(void *ptr, unsigned long frame,
    uint8_t* buf, int w, int h, int xoff, int xw, int ys) {
  ffst *ff = (ffst*) ptr;
  int frameFinished = 0;
  int64_t timestamp = (int64_t) frame;

    // if (ff->avprev == timestamp) return;
  //printf("Debug: ff-render %lu\n",frame);

  if (ff->pFrameFMT && ff->pFormatCtx && my_seek_frame(ff, &ff->packet, timestamp)) {
    while (1) { /* Decode video frame */
      frameFinished=0;	
      if(ff->packet.stream_index==ff->videoStream)
#if LIBAVCODEC_VERSION_MAJOR < 52 || ( LIBAVCODEC_VERSION_MAJOR == 52 && LIBAVCODEC_VERSION_MINOR < 21)
	avcodec_decode_video(ff->pCodecCtx, ff->pFrame, &frameFinished, ff->packet.data, ff->packet.size);
#else
	avcodec_decode_video2(ff->pCodecCtx, ff->pFrame, &frameFinished, &ff->packet);
#endif
      if(frameFinished) { /* Convert the image from its native format to FMT */
        ff->pSWSCtx = sws_getCachedContext(ff->pSWSCtx, ff->pCodecCtx->width, ff->pCodecCtx->height, ff->pCodecCtx->pix_fmt, ff->out_width, ff->out_height, ff->render_fmt, SWS_BICUBIC, NULL, NULL, NULL);
        sws_scale(ff->pSWSCtx, (const uint8_t * const*) ff->pFrame->data, ff->pFrame->linesize, 0, ff->pCodecCtx->height, ff->pFrameFMT->data, ff->pFrameFMT->linesize);
	av_free_packet(&ff->packet); /* XXX */
	break;
      } else  {
        //fprintf( stderr, "Frame not finished..\n");
	if(ff->packet.data) av_free_packet(&ff->packet);
	if(av_read_frame(ff->pFormatCtx, &ff->packet)<0) {
	  fprintf( stderr, "read error!\n");
	  reset_video_head(ff, &ff->packet);
	  render_empty_frame(ff, buf, w, h, xoff, ys);
	  break;
	}
#if LIBAVFORMAT_BUILD >=4616
	if (av_dup_packet(&ff->packet) < 0) {
	  printf("can not allocate packet\n");
	  break;
	}
#endif
      }
    } /* end while !frame_finished */
  } else {
    if (ff->pFrameFMT && !want_quiet) fprintf( stderr, "frame seek unsucessful (frame: %lu).\n",frame);
    render_empty_frame(ff, buf, w, h, xoff, ys);
  }
  return frameFinished ? 0 : -1;
}

void ff_get_info(void *ptr, VInfo *i) {
  ffst *ff = (ffst*) ptr;
  if (!i) return;
  // TODO check if move is open.. (not needed, dctrl prevents that)
  i->movie_width = ff->movie_width;
  i->movie_height = ff->movie_height;
  i->movie_aspect = ff_get_aspectratio(ptr);
  i->out_width = ff->out_width;
  i->out_height = ff->out_height;
  ff_getbuffersize(ptr, &i->buffersize);
  i->frames = ff->frames; // ff->duration * ff->framerate;
  //fl2ratio(&(i->framerate->num), &(i->framerate->den), ff->framerate);
  ff_get_framerate(ptr, &i->framerate);
}

void ff_create(void **ff) {
  (*((ffst**)ff)) = (ffst*) calloc(1,sizeof(ffst));
  (*((ffst**)ff))->render_fmt=PIX_FMT_RGB24;
  (*((ffst**)ff))->seekflags = SEEK_CONTINUOUS;
  (*((ffst**)ff))->want_ignstart = 0;
  (*((ffst**)ff))->want_genpts = 0;
  (*((ffst**)ff))->packet.data = NULL;
}

void ff_destroy(void **ff) {
  ff_close_movie(*((ffst**)ff));
  free(*((ffst**)ff));
  *ff= NULL;
}

// buf needs to point to an allocated area of ff->out_width, ff->out_height.
// ffmpeg will directly decode/scale into this buffer.
// if it's NULL an internal buffer will be used.
uint8_t *ff_set_bufferptr(void *ptr, uint8_t *buf) {
  ffst *ff = (ffst*) ptr;
  if (buf)
    ff->buffer=buf;
  else
    ff->buffer=ff->internal_buffer;
  avpicture_fill((AVPicture *)ff->pFrameFMT, ff->buffer, ff->render_fmt, ff->out_width, ff->out_height);
  return (NULL); // return prev. buffer?
}

uint8_t *ff_get_bufferptr(void *ptr) {
  ffst *ff = (ffst*) ptr;
  return ff->buffer;
}

void ff_resize(void *ptr, int w, int h, uint8_t *buf, VInfo *i) {
  ffst *ff = (ffst*) ptr;
  ff->out_width=w;
  ff->out_height=h;
  if (!buf)
    ff_init_moviebuffer(ff);
  else
    ff_set_bufferptr(ptr,buf);
  if (i) ff_get_info(ptr,i);
}

/* vi:set ts=8 sts=2 sw=2: */
