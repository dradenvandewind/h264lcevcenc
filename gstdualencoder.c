#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideoencoder.h>
#include <inttypes.h>
#include <stdio.h>

#include <x264.h>
#include <xeve.h>
#include <xeve_app_util.h>


#define PACKAGE "gstdualencoder"
#define MAX_BS_BUF (16 * 1024 * 1024)
#define MAX_BITSTREAM_SIZE (10 * 1000 * 1000)

G_BEGIN_DECLS

#define GST_TYPE_DUAL_ENCODER (gst_dual_encoder_get_type())
#define GST_DUAL_ENCODER(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_DUAL_ENCODER, GstDualEncoder))
#define GST_DUAL_ENCODER_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_DUAL_ENCODER, GstDualEncoderClass))
#define GST_IS_DUAL_ENCODER(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_DUAL_ENCODER))
#define GST_IS_DUAL_ENCODER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_DUAL_ENCODER))

typedef struct _GstDualEncoder GstDualEncoder;
typedef struct _GstDualEncoderClass GstDualEncoderClass;

struct _GstDualEncoder {
    GstVideoEncoder parent;
    
    /* Properties */
    gint baseline_width;
    gint baseline_height;
    gint baseline_bitrate;
    gint enhancement_width;
    gint enhancement_height;
    gint enhancement_bitrate;
    
    /* x264 encoder context for baseline */
    x264_t *x264_encoder;
    x264_param_t x264_params;
    x264_picture_t x264_pic_in;
    x264_picture_t x264_pic_out;
    
    /* XEVE encoder context for LCEVC enhancement */
    XEVE *xeve_handle;  // CORRIGÉ: XEVE est un pointeur
    XEVE_CDSC *xeve_cdsc;
    XEVE_PARAM xeve_params;
    XEVE_IMGB *imgb_rec;
    unsigned char *bs_buf;
    XEVE_BITB bitb;
    
    /* Stream info */
    GstVideoInfo input_info;
    gboolean configured;
};

struct _GstDualEncoderClass {
    GstVideoEncoderClass parent_class;
};

/* Properties */
enum {
    PROP_0,
    PROP_BASELINE_WIDTH,
    PROP_BASELINE_HEIGHT,
    PROP_BASELINE_BITRATE,
    PROP_ENHANCEMENT_WIDTH,
    PROP_ENHANCEMENT_HEIGHT,
    PROP_ENHANCEMENT_BITRATE
};

/* Pad templates */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, "
                    "format=(string){I420,YV12}, "
                    "width=(int)[1,MAX], "
                    "height=(int)[1,MAX], "
                    "framerate=(fraction)[0,MAX]")
);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("application/x-dual-stream, "
                    "baseline=(string)video/x-h264, "
                    "enhancement=(string)video/x-lcevc")
);

GType gst_dual_encoder_get_type(void);

G_DEFINE_TYPE(GstDualEncoder, gst_dual_encoder, GST_TYPE_VIDEO_ENCODER);

/* Initialize x264 encoder for baseline stream */
static gboolean
gst_dual_encoder_init_x264(GstDualEncoder *encoder)
{
    /* Set x264 parameters for baseline profile */
    x264_param_default_preset(&encoder->x264_params, "veryfast", "baseline");
    
    encoder->x264_params.i_width = encoder->baseline_width;
    encoder->x264_params.i_height = encoder->baseline_height;
    encoder->x264_params.i_fps_num = 30;
    encoder->x264_params.i_fps_den = 1;
    encoder->x264_params.rc.i_bitrate = encoder->baseline_bitrate;
    encoder->x264_params.rc.i_rc_method = X264_RC_ABR;
    
    /* Force baseline profile */
    encoder->x264_params.i_level_idc = 30;
    encoder->x264_params.i_keyint_max = 30;
    encoder->x264_params.b_annexb = 1;
    
    /* Open encoder */
    encoder->x264_encoder = x264_encoder_open(&encoder->x264_params);
    if (!encoder->x264_encoder) {
        GST_ERROR_OBJECT(encoder, "Failed to open x264 encoder");
        return FALSE;
    }
    
    /* Allocate picture */
    if (x264_picture_alloc(&encoder->x264_pic_in, X264_CSP_I420, 
                          encoder->baseline_width, encoder->baseline_height) < 0) {
        GST_ERROR_OBJECT(encoder, "Failed to allocate x264 picture");
        return FALSE;
    }
    
    return TRUE;
}

