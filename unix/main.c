#include <audacious/util.h>
#include <audacious/configdb.h>
#include <audacious/plugin.h>
#include <audacious/output.h>
#include <audacious/i18n.h>
#include <audacious/strings.h>

#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include "version.h"
#include "../src/vgmstream.h"
#include "gui.h"

#define TM_QUIT 0
#define TM_PLAY 1
#define TM_SEEK 2

#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

extern InputPlugin vgmstream_iplug;
//static CDecoder decoder;
static volatile long decode_seek;
static GThread *decode_thread;
static gint stream_length_samples;
static gint loop_count = 1;
static gint fade_seconds = 0;
static gint fade_delay_seconds = 0;
static gint decode_pos_samples = 0;
static VGMSTREAM *vgmstream = NULL;
static gchar strPlaying[260];
static InputPlugin *vgmstream_iplist[] = { &vgmstream_iplug, NULL };

/*
static gint get_ms_position()
{
  if (vgmstream)
  {
    return (decode_pos_samples * 1000) / vgmstream->sample_rate;
  }
  return 0;
}
*/

/* TODO -

Rewrite vgmstream/STREAMFILE so that you can open a VGMSTREAM* by providing
a custom implementation of STREAMFILE.  Audacious wants you to use the 
VFS functions to do I/O, but VGMSTREAM only supports fopen/fread/etc.
*/

/* Here's a function to convert file://<whatever> to the actual path */
static void get_file_name(char *buffer,const char *pfile)
{
  /* unconvert from file:// to regular path */
  gchar *unescaped = g_uri_unescape_string(pfile,NULL);
  if (strncmp(unescaped,"file://",7) == 0)
  {
    strcpy(buffer,unescaped+7);
  }
  else
  {
    strcpy(buffer,unescaped);
  }
  g_free(unescaped);
}

void vgmstream_mseek(InputPlayback *data,gulong ms);

#define CLOSE_STREAM() do { \
   if (vgmstream) close_vgmstream(vgmstream); \
   vgmstream = NULL; } while (0)

SIMPLE_INPUT_PLUGIN(vgmstream,vgmstream_iplist);

#define DS_EXIT -2

void* vgmstream_play_loop(InputPlayback *playback)
{
  int16_t buffer[576*2];
  long l;
  gint seek_needed_samples;
  gint samples_to_do;
  decode_seek = -1;
  playback->playing = 1;
  playback->eof = 0;
  
  decode_pos_samples = 0;

  while (playback->playing)
  {
    // ******************************************
    // Seeking
    // ******************************************
    // check thread flags, not my favorite method
    if (decode_seek == DS_EXIT)
    {
      goto exit_thread;
    }
    else if (decode_seek >= 0)
    {
      /* compute from ms to samples */
      seek_needed_samples = (long long)decode_seek * vgmstream->sample_rate / 1000L;
      if (seek_needed_samples < decode_pos_samples)
      {
	/* go back in time, reopen file */
	CLOSE_STREAM();
	decode_pos_samples = 0;
	vgmstream = init_vgmstream(strPlaying);
	if (vgmstream)
	{
	  samples_to_do = seek_needed_samples;
	}
	else
	{
	  samples_to_do = -1;
          // trigger eof
	  playback->eof = 1;
	}
      }
      else if (decode_pos_samples < seek_needed_samples)
      {
	/* go forward in time */
	samples_to_do = seek_needed_samples - decode_pos_samples;
      }
      else
      {
	/* seek to where we are, how convenient */
	samples_to_do = -1;
      }
      /* do the actual seeking */
      if (samples_to_do >= 0)
      {
	while (samples_to_do > 0)
	{
	  l = min(576,samples_to_do);
	  render_vgmstream(buffer,l,vgmstream);
	  samples_to_do -= l;
	  decode_pos_samples += l;
	}
	playback->output->flush(decode_seek);
	// reset eof flag
	playback->eof = 0;
      }
      // reset decode_seek
      decode_seek = -1;
    }
    
    // ******************************************
    // Playback
    // ******************************************
    if (!playback->eof)
    {
      // read data and pass onward
      samples_to_do = min(576,stream_length_samples - (decode_pos_samples + 576));
      l = (samples_to_do * vgmstream->channels*2);
      if (!l)
      {
	playback->eof = 1;
	// will trigger on next run through
      }
      else
      {
	// ok we read stuff , pass it on
	render_vgmstream(buffer,samples_to_do,vgmstream);
	/* TODO fading */
	playback->pass_audio(playback,FMT_S16_LE,vgmstream->channels , l , buffer , &playback->playing );

	decode_pos_samples += samples_to_do;
      }
    }
    else
    {
      // at EOF
      playback->output->buffer_free();
      playback->output->buffer_free();
      while (playback->output->buffer_playing())
	g_usleep(10000);
      playback->playing = 0;
      // this effectively ends the loop
    }
  }
 exit_thread:
  decode_seek = -1;
  playback->playing = 0;
  decode_pos_samples = 0;
  CLOSE_STREAM();
  return 0;
}

