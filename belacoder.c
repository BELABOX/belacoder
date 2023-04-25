/*
    belacoder - live video encoder with dynamic bitrate control
    Copyright (C) 2020 BELABOX project

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <assert.h>
#include <signal.h>
#include <sys/mman.h>

#include <gst/gst.h>
#include <gst/gstinfo.h>
#include <gst/app/gstappsink.h>
#include <glib-unix.h>

#include <srt.h>

#define SRT_MAX_OHEAD 0     // maximum SRT transmission overhead (when using appsink)
#define SRT_ACK_TIMEOUT 6000 // maximum interval between received ACKs before the connection is TOed

#define MIN_BITRATE (300 * 1000)
#define ABS_MAX_BITRATE (30 * 1000 * 1000)
#define DEF_BITRATE (6 * 1000 * 1000)

#define BITRATE_UPDATE_INT 20
#define BITRATE_INCR_MIN       (30*1000)  // the minimum bitrate increment step (bps)
#define BITRATE_INCR_INT       500        // the minimum interval for increasing the bitrate (ms)
#define BITRATE_INCR_SCALE     30         // the bitrate is increased by
                                          // BITRATE_INCR_MIN + cur_bitrate/BITRATE_INCR_SCALE

#define BITRATE_DECR_MIN       (100*1000) // the minimum value to decrease the bitrate by (bps)
#define BITRATE_DECR_INT       200        // (light congestion) min interval for decreasing the bitrate (ms)
#define BITRATE_DECR_FAST_INT  250        // (heavy congestion) min interval for decreasing the bitrate (ms)
#define BITRATE_DECR_SCALE     10         // under heavy congestion, the bitrate is decreased by
                                          // BITRATE_DECR_MIN + cur_bitrate/BITRATE_DECR_SCALE

// settings ranges
#define MAX_AV_DELAY 10000
#define MIN_SRT_LATENCY 100
#define MAX_SRT_LATENCY 10000
#define DEF_SRT_LATENCY 2000

#define min(a, b) ((a < b) ? a : b)
#define max(a, b) ((a > b) ? a : b)
#define min_max(a, l, h) (max(min((a), (h)), (l)))

//#define DEBUG 1
#ifdef DEBUG
  #define debug(...) fprintf (stderr, __VA_ARGS__)
#else
  #define debug(...)
#endif

static GstPipeline *gst_pipeline = NULL;
GMainLoop *loop;
GstElement *encoder, *overlay;
SRTSOCKET sock = -1;
int quit = 0;

int enc_bitrate_div = 1;

int av_delay = 0;

int min_bitrate = MIN_BITRATE;
int max_bitrate = DEF_BITRATE;
int cur_bitrate = MIN_BITRATE;

char *bitrate_filename = NULL;

int srt_latency = DEF_SRT_LATENCY;

uint64_t getms() {
  struct timespec time = {0, 0};
  assert(clock_gettime(CLOCK_MONOTONIC_RAW, &time) == 0);
  return time.tv_sec * 1000 + time.tv_nsec / 1000 / 1000;
}

/* Attempts to stop the gstreamer pipeline cleanly
   Also sets up an alarm in case it doesn't */
void stop() {
  if (!quit) {
    quit = 1;
    alarm(3);
    g_main_loop_quit(loop);
  }
}

/*
  This checks periodically for pipeline stalls. The alsasrc element tends to stall rather
  than error out when the input resolution changes for a live input into a Camlink 4K
  connected to a Jetson Nano. If you see this happening in other scenarios, please report it
*/
gboolean stall_check(gpointer data) {
  /* This will handle any signals delivered between setting up the handler and
     starting the loop. Couldn't find another way to avoid races / potentially
     losing signals */
  if (quit) {
    stop();
    return TRUE;
  }

  static gint64 prev_pos = -1;
  gint64 pos;
  if (!gst_element_query_position((GstElement *)gst_pipeline, GST_FORMAT_TIME, &pos))
    return TRUE;

  if (pos != -1 && pos == prev_pos) {
    fprintf(stderr, "Pipeline stall detected. Will exit now\n");
    stop();
  }

  prev_pos = pos;
  return TRUE;
}