/* Initialize XEVE encoder for LCEVC enhancement */
static gboolean
gst_dual_encoder_init_xeve(GstDualEncoder *encoder)
{
    int ret, err;

    encoder->xeve_cdsc = g_malloc(sizeof(XEVE_CDSC)); // Allocate but don't zero
    memset(encoder->xeve_cdsc, 0, sizeof(XEVE_CDSC)); // Now safe to memset

    if (encoder->xeve_cdsc == NULL)
    {
    printf("Failed to allocate XEVE_CDSC structure, Pointer is NULL");
    return;
  } else {
    encoder->xeve_cdsc->max_bs_buf_size = MAX_BS_BUF;
  }
  encoder->bs_buf = (unsigned char *)malloc(MAX_BS_BUF);
  if (!encoder->bs_buf) {
    printf( "Failed to allocate bitstream buffer");
    return;
  }
  // Initialize bitstream buffer
  if (encoder->bs_buf) {
    encoder->bitb.bsize = MAX_BS_BUF;
    encoder->bitb.addr = encoder->bs_buf;
  }

  if (encoder->xeve_cdsc) {
    /* get default parameters */
    xeve_param_default(&encoder->xeve_cdsc->param);
  }

  ret = xeve_param_ppt(&encoder->xeve_cdsc->param, XEVE_PROFILE_BASELINE,
                       XEVE_PRESET_FAST, XEVE_TUNE_ZEROLATENCY);

    if (XEVE_FAILED(ret)) {
    printf("cannot set profile, preset, tune to parameter: %d",
                     ret);

    ret = -1;
    // goto ERR;
  }



    
    /* Set XEVE parameters */
    xeve_param_default(&encoder->xeve_params);
    
    encoder->xeve_params.w = encoder->enhancement_width;
    encoder->xeve_params.h = encoder->enhancement_height;
    encoder->xeve_params.bitrate = encoder->enhancement_bitrate;
    
    /* Create encoder */
    encoder->xeve_handle= xeve_create(encoder->xeve_cdsc, &err);
    if (encoder->xeve_handle == NULL) {
        GST_ERROR_OBJECT(encoder, "Failed to create XEVE encoder: %d", err);
        return FALSE;
    }

    if (!encoder->xeve_handle || err != XEVE_OK) {
    printf( "Failed to initialize XEVE encoder (err=%d)", err);
    return FALSE;
  }
  if (ret = xeve_param_check(&encoder->xeve_cdsc->param)) {

    printf( "invalid configuration: %d", ret);
    ret = -1;
    // goto ERR;
  }
    
    /* Allocate input image buffer */
    
    encoder->imgb_rec = imgb_alloc(
      encoder->enhancement_width, 
      encoder->enhancement_height, XEVE_CS_SET(XEVE_CF_YCBCR420, 10, 0));
    if(!encoder->imgb_rec) {
        imgb_free(encoder->imgb_rec);
    encoder->imgb_rec = NULL;
        
    }

  



    
    return TRUE;
}

/* Cleanup function */
static void
gst_dual_encoder_cleanup(GstDualEncoder *encoder)
{
    if (encoder->x264_encoder) {
        x264_picture_clean(&encoder->x264_pic_in);
        x264_encoder_close(encoder->x264_encoder);
        encoder->x264_encoder = NULL;
    }
    
    if (encoder->xeve_handle) {
        xeve_delete(encoder->xeve_handle);
        encoder->xeve_handle = NULL;
    }
    
    if (encoder->imgb_rec) {
        imgb_free(encoder->imgb_rec);
        encoder->imgb_rec = NULL;
    }
    
    encoder->configured = FALSE;
}