void vgmstream_about()
{
  vgmstream_gui_about();
}

void vgmstream_configure()
{
  /* TODO */
}

void vgmstream_init()
{

}

void vgmstream_destroy()
{
  
}

gboolean vgmstream_is_our_file(char *pFile)
{
  char strFile[260];
  const char *pExt;
  gchar **exts;
  VGMSTREAM *stream;

  if (!pFile)
    return FALSE;

  /* get extension */
  pExt = strrchr(pFile,'.');
  if (!pExt)
    return FALSE;
  /* skip past period */
  ++pExt;

  get_file_name(strFile,pFile);
  
  for (exts = vgmstream_iplug.vfs_extensions;*exts;++exts)
  {
    if (strcasecmp(pExt,*exts) == 0)
    {
      if ((stream = init_vgmstream(strFile)))
      {
	close_vgmstream(stream);
	return TRUE;
      }      
    }
  }
  return FALSE;
}

void vgmstream_mseek(InputPlayback *data,gulong ms)
{
  if (vgmstream)
  {
    decode_seek = ms;
    data->eof = 0;
    
    while (decode_seek != -1)
      g_usleep(10000);
  }
}

void vgmstream_play(InputPlayback *context)
{
  // this is now called in a new thread context
  char title[260];
  
  get_file_name(title,context->filename);
  
  vgmstream = init_vgmstream(title);
  if (!vgmstream || vgmstream->channels <= 0 || vgmstream->channels > 2)
  {
    CLOSE_STREAM();
    return;
  }
  // open the audio device
  if (context->output->open_audio(FMT_S16_LE,vgmstream->sample_rate,vgmstream->channels) == 0)
  {
    CLOSE_STREAM();
    return;
  }
  /* copy file name */
  strcpy(strPlaying,title);
  // set the info
  stream_length_samples = get_vgmstream_play_samples(loop_count,fade_seconds,fade_delay_seconds,vgmstream);
  gint ms = (stream_length_samples * 1000LL) / vgmstream->sample_rate;
  gint rate   = vgmstream->sample_rate * 2 * vgmstream->channels;
  context->set_params(context,title,
		      /* length */ ms,
		      /* rate */rate,
		      /* freq */vgmstream->sample_rate,
		      /* n channels */vgmstream->channels);
  
  decode_thread = g_thread_self();
  context->set_pb_ready(context);
  vgmstream_play_loop(context);
}

void vgmstream_stop(InputPlayback *context)
{
  if (vgmstream)
  {
    // kill thread
    decode_seek = DS_EXIT;
    // wait for it to die
    g_thread_join(decode_thread);
    // close audio output
  }
  context->output->close_audio();
  // cleanup 
  CLOSE_STREAM();
}

void vgmstream_pause(InputPlayback *context,gshort paused)
{
  context->output->pause(paused);
}

void vgmstream_seek(InputPlayback *context,gint time)
{
  vgmstream_mseek(context,time * 1000);
}

int vgmstream_get_time(InputPlayback *context)
{
  if (!vgmstream)
    return -2;
  
  if (!context->playing || 
      (context->eof && !context->output->buffer_playing()))
    return -1;
  
  return context->output->output_time();
  //return get_ms_position();
}

void vgmstream_get_song_info(gchar *pFile,gchar **title,gint *length)
{
  char strTitle[260];
  VGMSTREAM *infostream;

  get_file_name(strTitle,pFile);

  *title = g_strdup(strTitle);

  if ((infostream = init_vgmstream(strTitle)))
  {
    *length = get_vgmstream_play_samples(loop_count,fade_seconds,fade_delay_seconds,infostream) * 1000LL / infostream->sample_rate;
    close_vgmstream(infostream);
  }
  else
  {
    *length = 0;
  }
}

void vgmstream_file_info_box(gchar *pFile)
{
  char msg[512];
  char strTitle[260];
  VGMSTREAM *stream;
  
  get_file_name(strTitle,pFile);
  
  if ((stream = init_vgmstream(strTitle)))
  {
    gint sls = get_vgmstream_play_samples(loop_count,fade_seconds,fade_delay_seconds,stream);
    gint ms = (sls * 1000LL) / stream->sample_rate;
    gint rate   = stream->sample_rate * 2 * stream->channels;
    
    sprintf(msg,"%s\nSample rate: %d\nStereo: %s\nTotal samples: %d\nBits per second: %d\nLength: %f seconds",strTitle,stream->sample_rate,(stream->channels >= 2) ? "yes" : "no",sls,rate,(double)ms / 1000.0);
    
    close_vgmstream(stream);

    audacious_info_dialog("File information",msg,"OK",FALSE,NULL,NULL);
  }
}