void update_overlay(int set_bitrate, double throughput,
                    int rtt, int rtt_th_min, int rtt_th_max,
                    int bs, int bs_th1, int bs_th2, int bs_th3) {
  if (GST_IS_ELEMENT(overlay)) {
    char overlay_text[100];
    snprintf(overlay_text, 100, "  b: %5d/%5.0f rtt: %3d/%3d/%3d bs: %3d/%3d/%3d/%3d",
             set_bitrate/1000, throughput,
             rtt, rtt_th_min, rtt_th_max,
             bs, bs_th1, bs_th2, bs_th3);
    g_object_set (G_OBJECT(overlay), "text", overlay_text, NULL);
  }
}

int parse_bitrate(char *bitrate_string) {
  int bitrate = strtol(bitrate_string, NULL, 10);
  if (bitrate < MIN_BITRATE || bitrate > ABS_MAX_BITRATE) {
    return -1;
  }
  return bitrate;
}

int read_bitrate_file() {
  FILE *f = fopen(bitrate_filename, "r");
  if (f == NULL) return -1;

  char *buf = NULL;
  size_t buf_sz = 0;
  int br[2];

  for (int i = 0; i < 2; i++) {
    buf_sz = getline(&buf, &buf_sz, f);
    if (buf_sz < 0) goto ret_err;
    br[i] = parse_bitrate(buf);
    if (br[i] < 0) goto ret_err;
  }

  free(buf);
  min_bitrate = br[0];
  max_bitrate = br[1];
  return 0;

ret_err:
  if (buf) free(buf);
  return -2;
}