/* Set caps and configure encoders */
static gboolean
gst_dual_encoder_set_format(GstVideoEncoder *vencoder, GstVideoCodecState *state)
{
    GstDualEncoder *encoder = GST_DUAL_ENCODER(vencoder);
    GstCaps *outcaps;
    GstVideoCodecState *output_state;
    
    /* Cleanup existing encoders */
    gst_dual_encoder_cleanup(encoder);
    
    encoder->input_info = state->info;
    
    /* Initialize both encoders */
    if (!gst_dual_encoder_init_x264(encoder)) {
        return FALSE;
    }
    
    if (!gst_dual_encoder_init_xeve(encoder)) {
        gst_dual_encoder_cleanup(encoder);
        return FALSE;
    }
    
    /* Set output caps */
    outcaps = gst_caps_new_simple("application/x-dual-stream",
                                 "baseline", G_TYPE_STRING, "video/x-h264",
                                 "enhancement", G_TYPE_STRING, "video/x-lcevc",
                                 NULL);
    
    output_state = gst_video_encoder_set_output_state(vencoder, outcaps, state);
    gst_video_codec_state_unref(output_state);
    
    encoder->configured = TRUE;
    
    return TRUE;
}

/* Scale frame to target resolution */
static GstBuffer *
scale_frame(GstVideoInfo *src_info, GstBuffer *src_buffer, 
            gint target_width, gint target_height)
{
    /* TODO: Implement proper scaling with GstVideoScaler */
    return gst_buffer_ref(src_buffer);
}

/* Copy YUV data to x264 picture */
static void
copy_yuv_to_x264(GstVideoInfo *info, GstMapInfo *map_info, 
                 x264_picture_t *pic, gint width, gint height)
{
    gint i;
    gsize y_size = width * height;
    gsize uv_size = y_size / 4;
    
    guint8 *data = map_info->data;
    
    /* Copy Y plane */
    memcpy(pic->img.plane[0], data, y_size);
    
    /* Copy U and V planes */
    if (GST_VIDEO_INFO_FORMAT(info) == GST_VIDEO_FORMAT_I420) {
        memcpy(pic->img.plane[1], data + y_size, uv_size);
        memcpy(pic->img.plane[2], data + y_size + uv_size, uv_size);
    } else if (GST_VIDEO_INFO_FORMAT(info) == GST_VIDEO_FORMAT_YV12) {
        memcpy(pic->img.plane[2], data + y_size, uv_size); /* V first in YV12 */
        memcpy(pic->img.plane[1], data + y_size + uv_size, uv_size); /* then U */
    }
}

