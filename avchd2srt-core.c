/*
 * AVCHD2SRT-core.c
 */

const char version[] = "0.7 (05 June 2011)";

/* FFMPEG-based program to extract the time/date/geo information from the h264 video stream
 * as generated by e.g. Sony HD camcorders.
 *
 * This program runs independently, but should be distributed together with a script called
 * AVCHD2SRT, while will allow for batch operation and also adds address information.
 *
 * See the file "00 README.html" for background and references
 *
 * Copyright (c) 2010, 2011 Henry Devettens henryd65@gmail.com
 *
 * Compilation instructions for linux:
 *
 * 1. get and compile the ffmpeg-0.6.1 package
 * 2. in the ffmpeg-0.6.1 main directory (assume /home/me/ffmpeg-0.6.1), compile this program: gcc -O3 -L"/home/me/ffmpeg-0.6.1"/libavdevice -L"/home/me/ffmpeg-0.6.1"/libavformat -L"/home/me/ffmpeg-0.6.1"/libavcodec -L"/home/me/ffmpeg-0.6.1"/libavutil -I"/home/me/ffmpeg-0.6.1" -o avchd2srt-core avchd2srt-core.c -lavutil -lavformat -lavcodec -lz -lavutil -lm  
 *    (if the ffmpeg libs have been installed as root, the -L and -I params are not needed)
 * 3. strip it
 *
 * On windows/mingw, I used
 *   ./configure --enable-memalign-hack --extra-cflags=-I/local/include --extra-ldflags=-L/local/lib
 * to configure, copied "avchd2srt.c" over ffmpeg.c and typed "make ffmpeg.exe" to create
 * the executable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

const char program_name[] = "avchd2srt-core";
const int program_birth_year = 2010;

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"

static const uint8_t avchd_mdpm[] =
  { 0x17,0xee,0x8c,0x60,0xf8,0x4d,0x11,0xd9,0x8c,0xd6,0x08,0x00,0x20,0x0c,0x9a,0x66,
    'M','D','P','M' };

char * weekday[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};

char * monthname[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
#define BCD_2_INT(c) ((c >> 4)*10+(c&0xf))

int srt=-1;			// srt counter
char srtT[256], srtTn[256];	// srt text: current and new
int srtTi=0, srtTni=0;		// index in srtT ans srtTn arrays for next addition
time_t secsince1970, srtTsec=0;	    // Tricky: if new sub is one sec before current, ignore.
struct tm t;			// to calculate srtTsec
time_t start_time, end_time;	    // to measure the duration per file
char SL;			// will be + for above and - for below sea level
long int srtTimer;		// start time (in milliseconds) for current srtT
int srtH, srtM, srtS, srtmS;	// Hour, Minute, Seconds, milli
int foundgeo;
int frm;			// frame counter
int fps_den, fps_num;		// for the frame rate
float fps;
char fileout[1024];		// output file name
FILE *filesrt;			    // filepointer to the output file

static void set_srt_hmsm() {
  srtH = (int) (srtTimer/3600000L); srtTimer -= 3600000L*(long int) srtH;
  srtM = (int) (srtTimer/60000L); srtTimer -= 60000L*(long int) srtM;
  srtS = (int) (srtTimer/1000L); srtmS = (int) (srtTimer-1000L*(long int)srtS);
}

static void print_one_srt_entry() {
  set_srt_hmsm();
  fprintf(filesrt, "%0d\n", srt+1);
  fprintf(filesrt, "%02d:%02d:%02d,%03d --> ", srtH, srtM, srtS, srtmS);
  srtTimer = (long int) (((long int)(frm)*1000L)/fps);
  set_srt_hmsm();
  fprintf(filesrt, "%02d:%02d:%02d,%03d\n", srtH, srtM, srtS, srtmS);
  fprintf(filesrt, "%s\n", srtT);
  srtTi = snprintf(srtT, 256, "");
}

static int set_output_file (char * filein) {
  int j;

  j = snprintf(fileout, 1024, "%s", filein);
  if (j>1020) { fprintf(stderr, "File name too long - exit.\n"); return -1;}
  while ((j>0) && (fileout[j]!='.')) {
    j--;
  }
  if (fileout[j]=='.') {
    snprintf(&(fileout[j+1]), 4, "srt");
  }
  if ((filesrt = fopen(fileout, "w")) == NULL) { 
     fprintf(stderr, "Cannot open \"%s\" for writing.\n", fileout);
     return -1;
  }
  return 0;
}

int main(int argc, char *argv[]) {

  AVFormatContext *pFormatCtx;
  int strm;
  int videoStream;
  AVCodecContext *pCodecCtx;
  AVCodec *pCodec;
  AVFrame *pFrame;
  int frameFinished=1;
  AVPacket packet;
  uint8_t * ptr;
  uint8_t bt0, bt1, bt2, bt3;
  int j;
  int tag, num_tags, i;
  int year, month, day, hour, minute, second, tz; 	// ..., timezone
  int latH, latD, latM, latS; 				// latitude hemi, deg, min, sec
  int lonE, lonD, lonM, lonS;               		// longitude "east", deg, min, sec
  int altS, altL, altD;                               	// altitude below/above sea, level, divisor
  int speed, speD, speU;				// speed, speed divisor, speed unit

  av_register_all();

  if (argc != 2) {
      printf("Usage: %s input_file\n\nOutput (srt format) sent to .srt file, Errors to stderr\n\n", 
             argv[0]);
      exit(1);
  }

  fprintf(stderr, "AVCHD2SRT version %s\n", version);

  /* allocate the media context */
  pFormatCtx = avformat_alloc_context();
  if (!pFormatCtx) {
      fprintf(stderr, "Memory error\n");
      return -1;
  }

  // Open video file
  if(av_open_input_file(&pFormatCtx, argv[1], NULL, 0, NULL)!=0) {
    fprintf(stderr, "Cannot open file %s.\n", argv[1]);
    return -1; // Couldn't open file
  }

  if (set_output_file(argv[1])) return -1;

  // Retrieve stream information
  if(av_find_stream_info(pFormatCtx)<0) {
    fprintf(stderr, "Could not find stream information\n");
    return -1; // Couldn't find stream information
  }

  // Dump information about file onto standard error:
  dump_format(pFormatCtx, 0, argv[1], 0);

  start_time = time (NULL);

  // Find the first video stream
  videoStream=-1;
  for(strm=0; strm<pFormatCtx->nb_streams; strm++)
    if(pFormatCtx->streams[strm]->codec->codec_type==CODEC_TYPE_VIDEO) {
      videoStream=strm;
//      fps_num=pFormatCtx->streams[strm]->r_frame_rate.num;
//      fps_den=pFormatCtx->streams[strm]->r_frame_rate.den;
//      fps = (float) fps_num / fps_den;
      fps_num=pFormatCtx->streams[strm]->codec->time_base.num;
      fps_den=pFormatCtx->streams[strm]->codec->time_base.den;
      fps = (float) fps_den / fps_num;
      fprintf(stderr, "  Frame rate: %2.2f (%d/%d)\n", fps, fps_num, fps_den);
      break;
    }

  fprintf(stderr, "  Output file name: \'%s\'\n", fileout);

  if(videoStream==-1) {
    fprintf(stderr, "Did not find a video stream\n");
    return -1; // Didn't find a video stream
  }

  // Get a pointer to the codec context for the video stream
  pCodecCtx=pFormatCtx->streams[videoStream]->codec;

  // Find the decoder for the video stream
  pCodec=avcodec_find_decoder(pCodecCtx->codec_id);
  if(pCodec==NULL) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1; // Codec not found
  }

  // Open codec
  if(avcodec_open(pCodecCtx, pCodec)<0) {
    fprintf(stderr, "Could not open codec\n");
    return -1; // Could not open codec
  }

  // Allocate video frame
  pFrame=avcodec_alloc_frame();

  frm=0;
  while(av_read_frame(pFormatCtx, &packet)>=0) {
    // Is this a packet from the video stream?
    if(packet.stream_index==videoStream) {
   	  // Decode video frame

//      fprintf(stderr, "Packet size before decode: %0d\n", packet.size);    
//      avcodec_decode_video(pCodecCtx, pFrame, &frameFinished,
//                           packet.data, packet.size);
//      fprintf(stderr, "Packet size after decode: %0d\n", packet.size);    
//      Can we do this quicker? All we need is a correct value of frameFinished.
        

// For Sony, the meta data appears in the key frame, so we looked for it
// only in those: if ((packet.flags & PKT_FLAG_KEY) != 0) {...
// But for Panasonic this did not work. So now we just always do it:

      if (1) { 

/*          if ((packet.flags & PKT_FLAG_KEY) != 0) 
             fprintf(stderr, "Found a key frame, frame number %0d (finished: %d)\n", frm+1, frameFinished);
          else
             fprintf(stderr, "Found a non-key frame, frame number %0d (finished: %d)\n", frm+1, frameFinished); 
          av_pkt_dump(stderr, &packet, 1);
        av_hex_dump(stderr, packet.data, 48);
*/

// start looking for the message - assuming it's within the first 256 bytes of the keyframe

        year = -1; month = -1; day = -1; hour = -1; minute = -1; second = -1; tz = -1;
        latH = -1; latD = -1; latM = -1; latS = -1; 
        lonE = -1; lonD = -1; lonM = -1; lonS = -1;
        altS = -1; altL = -1; altD = -1;
        speed = -1; speD = -1; speU = -1;

        ptr = packet.data;
        j=0;
        while ((j<256) && (memcmp(ptr+j, avchd_mdpm, 20))) j++;
        if (j<256) {
//          fprintf(stderr, "Found the message at bytes %0d\n", j);      	// comment out
//          av_hex_dump(stderr, ptr+j, 160);					// comment out

          /* Skip GUID + MDPM */
          ptr += j+20;

          num_tags = *ptr; ptr++;
          
          for(i = 0; i < num_tags; i++)
            {
            tag = *ptr; ptr++;

            bt0 = ptr[0]; bt1 = ptr[1]; bt2 = ptr[2]; bt3 = ptr[3];
            ptr += 4;

            // correct for the 0x000003's representing 0x0000...:

            if ((bt0==0x00) & (bt1==0x00) & (bt2==0x03)) {
               bt2 = bt3; bt3 = ptr[0]; ptr++;
            } else
            if ((bt1==0x00) & (bt2==0x00) & (bt3==0x03)) {
               bt3 = ptr[0]; ptr++;
            }


//            fprintf(stderr, "Tag: 0x%02x, Data: %02x %02x %02x %02x\n",	// comment out
//                    tag, bt0, bt1, bt2, bt3);					// comment out

// For Panasonic, some tags appear twice in the message, screwing it up.
// Am probably not reading the message properly. Never happened for Sony.
// So now we only take the first, hence the additional checks on ==-1 below.
// It may also be needed for Geo info - need to have that tested.

            switch(tag)
              {
              case 0x18:
                if (year==-1) { tz=bt0;
                                year  = BCD_2_INT(bt1)*100 + BCD_2_INT(bt2);
                                month = BCD_2_INT(bt3);}
                break;
              case 0x19:
                if (day==-1) { day    = BCD_2_INT(bt0); hour   = BCD_2_INT(bt1);
                               minute = BCD_2_INT(bt2); second = BCD_2_INT(bt3);}
                break;
	      case 0xb1:
		latH   = bt0;
		break;
	      case 0xb2:
		latD   = bt0*256 + bt1;
                break;
              case 0xb3:
                latM   = bt0*256 + bt1;
                break;
              case 0xb4:
                latS   = bt0*256 + bt1;
                break;
	      case 0xb5:
		lonE   = bt0;
		break;
	      case 0xb6:
		lonD   = bt0*256 + bt1;
                break;
              case 0xb7:
                lonM   = bt0*256 + bt1;
                break;
              case 0xb8:
                lonS   = bt0*256 + bt1;
                break;
              case 0xb9:
                altS   = bt0;
                break;
              case 0xba:
                altL   = bt0*256 + bt1;
                altD   = bt3;
                break;
              case 0xc1:
                speU   = bt0;
                break;
              case 0xc2:
                speed  = bt0*256 + bt1;
                speD   = bt3;
                break;
              }
            }
          
          srtTni=0;

          if((year >= 0) && (month >= 0) && (day >= 0) &&
             (hour >= 0) && (minute >= 0) && (second >= 0))
            {
              t.tm_year = year-1900;  t.tm_mon = month-1; t.tm_mday = day;
              t.tm_hour = hour;       t.tm_min = minute;  t.tm_sec = second;
              t.tm_isdst = 0;
              secsince1970 = mktime(&t);
              srtTni += snprintf(&(srtTn[srtTni]), 40, "%s %02d-%s-%04d %02d:%02d:%02d", 
                                 weekday[t.tm_wday],
                                 day, monthname[month-1], year, hour, minute, second);
              if (tz<64) { // valid time zone value
                if (tz<32) SL='+'; else SL='-';
                tz=tz%32;
                srtTni += snprintf(&(srtTn[srtTni]), 30, " (%c%02d:%02d)",
                                   SL, tz/2, 30*(tz%2));
              }
              srtTni += snprintf(&(srtTn[srtTni]), 3, "\n");
            }
          foundgeo=0;
          if((latH >= 0) && (latD >= 0) && (latM >= 0) && (latS >= 0) && 
             (lonE >= 0) && (lonD >= 0) && (lonM >= 0) && (lonS >= 0))
            {
            foundgeo=1;
            srtTni += snprintf(&(srtTn[srtTni]), 60, "GPS: %0d %0d %0.2f %c %0d %0d %0.2f %c",
                               latD, latM, ((float)latS)/1000.0, latH,
                               lonD, lonM, ((float)lonS)/1000.0, lonE);
            }
          if ((altS >= 0) && (altL >= 0))
            {
            foundgeo=1;
            if (altS==0) SL='+'; else SL='-';          // Above + or Below - Sea level
            if (altD==0) altD = 10;			// stub - just assume, not sure
            if (altD==1) 
               srtTni += snprintf(&(srtTn[srtTni]), 30, " %c%0d m", SL, altL);
            else
               srtTni += snprintf(&(srtTn[srtTni]), 30, " %c%0.1f m", SL, ((float)altL)/(float)altD);
            }
          if ((speed>=0) && (speD>0) && (speU>0))
            {
            foundgeo=1;
            if (speD==1) 
               srtTni += snprintf(&(srtTn[srtTni]), 30, " %0d", speed);
            else
               srtTni += snprintf(&(srtTn[srtTni]), 30, " %0.1f", ((float)speed)/(float)speD);
            if (speU=='K')
               srtTni += snprintf(&(srtTn[srtTni]), 30, " %s", "km/h");
            else if (speU=='M')
               srtTni += snprintf(&(srtTn[srtTni]), 30, " %s", "mph");
            else if (speU=='N')
               srtTni += snprintf(&(srtTn[srtTni]), 30, " %s", "knots");
            }
          if (foundgeo) {
            srtTni += snprintf(&(srtTn[srtTni]), 30, "\n"); 
          }

          if ( (strcmp (srtT,srtTn) != 0) && ((srtTsec-secsince1970) != 1) ) {
							// new subtitle not the same as current
 		// and the new subtitle is not one second older than current. In latter case we assume
		// B-frames and silenty ignore for now
            if (srtTi) print_one_srt_entry();		// so print & clear the current one if it exists
            if (srtTni) {				// and if there is a new one, make current
              srt++;
              srtTimer = (long int) (((long int)(frm)*1000L)/fps);
              srtTsec = secsince1970;
              srtTi = snprintf(srtT, 256, "%s", srtTn);
            }
          } // else if new subtitle is the same as previous, do nothing
        } else {
//          fprintf(stderr, "did not find the message\n");
        }
      }
      // Did we get a video frame?
      if(frameFinished) {
        frm++;
      }
    }
    
    // Free the packet that was allocated by av_read_frame
    av_free_packet(&packet);
  }

  if (srtTi>0) 
    print_one_srt_entry();

  fprintf(stderr, "  Read %d frames\n", frm);

  end_time = time (NULL);

  fprintf(stderr, "Processed in %0d seconds\n", (int)difftime(end_time, start_time));

  // Close the SRT output file
  fclose(filesrt);

  // Free the YUV frame
  av_free(pFrame);

  // Close the codec
  avcodec_close(pCodecCtx);

  // Close the video file
  av_close_input_file(pFormatCtx);

  return 0;

}