#define SRT_PKT_SIZE 1316
#define RTT_TO_BS(rtt) ((throughput / 8) * (rtt) / SRT_PKT_SIZE)
void update_bitrate(SRT_TRACEBSTATS *stats, uint64_t ctime) {
  /*
   * Send buffer size stats
   */
  int bs = -1;
  int sz = sizeof(bs);
  int ret = srt_getsockflag(sock, SRTO_SNDDATA, &bs, &sz);
  if (ret != 0 || bs < 0) return;

  // Rolling average
  static double bs_avg = 0;
  bs_avg = bs_avg*0.99 + (double)bs * 0.01;

  // Update the buffer size jitter
  static double bs_jitter = 0;
  static int prev_bs = 0;
  bs_jitter = 0.99 * bs_jitter;
  int delta_bs = bs - prev_bs;
  if (delta_bs > bs_jitter) {
    bs_jitter = (double)delta_bs;
  }
  prev_bs = bs;


  /*
   * RTT stats
   */
  int rtt = (int)stats->msRTT;

  // Update the average RTT
  static double rtt_avg = 0;
  if (rtt_avg == 0.0) {
    rtt_avg = (double)rtt;
  } else {
    rtt_avg = rtt_avg * 0.99 + 0.01 * (double)rtt;
  }

  // Update the average RTT delta
  static double rtt_avg_delta = 0;
  static int prev_rtt = 300;
  double delta_rtt = (double)(rtt - prev_rtt);
  rtt_avg_delta = rtt_avg_delta * 0.8 + delta_rtt * 0.2;
  prev_rtt = rtt;

  // Update the minimum RTT
  static double rtt_min = 200.0;
  rtt_min *= 1.001;
  if (rtt != 100 && rtt < rtt_min && rtt_avg_delta < 1.0) {
    rtt_min = rtt;
  }

  // Update the RTT jitter
  static double rtt_jitter = 0;
  rtt_jitter *= 0.99;
  if (delta_rtt > rtt_jitter) {
    rtt_jitter = delta_rtt;
  }


  /*
   * Rolling average of the network throughput
   */
  static double throughput = 0.0;
  throughput *= 0.97;
  throughput += ((double)stats->mbpsSendRate * 1000.0 * 1000.0 / 1024.0) * 0.03;


  debug("bs: %d bs_avg: %f, bs_jitter %f, bitrate %d rtt %d, delta rtt %.0f, avg delta %.1f, avg rtt %.1f, rtt_jitter, %.2f, rtt_min %.1f\n",
        bs, bs_avg, bs_jitter, cur_bitrate, rtt, delta_rtt, rtt_avg_delta, rtt_avg, rtt_jitter, rtt_min);


  static uint64_t next_bitrate_incr = 0;
  static uint64_t next_bitrate_decr = 0;

  int bitrate = cur_bitrate;
  int bs_th3 = (bs_avg + bs_jitter)*4;
  int bs_th2 = max(50, bs_avg + max(bs_jitter*3.0, bs_avg));
  bs_th2 = min(bs_th2, RTT_TO_BS(srt_latency/2));
  int bs_th1 = max(50, bs_avg + bs_jitter*2.5);
  int rtt_th_max = rtt_avg + max(rtt_jitter*4, rtt_avg*15/100);
  int rtt_th_min = rtt_min + max(1, rtt_jitter*2);


  if (bitrate > min_bitrate && (rtt >= (srt_latency / 3) || bs > bs_th3)) {
    bitrate = min_bitrate;
    next_bitrate_decr = ctime + BITRATE_DECR_INT;

  } else if (ctime > next_bitrate_decr &&
      (rtt > (srt_latency / 5) || bs > bs_th2)) {
    bitrate -= BITRATE_DECR_MIN + bitrate/BITRATE_DECR_SCALE;
    next_bitrate_decr = ctime + BITRATE_DECR_FAST_INT;

  } else if (ctime > next_bitrate_decr &&
             (rtt > rtt_th_max || bs > bs_th1)) {
    bitrate -= BITRATE_DECR_MIN;
    next_bitrate_decr = ctime + BITRATE_DECR_INT;

  } else if (ctime > next_bitrate_incr &&
             rtt < rtt_th_min && rtt_avg_delta < 0.01) {
    bitrate += BITRATE_INCR_MIN + bitrate / BITRATE_INCR_SCALE;
    next_bitrate_incr = ctime + BITRATE_INCR_INT;
  }

  bitrate = min_max(bitrate, min_bitrate, max_bitrate);

  // round the bitrate we set to 100 kbps
  int rounded_br = bitrate / (100*1000) * (100*1000);

  update_overlay(rounded_br, throughput, rtt, rtt_th_min, rtt_th_max, bs, bs_th1, bs_th2, bs_th3);

  if (bitrate != cur_bitrate) {
    cur_bitrate = bitrate;

    g_object_set (G_OBJECT(encoder), "bitrate", rounded_br / enc_bitrate_div, NULL);

    debug("set bitrate to %d, internal value %d\n", rounded_br, cur_bitrate);
  }
}

gboolean connection_housekeeping() {
  uint64_t ctime = getms();
  static uint64_t prev_ack_ts = 0;
  static uint64_t prev_ack_count = 0;

  // SRT stats
  SRT_TRACEBSTATS stats;
  int ret = srt_bstats(sock, &stats, 1);
  if (ret != 0) goto r;

  // Track when the most recent ACK was received
  if (stats.pktRecvACKTotal != prev_ack_count) {
    prev_ack_count = stats.pktRecvACKTotal;
    prev_ack_ts = ctime;
  }
  /* Manual check for connection timeout, because SRT is Pepega
     and will fail to timeout if RTT was high */
  if (prev_ack_count != 0 && (ctime - prev_ack_ts) > SRT_ACK_TIMEOUT) {
    fprintf(stderr, "The SRT connection timed out, exiting\n");
    stop();
  }

  // We can only update the bitrate when we have a configurable encoder
  if (GST_IS_ELEMENT(encoder)) {
    update_bitrate(&stats, ctime);
  }

r:
  return TRUE;
}