/* Encode frame with both encoders */
static GstFlowReturn
gst_dual_encoder_handle_frame(GstVideoEncoder *vencoder, GstVideoCodecFrame *frame)
{
    GstDualEncoder *encoder = GST_DUAL_ENCODER(vencoder);
    GstBuffer *baseline_buffer = NULL;
    GstBuffer *enhancement_buffer = NULL;
    GstBuffer *scaled_baseline, *scaled_enhancement;
    GstMapInfo map_info;
    x264_nal_t *nal;
    int i_nal;
    int frame_size;
    XEVE_BITB bitb;
    XEVE_STAT stat;
    int ret;
    GstFlowReturn flow_ret = GST_FLOW_ERROR;
    
    if (!encoder->configured) {
        return GST_FLOW_NOT_NEGOTIATED;
    }
    
    /* Scale input frame for both resolutions */
    scaled_baseline = scale_frame(&encoder->input_info, frame->input_buffer,
                                 encoder->baseline_width, encoder->baseline_height);
    
    scaled_enhancement = scale_frame(&encoder->input_info, frame->input_buffer,
                                    encoder->enhancement_width, encoder->enhancement_height);
    
    /* Encode baseline with x264 */
    if (gst_buffer_map(scaled_baseline, &map_info, GST_MAP_READ)) {
        copy_yuv_to_x264(&encoder->input_info, &map_info, &encoder->x264_pic_in,
                        encoder->baseline_width, encoder->baseline_height);
        gst_buffer_unmap(scaled_baseline, &map_info);
        
        frame_size = x264_encoder_encode(encoder->x264_encoder, &nal, &i_nal,
                                       &encoder->x264_pic_in, &encoder->x264_pic_out);
        
        if (frame_size > 0 && i_nal > 0) {
            /* Create buffer with all NAL units */
            gsize total_size = 0;
            int i;
            
            for (i = 0; i < i_nal; i++) {
                total_size += nal[i].i_payload;
            }
            
            baseline_buffer = gst_buffer_new_allocate(NULL, total_size, NULL);
            GstMapInfo out_map;
            
            if (gst_buffer_map(baseline_buffer, &out_map, GST_MAP_WRITE)) {
                gsize offset = 0;
                for (i = 0; i < i_nal; i++) {
                    memcpy(out_map.data + offset, nal[i].p_payload, nal[i].i_payload);
                    offset += nal[i].i_payload;
                }
                gst_buffer_unmap(baseline_buffer, &out_map);
            }
        }
    }
    
    /* TODO: Implement XEVE encoding properly */
    /* For now, just create a dummy enhancement buffer */
    enhancement_buffer = gst_buffer_new_allocate(NULL, 1024, NULL);
    
    /* Combine both streams - simple approach: create a buffer with both */
    if (baseline_buffer && enhancement_buffer) {
        GstBuffer *output_buffer = gst_buffer_new();
        gst_buffer_append(output_buffer, baseline_buffer);
        gst_buffer_append(output_buffer, enhancement_buffer);
        
        /* Set output buffer */
        frame->output_buffer = output_buffer;
        flow_ret = gst_video_encoder_finish_frame(vencoder, frame);
    } else {
        if (baseline_buffer) gst_buffer_unref(baseline_buffer);
        if (enhancement_buffer) gst_buffer_unref(enhancement_buffer);
        flow_ret = GST_FLOW_ERROR;
    }
    
    /* Clean up */
    gst_buffer_unref(scaled_baseline);
    gst_buffer_unref(scaled_enhancement);
    
    return flow_ret;
}

