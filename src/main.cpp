/*
 * Copyright 2020 Xilinx Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <string>
#include <array>
#include <vector>
#include <sstream>
#include <memory>
#include <stdexcept>

#define DEFAULT_RTSP_PORT "5000"


static char *port = (char *) DEFAULT_RTSP_PORT;

static gchar* confdir = (gchar*)"/opt/xilinx/share/ivas";
static gchar* filename = (gchar*)"";
static gchar* infileType = (gchar*)"h264";
static gchar* outMediaType = (gchar*)"h264";
static gchar* target = (gchar*)"dp";
static gint   fr = 30;
static gint mipi = -1;
static gint usb = -1;
static gint w = 1920;
static gint h = 1080;
static gboolean nodet = FALSE;
static gboolean reportFps = FALSE;
static gboolean roiOff = FALSE;
static GOptionEntry entries[] =
{
    { "mipi", 'm', 0, G_OPTION_ARG_INT, &mipi, "mipi media id, e.g. 1 for /dev/media1", "media_ID"},
    { "usb", 'u', 0, G_OPTION_ARG_INT, &usb, "usb camera video device id, e.g. 2 for /dev/video2", "video_ID"},
    { "file", 'f', 0, G_OPTION_ARG_STRING, &filename, "location of h26x file as input", "file path"},
    { "infile-type", 'i', 0, G_OPTION_ARG_STRING, &infileType, "input file type: [h264 | h265]", "h264"},
    { "w", 'w', 0, G_OPTION_ARG_INT, &w, "resolution w of the input", "1920"},
    { "h", 'h', 0, G_OPTION_ARG_INT, &h, "resolution h of the input", "1080"},
    { "framerate", 'r', 0, G_OPTION_ARG_INT, &fr, "framerate of the input", "30"},

    { "target", 't', 0, G_OPTION_ARG_STRING, &target, "[dp|rtsp|file]", "dp"},
    { "outmedia-type", 'o', 0, G_OPTION_ARG_STRING, &outMediaType, "output file type: [h264 | h265]", "h264"},
    { "port", 'p', 0, G_OPTION_ARG_STRING, &port,
        "Port to listen on (default: " DEFAULT_RTSP_PORT ")", "5000"},

    { "nodet", 'n', 0, G_OPTION_ARG_NONE, &nodet, "no AI inference", NULL },
    { "report", 'R', 0, G_OPTION_ARG_NONE, &reportFps, "report fps", NULL },
    { "ROI-off", 0, 0, G_OPTION_ARG_NONE, &roiOff, "trun off ROI", NULL },
    { NULL }
};

static gboolean
my_bus_callback (GstBus * bus, GstMessage * message, gpointer data)
{
  GMainLoop *loop = (GMainLoop *) data;
  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_INFO:{
      GError *err;
      gchar *debug;
      gst_message_parse_info (message, &err, &debug);
      g_print ("Info: %s\n", debug);
      break;
    }
    case GST_MESSAGE_EOS:
      g_print ("End of stream\n");
      g_main_loop_quit (loop);
      break;
    case GST_MESSAGE_ERROR:{
      GError *err;
      gchar *debug;
      gst_message_parse_error (message, &err, &debug);
      g_print ("Error: %s\n", debug);
      break;
    }
    default:
      /* unhandled message */
      break;
  }

  return TRUE;
}