GstFlowReturn new_buf_cb(GstAppSink *sink, gpointer user_data) {
  static char pkt[SRT_PKT_SIZE];
  static int pkt_len = 0;

  GstSample *sample = gst_app_sink_pull_sample(sink);

  if (!sample) exit(EXIT_FAILURE);

  GstBuffer *buffer = NULL;
  GstMapInfo map = {0};

  buffer = gst_sample_get_buffer(sample);
  gst_buffer_map(buffer, &map, GST_MAP_READ);

  // We send SRT_PKT_SIZE size packets, splitting and merging samples if needed
  int sample_sz = map.size;
  do {
    int copy_sz = min(SRT_PKT_SIZE - pkt_len, sample_sz);
    memcpy((void *)pkt + pkt_len, map.data, copy_sz);
    pkt_len += copy_sz;

    if (pkt_len == SRT_PKT_SIZE) {
      int nb = srt_send(sock, pkt, SRT_PKT_SIZE);
      if (nb != SRT_PKT_SIZE) {
        fprintf(stderr, "The SRT connection failed, exiting\n");
        stop();
        goto ret;
      }
      pkt_len = 0;
    }

    sample_sz -= copy_sz;
  } while(sample_sz);

ret:
  gst_buffer_unmap(buffer, &map);
  gst_sample_unref (sample);

  return GST_FLOW_OK;
}

int parse_ip(struct sockaddr_in *addr, char *ip_str) {
  in_addr_t ip = inet_addr(ip_str);
  if (ip == -1) return -1;

  memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET; 
  addr->sin_addr.s_addr = ip;

  return 0;
}

int parse_ip_port(struct sockaddr_in *addr, char *ip_str, char *port_str) {
  if (parse_ip(addr, ip_str) != 0) return -1;

  int port = strtol(port_str, NULL, 10);
  if (port <= 0 || port > 65535) return -2;
  addr->sin_port = htons(port);

  return 0;
}

int connect_srt(char *host, char *port, char *stream_id) {
  struct addrinfo hints;
  struct addrinfo *addrs;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  int ret = getaddrinfo(host, port, &hints, &addrs);
  if (ret != 0) return -1;

  sock = srt_create_socket();
  if (sock == SRT_INVALID_SOCK) return -2;

#if SRT_MAX_OHEAD > 0
  // auto, based on input rate
  int64_t max_bw = 0;
  ret = srt_setsockflag(sock, SRTO_MAXBW, &max_bw, sizeof(max_bw));
  assert(ret == 0);

  // overhead(retransmissions)
  int32_t ohead = SRT_MAX_OHEAD;
  ret = srt_setsockflag(sock, SRTO_OHEADBW, &ohead, sizeof(ohead));
  assert(ret == 0);
#endif

  ret = srt_setsockflag(sock, SRTO_LATENCY, &srt_latency, sizeof(srt_latency));
  assert(ret == 0);

  if (stream_id != NULL) {
    ret = srt_setsockflag(sock, SRTO_STREAMID, stream_id, strlen(stream_id));
    assert(ret == 0);
  }

  int32_t algo = 1;
  ret = srt_setsockflag(sock, SRTO_RETRANSMITALGO, &algo, sizeof(algo));
  assert(ret == 0);

  int connected = -3;
  for (struct addrinfo *addr = addrs; addr != NULL; addr = addr->ai_next) {
    ret = srt_connect(sock, addr->ai_addr, addr->ai_addrlen);
    if (ret == 0) {
      connected = 0;

      int len = sizeof(srt_latency);
      ret = srt_getsockflag(sock, SRTO_PEERLATENCY, &srt_latency, &len);
      assert(ret == 0);
      fprintf(stderr, "SRT connected to %s:%s. Negotiated latency: %d ms\n",
              host, port, srt_latency);
      break;
    }
  }
  freeaddrinfo(addrs);

  return connected;
}