/* Property setters/getters */
static void
gst_dual_encoder_set_property(GObject *object, guint prop_id,
                              const GValue *value, GParamSpec *pspec)
{
    GstDualEncoder *encoder = GST_DUAL_ENCODER(object);
    
    switch (prop_id) {
        case PROP_BASELINE_WIDTH:
            encoder->baseline_width = g_value_get_int(value);
            break;
        case PROP_BASELINE_HEIGHT:
            encoder->baseline_height = g_value_get_int(value);
            break;
        case PROP_BASELINE_BITRATE:
            encoder->baseline_bitrate = g_value_get_int(value);
            break;
        case PROP_ENHANCEMENT_WIDTH:
            encoder->enhancement_width = g_value_get_int(value);
            break;
        case PROP_ENHANCEMENT_HEIGHT:
            encoder->enhancement_height = g_value_get_int(value);
            break;
        case PROP_ENHANCEMENT_BITRATE:
            encoder->enhancement_bitrate = g_value_get_int(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
gst_dual_encoder_get_property(GObject *object, guint prop_id,
                              GValue *value, GParamSpec *pspec)
{
    GstDualEncoder *encoder = GST_DUAL_ENCODER(object);
    
    switch (prop_id) {
        case PROP_BASELINE_WIDTH:
            g_value_set_int(value, encoder->baseline_width);
            break;
        case PROP_BASELINE_HEIGHT:
            g_value_set_int(value, encoder->baseline_height);
            break;
        case PROP_BASELINE_BITRATE:
            g_value_set_int(value, encoder->baseline_bitrate);
            break;
        case PROP_ENHANCEMENT_WIDTH:
            g_value_set_int(value, encoder->enhancement_width);
            break;
        case PROP_ENHANCEMENT_HEIGHT:
            g_value_set_int(value, encoder->enhancement_height);
            break;
        case PROP_ENHANCEMENT_BITRATE:
            g_value_set_int(value, encoder->enhancement_bitrate);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/* Class initialization */
static void
gst_dual_encoder_class_init(GstDualEncoderClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstVideoEncoderClass *encoder_class = GST_VIDEO_ENCODER_CLASS(klass);
    
    gobject_class->set_property = gst_dual_encoder_set_property;
    gobject_class->get_property = gst_dual_encoder_get_property;
    gobject_class->finalize = (GObjectFinalizeFunc)gst_dual_encoder_cleanup;
    
    /* Install properties */
    g_object_class_install_property(gobject_class, PROP_BASELINE_WIDTH,
        g_param_spec_int("baseline-width", "Baseline Width",
                         "Width for H.264 baseline stream", 1, G_MAXINT, 640,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    
    g_object_class_install_property(gobject_class, PROP_BASELINE_HEIGHT,
        g_param_spec_int("baseline-height", "Baseline Height",
                         "Height for H.264 baseline stream", 1, G_MAXINT, 480,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    
    g_object_class_install_property(gobject_class, PROP_BASELINE_BITRATE,
        g_param_spec_int("baseline-bitrate", "Baseline Bitrate",
                         "Bitrate for H.264 baseline stream (kbps)", 1, G_MAXINT, 500,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    
    g_object_class_install_property(gobject_class, PROP_ENHANCEMENT_WIDTH,
        g_param_spec_int("enhancement-width", "Enhancement Width",
                         "Width for LCEVC enhancement stream", 1, G_MAXINT, 1920,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    
    g_object_class_install_property(gobject_class, PROP_ENHANCEMENT_HEIGHT,
        g_param_spec_int("enhancement-height", "Enhancement Height",
                         "Height for LCEVC enhancement stream", 1, G_MAXINT, 1080,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    
    g_object_class_install_property(gobject_class, PROP_ENHANCEMENT_BITRATE,
        g_param_spec_int("enhancement-bitrate", "Enhancement Bitrate",
                         "Bitrate for LCEVC enhancement stream (kbps)", 1, G_MAXINT, 200,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    
    /* Set element details */
    gst_element_class_set_static_metadata(element_class,
        "Dual H.264/LCEVC Encoder",
        "Codec/Encoder/Video",
        "Encode video with both H.264 baseline and LCEVC enhancement layers",
        "Le Blond Erwan <erwanleblond@gmail.com>");
    
    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);
    
    encoder_class->set_format = GST_DEBUG_FUNCPTR(gst_dual_encoder_set_format);
    encoder_class->handle_frame = GST_DEBUG_FUNCPTR(gst_dual_encoder_handle_frame);
}

/* Instance initialization */
static void
gst_dual_encoder_init(GstDualEncoder *encoder)
{
    /* Set default properties */
    encoder->baseline_width = 640;
    encoder->baseline_height = 480;
    encoder->baseline_bitrate = 500;
    encoder->enhancement_width = 1920;
    encoder->enhancement_height = 1080;
    encoder->enhancement_bitrate = 200;
    encoder->configured = FALSE;
    encoder->x264_encoder = NULL;
    encoder->xeve_handle = NULL;
    encoder->imgb_rec = NULL;
}

/* Plugin initialization */
static gboolean
plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "dualencoder", GST_RANK_NONE,
                                GST_TYPE_DUAL_ENCODER);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    dualencoder,
    "Dual H.264/LCEVC encoder plugin",
    plugin_init,
    "1.0",
    "LGPL",
    "GStreamer Dual Encoder",
    "https://example.com/"
)

G_END_DECLS