static std::string exec(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

static std::vector<std::string> GetIp()
{
    std::string s = exec("ifconfig | grep 'inet addr' | sed 's/.*inet addr:\\([^ ]*\\).*/\\1/'");

    std::vector<std::string> rarray;
    std::size_t pos;
    while ((pos = s.find("\n")) != std::string::npos) {
        std::string token = s.substr(0, pos);
        if (token != "127.0.0.1") {
            rarray.push_back(token);
        }
        s.erase(0, pos + std::string("\n").length());
    }

    return rarray;
}


int
main (int argc, char *argv[])
{
    GMainLoop *loop;
    GstRTSPServer *server;
    GstRTSPMountPoints *mounts;
    GstRTSPMediaFactory *factory;
    GstRTSPSessionPool *session;
    GOptionContext *optctx;
    GError *error = NULL;
    guint busWatchId;

    session = gst_rtsp_session_pool_new();
    gst_rtsp_session_pool_set_max_sessions  (session, 255);


    optctx = g_option_context_new ("- Application for facedetion detction on SoM board of Xilinx.");
    g_option_context_add_main_entries (optctx, entries, NULL);
    g_option_context_add_group (optctx, gst_init_get_option_group ());
    if (!g_option_context_parse (optctx, &argc, &argv, &error)) {
        g_printerr ("Error parsing options: %s\n", error->message);
        g_option_context_free (optctx);
        g_clear_error (&error);
        return -1;
    }
    g_option_context_free (optctx);

    loop = g_main_loop_new (NULL, FALSE);

    char pip[2500];
    pip[0] = '\0';

    char *perf = (char*)"";
    if (reportFps)
    {
        perf = (char*)"! perf ";
    }

    if (std::string(target) == "rtsp")
    {
        sprintf(pip + strlen(pip), "( ");
    }
    {
        if (std::string(filename) != "") {
            sprintf(pip + strlen(pip), 
                    "filesrc location=%s ! %sparse ! queue ! omx%sdec ! video/x-raw, width=%d, height=%d, format=NV12, framerate=%d/1 ", filename, infileType, infileType, w, h, fr);
        } else if (mipi > -1) {
            sprintf(pip + strlen(pip), 
                    "mediasrcbin name=videosrc media-device=/dev/media%d %s !  video/x-raw, width=%d, height=%d, format=NV12, framerate=30/1 ", mipi, (w==1920 && h==1080 && std::string(target) == "dp" ? " v4l2src0::io-mode=dmabuf v4l2src0::stride-align=256" : ""), w, h );
        } else if (usb > -1) {
            sprintf(pip + strlen(pip), 
                    "v4l2src name=videosrc device=/dev/video%d io-mode=mmap %s !  video/x-raw, width=%d, height=%d ! videoconvert \
                    ! video/x-raw, format=NV12",
                    usb, (w==1920 && h==1080 && std::string(target) == "dp" ? "stride-align=256" : ""), w, h );
        }

        if (!nodet) {
            sprintf(pip + strlen(pip), " ! tee name=t \
                    ! queue ! ivas_xm2m kconfig=\"%s/kernel_xpp_pipeline.json\" \
                    ! video/x-raw, width=640, height=360, format=BGR \
                    ! queue ! ivas_xfilter kernels-config=\"%s/kernel_densebox_640_360.json\" \
                    ! ima.sink_master \
                    ivas_xmetaaffixer name=ima ima.src_master ! fakesink \
                    t. \
                    ! queue ! ima.sink_slave_0 ima.src_slave_0 ! queue ! ivas_xfilter kernels-config=\"%s/kernel_boundingbox_facedetect.json\" ", confdir, confdir, confdir);
        }
    }

    if (std::string(target) == "rtsp")
    {
        /* create a server instance */
        server = gst_rtsp_server_new ();
        g_object_set (server, "service", port, NULL);
        mounts = gst_rtsp_server_get_mount_points (server);
        factory = gst_rtsp_media_factory_new ();

        sprintf(pip + strlen(pip), " \
                %s \
                ! queue ! omx%senc qp-mode=%s  num-slices=8 gop-length=60 periodicity-idr=270 \
                control-rate=low-latency                      gop-mode=low-delay-p gdr-mode=horizontal cpb-size=200 \
                initial-delay=100  filler-data=false min-qp=15  max-qp=40  b-frames=0  low-bandwidth=false \
                ! video/x-%s, alignment=au \
                ! queue %s ! rtp%spay name=pay0 pt=96 )",
                roiOff ? "" : " ! queue ! ivas_xroigen roi-type=1 roi-qp-delta=-10 roi-max-num=10 ",
                outMediaType,
                roiOff ? "auto" : "1",
                outMediaType, perf, outMediaType);

        gst_rtsp_media_factory_set_launch (factory, pip);
        gst_rtsp_media_factory_set_shared (factory, TRUE);
        gst_rtsp_mount_points_add_factory (mounts, "/test", factory);

        g_object_unref (mounts);

        /* attach the server to the default maincontext */
        gst_rtsp_server_attach (server, NULL);

        /* start serving */
        std::vector<std::string> ips = GetIp();
        std::ostringstream addr("");
        for (auto&ip : ips)
        {
            addr << "rtsp://" << ip << ":" << port << "/test\n";
        }
        g_print ("stream ready at:\n %s", addr.str().c_str());
        g_main_loop_run (loop);
    }
    else
    {
        if (std::string(target) == "file")
        {
            sprintf(pip + strlen(pip), "\
                ! queue ! omx%senc qp-mode=auto num-slices=8 gop-length=60 periodicity-idr=270 \
                control-rate=low-latency                      gop-mode=low-delay-p gdr-mode=horizontal cpb-size=200 \
                initial-delay=100  filler-data=false min-qp=15  max-qp=40  b-frames=0  low-bandwidth=false %s \
                ! filesink location=./out.%s", outMediaType, perf, outMediaType);
        }
        else if (std::string(target) == "dp")
        {
            sprintf(pip + strlen(pip), "\
                    ! queue %s ! kmssink driver-name=xlnx plane-id=39 sync=false fullscreen-overlay=true", perf);
        }

        GstElement *pipeline = gst_parse_launch(pip, NULL);
        gst_element_set_state (pipeline, GST_STATE_PLAYING);
        /* Wait until error or EOS */
        GstBus *bus = gst_element_get_bus (pipeline);
        loop = g_main_loop_new (NULL, FALSE);
        busWatchId = gst_bus_add_watch (bus, my_bus_callback, loop);
        g_main_loop_run (loop);

        gst_object_unref (bus);
        gst_element_set_state (pipeline, GST_STATE_NULL);
        gst_object_unref (pipeline);
        g_source_remove (busWatchId);
        g_main_loop_unref (loop);
    }
    return 0;
}