void exit_syntax() {
  fprintf(stderr, "Syntax: belacoder PIPELINE_FILE ADDR PORT [options]\n\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  -v                  Print the version and exit\n");
  fprintf(stderr, "  -d <delay>          Audio-video delay in milliseconds\n");
  fprintf(stderr, "  -s <streamid>       SRT stream ID\n");
  fprintf(stderr, "  -l <latency>        SRT latency in milliseconds\n");
  fprintf(stderr, "  -b <bitrate file>   Bitrate settings file, see below\n\n");
  fprintf(stderr, "Bitrate settings file syntax:\n");
  fprintf(stderr, "MIN BITRATE (bps)\n");
  fprintf(stderr, "MAX BITRATE (bps)\n---\n");
  fprintf(stderr, "example for 500 Kbps - 60000 Kbps:\n\n");
  fprintf(stderr, "    printf \"500000\\n6000000\" > bitrate_file\n\n");
  fprintf(stderr, "---\n");
  fprintf(stderr, "Send SIGHUP to reload the bitrate settings while running.\n");
  exit(EXIT_FAILURE);
}

static void cb_delay (GstElement *identity, GstBuffer *buffer, gpointer data) {
  buffer = gst_buffer_make_writable(buffer);
  GST_BUFFER_PTS (buffer) += GST_SECOND * abs(av_delay) / 1000;
}

static int get_sink_framerate(GstElement *element, gint *numerator, gint *denominator) {
  int ret = -1;

  GstPad *pad = gst_element_get_static_pad(element, "sink");
  if (!pad) {
    return -1;
  }

  GstCaps *caps = gst_pad_get_current_caps(pad);
  if (caps != NULL) {
    if (gst_caps_is_fixed(caps)) {
      const GstStructure *str = gst_caps_get_structure (caps, 0);
      if (gst_structure_get_fraction(str, "framerate", numerator, denominator)) {
        ret = 0;
      }
    }

    gst_caps_unref(caps);
  }

  gst_object_unref(pad);
  return ret;
}

unsigned long pts = 0;
static void cb_ptsfixup(GstElement *identity, GstBuffer *buffer, gpointer data) {
  static long period = 0;
  static long prev_pts = 0;
  long input_pts = GST_BUFFER_PTS(buffer);

  // get rid of the DTS, the following elements should use the PTS
  GST_BUFFER_DTS(buffer) = 0;

  // First frame, obtain the framerate and initial PTS
  if (pts == 0) {
    int fr_numerator = 0, fr_denominator = 0;
    if (get_sink_framerate(identity, &fr_numerator, &fr_denominator) == 0) {
      pts = input_pts;
      period = GST_SECOND * fr_denominator / fr_numerator;
      printf("%s: framerate: %d / %d, period is %ld\n",
             __FUNCTION__, fr_numerator, fr_denominator, period);
    }

  // Subsequent frames, adjust the PTS
  } else {
    #define AVG_MULT 1000
    #define AVG_WEIGHT 3 // AVG_WEIGHT out of AVG_MULT
    #define AVG_PREV (AVG_MULT-AVG_WEIGHT)
    #define AVG_ROUNDING (AVG_MULT/2)
    /* Rolling average to account for slight differences from the nominal framerate
       and even slight drifting over time due to temperature or voltage variation
       Have to add AVG_ROUNDING to avoid precision loss due to dividing by AVG_MULT
    */
    period = (period * AVG_PREV + AVG_ROUNDING) / AVG_MULT +
             ((input_pts - prev_pts) * AVG_WEIGHT + AVG_ROUNDING)/ AVG_MULT;

    /* As long as the input PTS is within 0 to 2.0 periods of the previous
       output PTS, assume that it was a continuous read at period ns from
       the previous frame and increment the PTS accordingly. Otherwise, handle
       the discontinuity by either dropping an input buffer or skipping an
       output period, as needed. */
    long diff = input_pts - pts;
    long incr = (diff/2 + period) / period * period;
    if (incr > 0) {
      pts += incr;
      debug("%s: in pts: %lu, out pts: %lu, incr %ld, diff %ld, period %ld\n",
             __FUNCTION__, GST_BUFFER_PTS(buffer), pts, incr, diff, period);
      GST_BUFFER_PTS(buffer) = pts;
    } else {
      debug("skipping frame: pts %lu, prev pts %lu, output pts: %lu, diff %ld\n",
             input_pts, prev_pts, pts, diff);
      GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DROPPABLE);
    }
  }

  prev_pts = input_pts;
}

void cb_pipeline (GstBus *bus, GstMessage *message, gpointer user_data) {
  switch(GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR:
      fprintf(stderr, "gstreamer error from %s\n", message->src->name);
      stop();
      break;
    case GST_MESSAGE_EOS:
      fprintf(stderr, "gstreamer eos from %s\n", message->src->name);
      stop();
      break;
    default:
      break;
  }
}

// Only called if the pipeline failed to stop
void cb_sigalarm(int signum) {
  _exit(EXIT_SUCCESS); // exiting deliberately following SIGINT or SIGTERM
}

#define FIXED_ARGS 3
int main(int argc, char** argv) {
  int opt;
  char *srt_host = NULL;
  char *srt_port = NULL;
  char *stream_id = NULL;
  srt_latency = DEF_SRT_LATENCY;

  while ((opt = getopt(argc, argv, "d:b:s:l:v")) != -1) {
    switch (opt) {
      case 'b':
        bitrate_filename = optarg;
        break;
      case 'd':
        av_delay = strtol(optarg, NULL, 10);
        if (av_delay < -MAX_AV_DELAY || av_delay > MAX_AV_DELAY) {
          fprintf(stderr, "Maximum sound delay +/- %d\n\n", MAX_AV_DELAY);
          exit_syntax();
        }
        break;
      case 's':
        stream_id = optarg;
        break;
      case 'l':
        srt_latency = strtol(optarg, NULL, 10);
        if (srt_latency < MIN_SRT_LATENCY || srt_latency > MAX_SRT_LATENCY) {
          fprintf(stderr, "The SRT latency must be between %d and %d ms\n\n",
                  MIN_SRT_LATENCY, MAX_SRT_LATENCY);
          exit_syntax();
        }
        break;
      case 'v':
        printf(VERSION "\n");
        exit(EXIT_SUCCESS);
      default:
        exit_syntax();
    }
  }

  if (argc - optind != FIXED_ARGS) {
    exit_syntax();
  }


  // Read the pipeline file
  int pipeline_fd = open(argv[optind], O_RDONLY);
  if (pipeline_fd < 0) {
    fprintf(stderr, "Failed to open the pipeline file %s: ", argv[optind]);
    perror("");
    exit(EXIT_FAILURE);
  }
  int len = lseek(pipeline_fd, 0, SEEK_END);
  if (len == 0) {
    fprintf(stderr, "The pipeline file is empty, exiting\n");
    exit(EXIT_FAILURE);
  }
  char *launch_string = mmap(0, len, PROT_READ, MAP_PRIVATE, pipeline_fd, 0);
  fprintf(stderr, "Gstreamer pipeline: %s\n", launch_string);

  gst_init (&argc, &argv);
  GError *error = NULL;
  gst_pipeline  = (GstPipeline*) gst_parse_launch(launch_string, &error);
  if (gst_pipeline == NULL) {
    g_print( "Failed to parse launch: %s\n", error->message);
    return -1;
  }
  if (error) g_error_free(error);
  GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(gst_pipeline));
  gst_bus_add_signal_watch(bus);
  g_signal_connect(bus, "message", (GCallback)cb_pipeline, gst_pipeline);


  // Optional dynamic video bitrate
  if (bitrate_filename) {
    int ret;
    if ((ret = read_bitrate_file()) != 0) {
      if (ret == -1) {
        fprintf(stderr, "Failed to read the bitrate settings file %s\n", bitrate_filename);
      } else {
        fprintf(stderr, "Failed to read valid bitrate settings from %s\n", bitrate_filename);
      }
      exit_syntax();
    }
  }
  cur_bitrate = max_bitrate;
  fprintf(stderr, "Max bitrate: %d\n", max_bitrate);
  signal(SIGHUP, (__sighandler_t)read_bitrate_file);

  encoder = gst_bin_get_by_name(GST_BIN(gst_pipeline), "venc_bps");
  if (!GST_IS_ELEMENT(encoder)) {
    encoder = gst_bin_get_by_name(GST_BIN(gst_pipeline), "venc_kbps");
    enc_bitrate_div = 1000;
  }
  if (GST_IS_ELEMENT(encoder)) {
    g_object_set (G_OBJECT(encoder), "bitrate", cur_bitrate / enc_bitrate_div, NULL);
  } else {
    fprintf(stderr, "Failed to get an encoder element from the pipeline, "
                    "no dynamic bitrate control\n");
    encoder = NULL;
  }


  // Optional bitrate overlay
  overlay = gst_bin_get_by_name(GST_BIN(gst_pipeline), "overlay");
  update_overlay(0,0,0,0,0,0,0,0,0);


  // Optional sound delay via an identity element
  fprintf(stderr, "A-V delay: %d ms\n", av_delay);
  GstElement *identity_elem = gst_bin_get_by_name(GST_BIN(gst_pipeline), av_delay >= 0 ? "a_delay" : "v_delay");
  if (GST_IS_ELEMENT(identity_elem)) {
    g_object_set(G_OBJECT(identity_elem), "signal-handoffs", TRUE, NULL);
    g_signal_connect(identity_elem, "handoff", G_CALLBACK(cb_delay), NULL);
  } else {
    fprintf(stderr, "Failed to get a delay element from the pipeline, not applying a delay\n");
  }


  // Optional video PTS interval fixup
  // To avoid OBS dropping frames due to PTS jitter
  identity_elem = gst_bin_get_by_name(GST_BIN(gst_pipeline), "ptsfixup");
  if (GST_IS_ELEMENT(identity_elem)) {
    g_object_set(G_OBJECT(identity_elem), "signal-handoffs", TRUE, NULL);
    g_signal_connect(identity_elem, "handoff", G_CALLBACK(cb_ptsfixup), NULL);
  } else {
    fprintf(stderr, "Failed to get a ptsfixup element from the pipeline, "
                    "not removing PTS jitter\n");
  }


  // Optional SRT streaming via an appsink (needed for dynamic video bitrate)
  GstAppSinkCallbacks callbacks = {NULL, NULL, new_buf_cb};
  GstElement *srt_app_sink = gst_bin_get_by_name(GST_BIN(gst_pipeline), "appsink");
  if (GST_IS_ELEMENT(srt_app_sink)) {
    gst_app_sink_set_callbacks (GST_APP_SINK(srt_app_sink), &callbacks, NULL, NULL);
    srt_host = argv[optind+1];
    srt_port = argv[optind+2];

    srt_startup();
  }

  if (GST_IS_ELEMENT(srt_app_sink)) {
    int ret_srt;
    do {
      ret_srt = connect_srt(srt_host, srt_port, stream_id);
      if (ret_srt != 0) {
        fprintf(stderr, "Failed to establish an SRT connection. Retrying...\n");
        usleep(500*1000);
      }
    } while(ret_srt != 0);
  }

  // We can only monitor the connection when we use an appsink
  if (GST_IS_ELEMENT(srt_app_sink)) {
    g_timeout_add(BITRATE_UPDATE_INT, connection_housekeeping, NULL);
  }

  /*
    We used to attempt to restart the pipeline in case of errors
    However the version of flvdemux distributed with Ubuntu 18.04
    for the Jetson Nano fails to restart.
    Rather than deal with glitchy pipeline elements, just give up
    and exit. Ensure you run belacoder in a wrapper script which
    can restart it if needed, e.g. belaUI
  */
  loop = g_main_loop_new (NULL, FALSE);
  signal(SIGTERM, stop);
  signal(SIGINT, stop);
  signal(SIGALRM, cb_sigalarm);
  g_timeout_add(1000, stall_check, NULL); // check every second

  // Everything good so far, start the gstreamer pipeline
  gst_element_set_state((GstElement*)gst_pipeline, GST_STATE_PLAYING);
  g_main_loop_run(loop);
  gst_element_set_state((GstElement*)gst_pipeline, GST_STATE_NULL);

  if (sock >= 0) {
    srt_close(sock);
  }

  return 0;
}
