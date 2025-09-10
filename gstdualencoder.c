#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideoencoder.h>

#include <gst/video/gstvideometa.h>
//include <gst/video/gstvideoconverter.h>

#include <inttypes.h>
#include <stdio.h>
#include <unistd.h> // for sysconf


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
    gint gop_size;
    gint fps_n;
    gint fps_d;
    
    /* x264 encoder context for baseline */
    x264_t *x264_encoder;
    x264_param_t x264_params;
    x264_picture_t x264_pic_in;
    x264_picture_t x264_pic_out;
    
    /* XEVE encoder context for LCEVC enhancement */
    XEVE *xeve_handle;  // CORRIGÉ: XEVE est un pointeur
    XEVE_CDSC *xeve_cdsc;
    //XEVE_PARAM xeve_params;
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
    PROP_ENHANCEMENT_BITRATE,
    PROP_GOP_SIZE
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
    GST_STATIC_CAPS("video/x-h264, "
                    "lcevc = (boolean) true, "
                    "stream-format = (string) byte-stream, "
                    "alignment = (string) au ")
);


GType gst_dual_encoder_get_type(void);

G_DEFINE_TYPE(GstDualEncoder, gst_dual_encoder, GST_TYPE_VIDEO_ENCODER);

/* Initialize x264 encoder for baseline stream */
static gboolean
gst_dual_encoder_init_x264(GstDualEncoder *encoder)
{
    /* Set x264 parameters for baseline profile */
    x264_param_default_preset(&encoder->x264_params, "veryfast", NULL);
    
    encoder->x264_params.i_width = encoder->baseline_width;
    encoder->x264_params.i_height = encoder->baseline_height;
    encoder->x264_params.i_fps_num = encoder->fps_n;    
    encoder->x264_params.i_fps_den = encoder->fps_d;
    encoder->x264_params.rc.i_bitrate = encoder->baseline_bitrate;
    encoder->x264_params.rc.i_rc_method = X264_RC_ABR;
    
    /* Force baseline profile */
    encoder->x264_params.i_level_idc = 30;
    encoder->x264_params.i_keyint_max = encoder->gop_size;
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

static int set_extra_config(XEVE handle, gint hash, gint info) {
  int ret, size;
  // todo to be configure from plugin param
  // embed SEI messages identifying encoder parameters

  // embed picture signature (HASH) for conformance checking in decoding"

  size = 4;
  ret = xeve_config(handle, XEVE_CFG_SET_SEI_CMD, &info, &size);
  if (XEVE_FAILED(ret)) {
    g_print("failed to set config for sei command info messages\n");
    return -1;
  }

  if (hash) {
    size = 4;
    ret = xeve_config(handle, XEVE_CFG_SET_USE_PIC_SIGNATURE, &hash, &size);
    if (XEVE_FAILED(ret)) {
      g_print("failed to set config for picture signature\n");

      return -1;
    }
  }

  return 0;
}
void print_xeve_param(const XEVE_PARAM *param) {
  if (param == NULL) {
    printf("Error: NULL parameter passed to print_xeve_param\n");
    return;
  }

  printf("XEVE_PARAM Structure Values:\n");
  printf("===========================\n");

  /* Basic parameters */
  printf("Profile: %d\n", param->profile);
  printf("Threads: %d\n", param->threads);
  printf("Width: %d\n", param->w);
  printf("Height: %d\n", param->h);
  printf("FPS: %d/%d\n", param->fps.num, param->fps.den);
  printf("Keyint: %d\n", param->keyint);
  printf("Color space: %d\n", param->cs);
  printf("RC type: %d\n", param->rc_type);
  printf("QP: %d\n", param->qp);
  printf("QP CB offset: %d\n", param->qp_cb_offset);
  printf("QP CR offset: %d\n", param->qp_cr_offset);
  printf("Bitrate: %d kbps\n", param->bitrate);
  printf("VBV bufsize: %d kbits\n", param->vbv_bufsize);
  printf("CRF: %d\n", param->crf);
  printf("B-frames: %d\n", param->bframes);
  printf("AQ mode: %d\n", param->aq_mode);
  printf("Lookahead: %d\n", param->lookahead);
  printf("Closed GOP: %d\n", param->closed_gop);
  printf("Use Annex-B: %d\n", param->use_annexb);
  printf("Use filler: %d\n", param->use_filler);

  /* Chroma QP table */
  printf("Chroma QP table present: %d\n", param->chroma_qp_table_present_flag);
  // Note: The arrays are too large to print completely - you might want to
  // print just a few elements

  /* Coding tools */
  printf("Disable HGOP: %d\n", param->disable_hgop);
  printf("Ref pic gap length: %d\n", param->ref_pic_gap_length);
  printf("Codec bit depth: %d\n", param->codec_bit_depth);
  printf("Level IDC: %d\n", param->level_idc);
  printf("Cutree: %d\n", param->cutree);
  printf("Constrained intra pred: %d\n", param->constrained_intra_pred);
  printf("Use deblock: %d\n", param->use_deblock);
  printf("Inter slice type: %d\n", param->inter_slice_type);

  /* Picture cropping */
  printf("Picture cropping flag: %d\n", param->picture_cropping_flag);
  if (param->picture_cropping_flag) {
    printf("  Crop left: %d\n", param->picture_crop_left_offset);
    printf("  Crop right: %d\n", param->picture_crop_right_offset);
    printf("  Crop top: %d\n", param->picture_crop_top_offset);
    printf("  Crop bottom: %d\n", param->picture_crop_bottom_offset);
  }

  /* More parameters */
  printf("RDO DBK switch: %d\n", param->rdo_dbk_switch);
  printf("QP increase frame: %d\n", param->qp_incread_frame);
  printf("SEI CMD info: %d\n", param->sei_cmd_info);
  printf("Use pic sign: %d\n", param->use_pic_sign);
  printf("F I-frame: %d\n", param->f_ifrm);
  printf("QP max: %d\n", param->qp_max);
  printf("QP min: %d\n", param->qp_min);
  printf("GOP size: %d\n", param->gop_size);
  printf("Force output: %d\n", param->force_output);
  printf("Use FCST: %d\n", param->use_fcst);
  printf("Chroma format IDC: %d\n", param->chroma_format_idc);
  printf("CS width shift: %d\n", param->cs_w_shift);
  printf("CS height shift: %d\n", param->cs_h_shift);

  /* CU settings */
  printf("Max CU intra: %d\n", param->max_cu_intra);
  printf("Min CU intra: %d\n", param->min_cu_intra);
  printf("Max CU inter: %d\n", param->max_cu_inter);
  printf("Min CU inter: %d\n", param->min_cu_inter);

  /* Motion estimation */
  printf("Ref frames: %d\n", param->ref);
  printf("ME ref num: %d\n", param->me_ref_num);
  printf("ME algorithm: %d\n", param->me_algo);
  printf("ME range: %d\n", param->me_range);
  printf("ME sub: %d\n", param->me_sub);
  printf("ME sub pos: %d\n", param->me_sub_pos);
  printf("ME sub range: %d\n", param->me_sub_range);
  printf("Skip threshold: %f\n", param->skip_th);
  printf("Merge num: %d\n", param->merge_num);
  printf("RDOQ: %d\n", param->rdoq);
  printf("CABAC refine: %d\n", param->cabac_refine);

  /* Main Profile Parameters */
  printf("IBC flag: %d\n", param->ibc_flag);
  printf("IBC search range X: %d\n", param->ibc_search_range_x);
  printf("IBC search range Y: %d\n", param->ibc_search_range_y);
  printf("IBC hash search flag: %d\n", param->ibc_hash_search_flag);
  printf("IBC hash search max cand: %d\n", param->ibc_hash_search_max_cand);
  printf("IBC hash search range for small blocks: %d\n",
         param->ibc_hash_search_range_4smallblk);
  printf("IBC fast method: %d\n", param->ibc_fast_method);

  /* Toolset and framework */
  printf("Toolset IDC H: %d\n", param->toolset_idc_h);
  printf("Toolset IDC L: %d\n", param->toolset_idc_l);
  printf("BTT: %d\n", param->btt);
  printf("SUCO: %d\n", param->suco);

  /* VUI parameters */
  printf("SAR: %d\n", param->sar);
  printf("SAR width: %d, SAR height: %d\n", param->sar_width,
         param->sar_height);
  printf("Video format: %d\n", param->videoformat);
  printf("Range: %d\n", param->range);
  printf("Color primaries: %d\n", param->colorprim);
  printf("Transfer characteristics: %d\n", param->transfer);
  printf("Matrix coefficients: %d\n", param->matrix_coefficients);

  /* SEI options */
  printf("Master display: %d\n", param->master_display);
  printf("Max CLL: %d\n", param->max_cll);
  printf("Max FALL: %d\n", param->max_fall);

  printf("===========================\n");
}


/* Initialize XEVE encoder for LCEVC enhancement */
static gboolean
gst_dual_encoder_init_xeve(GstDualEncoder *encoder)
{
    int ret, err;
    //err = 0;
    encoder->xeve_cdsc = NULL;
    encoder->bs_buf = NULL;
    encoder->imgb_rec = NULL;
    encoder->xeve_handle = NULL;


   encoder->xeve_cdsc = g_malloc(sizeof(XEVE_CDSC));
   memset(encoder->xeve_cdsc, 0, sizeof(XEVE_CDSC));
   
   // Use g_malloc0 to zero-initialize
    if (!encoder->xeve_cdsc) {
        GST_ERROR_OBJECT(encoder, "Failed to allocate XEVE_CDSC structure");
        return FALSE;
    }
    
    encoder->xeve_cdsc->max_bs_buf_size = MAX_BS_BUF;

  encoder->bs_buf = (unsigned char *)malloc(MAX_BS_BUF);
  if (!encoder->bs_buf) {
    printf( "Failed to allocate bitstream buffer");
    return FALSE;   
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
#if 0
  ret = xeve_param_ppt(&encoder->xeve_cdsc->param, XEVE_PROFILE_BASELINE,
                       XEVE_PRESET_FAST, XEVE_TUNE_ZEROLATENCY);

    if (XEVE_FAILED(ret)) {
    printf("cannot set profile, preset, tune to parameter: %d",
                     ret);

    ret = -1;
    goto cleanup_and_fail;

  }
#endif


    
    /* Set XEVE parameters */

    encoder->xeve_cdsc->param.w = (encoder->enhancement_width + 15) & ~15; 
    encoder->xeve_cdsc->param.h = (encoder->enhancement_height + 15) & ~15;
    encoder->xeve_cdsc->param.fps.num = encoder->fps_n;
    encoder->xeve_cdsc->param.fps.den = encoder->fps_d;
    encoder->xeve_cdsc->param.keyint = encoder->gop_size;
    encoder->xeve_cdsc->param.bframes = 0; // No B-frames for low-latency
    encoder->xeve_cdsc->param.cs = XEVE_CS_SET(
        XEVE_CF_YCBCR420, encoder->xeve_cdsc->param.codec_bit_depth, 0);
    //XEVE_CS_YCBCR420;
    encoder->xeve_cdsc->param.rc_type = XEVE_RC_ABR;
    encoder->xeve_cdsc->param.threads = (int)sysconf(_SC_NPROCESSORS_ONLN); // 8;
    encoder->xeve_cdsc->param.profile = XEVE_PROFILE_BASELINE;

 

    encoder->xeve_cdsc->param.bitrate = encoder->enhancement_bitrate; // in kbps
    int pixel_count = encoder->xeve_cdsc->param.w * encoder->xeve_cdsc->param.h;
    if (encoder->enhancement_bitrate < 1000) {
        // Set minimum bitrate based on resolution
        encoder->xeve_cdsc->param.bitrate = pixel_count * encoder->fps_n / (1000 * encoder->fps_d);
        GST_INFO_OBJECT(encoder, "Adjusted bitrate to %d kbps based on resolution",
                       encoder->xeve_cdsc->param.bitrate);
    } else {
        encoder->xeve_cdsc->param.bitrate = encoder->enhancement_bitrate;
    }

    encoder->xeve_cdsc->param.vbv_bufsize = encoder->enhancement_bitrate*2; // in kbits
    // additional settings to align with first plugin

    encoder->xeve_cdsc->param.qp = 22;
    encoder->xeve_cdsc->param.crf = 12;
    encoder->xeve_cdsc->param.aq_mode = 0;
    encoder->xeve_cdsc->param.lookahead = 17;
    encoder->xeve_cdsc->param.ref_pic_gap_length = 0;
    encoder->xeve_cdsc->param.inter_slice_type = 0;
    encoder->xeve_cdsc->param.use_fcst = 0;

    GST_INFO_OBJECT(encoder, "XEVE config: %dx%d, %dkbps, level %d, fps %d/%d",
                   encoder->xeve_cdsc->param.w, encoder->xeve_cdsc->param.h,
                   encoder->xeve_cdsc->param.bitrate, encoder->xeve_cdsc->param.level_idc,
                   encoder->xeve_cdsc->param.fps.num, encoder->xeve_cdsc->param.fps.den);




    /* Validate parameters */
    ret = xeve_param_check(&encoder->xeve_cdsc->param);

    if (ret != XEVE_OK) {
        GST_ERROR_OBJECT(encoder, "Invalid XEVE configuration: %d (0x%x)", ret, ret);
        GST_ERROR_OBJECT(encoder, "Check width=%d, height=%d, bitrate=%d", 
                        encoder->xeve_cdsc->param.w, 
                        encoder->xeve_cdsc->param.h,
                        encoder->xeve_cdsc->param.bitrate);
        free(encoder->bs_buf);
        g_free(encoder->xeve_cdsc);
        encoder->bs_buf = NULL;
        encoder->xeve_cdsc = NULL;
        return FALSE;
    }
#if 1
  ret = xeve_param_ppt(&encoder->xeve_cdsc->param, XEVE_PROFILE_BASELINE,
                       XEVE_PRESET_FAST, XEVE_TUNE_ZEROLATENCY);

    if (XEVE_FAILED(ret)) {
    printf("cannot set profile, preset, tune to parameter: %d",
                     ret);

    ret = -1;
    goto cleanup_and_fail;

  }
#endif

  if (encoder->xeve_handle) {
    xeve_delete(encoder->xeve_handle);
    encoder->xeve_handle = NULL;
  }


    GST_INFO_OBJECT(encoder, "XEVE parameters validated successfully");
print_xeve_param(&encoder->xeve_cdsc->param);
    /* Create encoder - IMPORTANT: err doit être initialisé à 0 */
    encoder->xeve_handle = xeve_create(encoder->xeve_cdsc, &err);
    if (!encoder->xeve_handle || err != XEVE_OK) {
        GST_ERROR_OBJECT(encoder, "Failed to create XEVE encoder: %d (0x%x)", err, err);
        
        // Try to get more specific error information
        if (encoder->xeve_cdsc) {
            GST_ERROR_OBJECT(encoder, "Configuration: %dx%d, bitrate=%d, fps=%d/%d",
                            encoder->xeve_cdsc->param.w,
                            encoder->xeve_cdsc->param.h,
                            encoder->xeve_cdsc->param.bitrate,
                            encoder->xeve_cdsc->param.fps.num,
                            encoder->xeve_cdsc->param.fps.den);
        }
        
        free(encoder->bs_buf);
        g_free(encoder->xeve_cdsc);
        encoder->bs_buf = NULL;
        encoder->xeve_cdsc = NULL;
        return FALSE;
    }



   

  print_xeve_param(&encoder->xeve_cdsc->param);




    /* Allocate input image buffer AFTER successful encoder creation */
    encoder->imgb_rec = imgb_alloc(
        encoder->enhancement_width, 
        encoder->enhancement_height, 
        XEVE_CS_SET(XEVE_CF_YCBCR420, 8, 0)
    );
    if (!encoder->imgb_rec) {
        GST_ERROR_OBJECT(encoder, "Failed to allocate XEVE image buffer");
        goto cleanup_and_fail;
    }

    /* Configure extra settings (SEI, etc.) */
    if (set_extra_config(encoder->xeve_handle, 1, 1) < 0) {
        GST_WARNING_OBJECT(encoder, "Failed to set extra XEVE configuration, continuing anyway");
    }

    GST_INFO_OBJECT(encoder, "XEVE encoder initialized successfully");
    return TRUE;

cleanup_and_fail:
    if (encoder->imgb_rec) {
        imgb_free(encoder->imgb_rec);
        encoder->imgb_rec = NULL;
    }
    if (encoder->xeve_handle) {
        xeve_delete(encoder->xeve_handle);
        encoder->xeve_handle = NULL;
    }
    if (encoder->bs_buf) {
        free(encoder->bs_buf);
        encoder->bs_buf = NULL;
    }
    if (encoder->xeve_cdsc) {
        g_free(encoder->xeve_cdsc);
        encoder->xeve_cdsc = NULL;
    }
    return FALSE; 
}

/* Cleanup function */
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
    
    if (encoder->bs_buf) {
        free(encoder->bs_buf);
        encoder->bs_buf = NULL;
    }
    
    if (encoder->xeve_cdsc) {
        g_free(encoder->xeve_cdsc);
        encoder->xeve_cdsc = NULL;
    }
    
    encoder->configured = FALSE;
}

/* Set caps and configure encoders */
static gboolean
gst_dual_encoder_set_format(GstVideoEncoder *vencoder, GstVideoCodecState *state)
{
    GstDualEncoder *self = GST_DUAL_ENCODER(vencoder);
    GstCaps *outcaps;
    GstVideoCodecState *output_state;
    GstVideoInfo *info = &state->info;

    GST_INFO_OBJECT(self, "set_format called with caps: %" GST_PTR_FORMAT, state->caps);

  gint width = GST_VIDEO_INFO_WIDTH(info);
  gint height = GST_VIDEO_INFO_HEIGHT(info);
  gint fps_n = GST_VIDEO_INFO_FPS_N(info);
  gint fps_d = GST_VIDEO_INFO_FPS_D(info);

  GST_INFO_OBJECT(self, "Input format: %dx%d @ %d/%d fps, format: %s", 
                   width, height, fps_n, fps_d,
                   gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(info)));

  // Check that the dimensions are valid
  if (width <= 0 || height <= 0) {
    GST_ERROR_OBJECT(self, "Invalid dimensions extracted: %dx%d", width,
                     height);
    return FALSE;
  }

  // Validate input format
    if (GST_VIDEO_INFO_FORMAT(info) != GST_VIDEO_FORMAT_I420 &&
        GST_VIDEO_INFO_FORMAT(info) != GST_VIDEO_FORMAT_YV12) {
        GST_ERROR_OBJECT(self, "Unsupported input format: %s",
                        gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(info)));
        return FALSE;
    }

  self->baseline_width = width;
  self->baseline_height = height;
  self->enhancement_width = width / 2;  // Example: enhancement is half resolution
  self->enhancement_height = height / 2;
  self->fps_n = fps_n;
  self->fps_d = fps_d;

  GST_INFO_OBJECT(self, "Configuring encoders: baseline=%dx%d@%dkbps, enhancement=%dx%d@%dkbps",
                   self->baseline_width, self->baseline_height, self->baseline_bitrate,
                   self->enhancement_width, self->enhancement_height, self->enhancement_bitrate);








    
    /* Cleanup existing encoders */
    //gst_dual_encoder_cleanup(self);
    
    self->input_info = state->info;

     
#if  1   
    /* Initialize both encoders */
    if (!gst_dual_encoder_init_x264(self)) {
        return FALSE;
    }
  
    if (!self->x264_encoder ) {
        GST_ERROR_OBJECT(self, " h264 encoders structure not initialized");
        //return FALSE;
    }
#else

   if (!gst_dual_encoder_init_xeve(self)) {
        gst_dual_encoder_cleanup(self);
        return FALSE;
    }

    if (!self->xeve_handle) {
        GST_ERROR_OBJECT(self, " lcevc encoders structure not initialized");
        //return FALSE;
    }
#endif


    outcaps = gst_caps_new_simple("video/x-h264",
                                    "lcevc", G_TYPE_BOOLEAN, TRUE,
                                    "stream-format", G_TYPE_STRING, "byte-stream",
                                    "alignment", G_TYPE_STRING, "au",
                                    NULL);


    
    
    output_state = gst_video_encoder_set_output_state(vencoder, outcaps, state);
    gst_video_codec_state_unref(output_state);
    
    self->configured = TRUE;
        GST_INFO_OBJECT(self, "Format set successfully");
    
    return TRUE;
}
static GstBuffer *
scale_frame(GstVideoInfo *src_info, GstBuffer *src_buffer, 
            gint dst_width, gint dst_height)
{
    GstMapInfo src_map;
    GstBuffer* dst_buffer = NULL;
    guint8* src_data = NULL;
    guint8* dst_data = NULL;
    
    gint src_width = 0;
    gint src_height = 0;


    src_width = GST_VIDEO_INFO_WIDTH(src_info);
    src_height = GST_VIDEO_INFO_HEIGHT(src_info);
    g_return_val_if_fail(src_buffer != NULL, NULL);
    g_return_val_if_fail(src_width > 0 && src_height > 0, NULL);
    g_return_val_if_fail(dst_width > 0 && dst_height > 0, NULL);

    /* Check if scaling is actually needed */
    if (src_width == dst_width &&
        src_height == dst_height) {
        return gst_buffer_ref(src_buffer);
    }

// Map the source buffer for reading
    if (!gst_buffer_map(src_buffer, &src_map, GST_MAP_READ)) {
        GST_ERROR("Failed to map source buffer");
        return NULL;
    }
    
    src_data = src_map.data;
    
    // Calculate buffer sizes
    gsize src_y_size = src_width * src_height;
    gsize src_u_size = (src_width / 2) * (src_height / 2);
    gsize src_v_size = src_u_size;
    
    gsize dst_y_size = dst_width * dst_height;
    gsize dst_u_size = (dst_width / 2) * (dst_height / 2);
    gsize dst_v_size = dst_u_size;
    gsize dst_total_size = dst_y_size + dst_u_size + dst_v_size;
    
    // Create destination buffer
    dst_buffer = gst_buffer_new_allocate(NULL, dst_total_size, NULL);
    if (!dst_buffer) {
        GST_ERROR("Failed to allocate destination buffer");
        gst_buffer_unmap(src_buffer, &src_map);
        return NULL;
    }
    
    // Map destination buffer for writing
    GstMapInfo dst_map;
    if (!gst_buffer_map(dst_buffer, &dst_map, GST_MAP_WRITE)) {
        GST_ERROR("Failed to map destination buffer");
        gst_buffer_unref(dst_buffer);
        gst_buffer_unmap(src_buffer, &src_map);
        return NULL;
    }
    
    dst_data = dst_map.data;
    
    // Get pointers to Y, U, V planes
    guint8* src_y = src_data;
    guint8* src_u = src_y + src_y_size;
    guint8* src_v = src_u + src_u_size;
    
    guint8* dst_y = dst_data;
    guint8* dst_u = dst_y + dst_y_size;
    guint8* dst_v = dst_u + dst_u_size;
    
    // Calculate scaling ratios
    gfloat x_ratio = (gfloat)src_width / dst_width;
    gfloat y_ratio = (gfloat)src_height / dst_height;
    
    // Rescale Y plane (luma)
    for (gint y = 0; y < dst_height; y++) {
        gint src_y_pos = (gint)(y * y_ratio) * src_width;
        gint dst_y_pos = y * dst_width;
        
        for (gint x = 0; x < dst_width; x++) {
            gint src_x_pos = (gint)(x * x_ratio);
            dst_y[dst_y_pos + x] = src_y[src_y_pos + src_x_pos];
        }
    }
    
    // Rescale U and V planes (chroma)
    for (gint y = 0; y < dst_height / 2; y++) {
        gint src_y_pos = (gint)(y * y_ratio) * (src_width / 2);
        gint dst_y_pos = y * (dst_width / 2);
        
        for (gint x = 0; x < dst_width / 2; x++) {
            gint src_x_pos = (gint)(x * x_ratio);
            
            // U plane
            dst_u[dst_y_pos + x] = src_u[src_y_pos + src_x_pos];
            
            // V plane
            dst_v[dst_y_pos + x] = src_v[src_y_pos + src_x_pos];
        }
    }
    
    // Set buffer metadata
    GstVideoMeta* video_meta = gst_buffer_get_video_meta(src_buffer);
    if (video_meta) {
        gst_buffer_add_video_meta(dst_buffer, GST_VIDEO_FRAME_FLAG_NONE,
                                 GST_VIDEO_FORMAT_I420, dst_width, dst_height);
    }
    
    // Copy timestamp and duration
    GST_BUFFER_PTS(dst_buffer) = GST_BUFFER_PTS(src_buffer);
    GST_BUFFER_DTS(dst_buffer) = GST_BUFFER_DTS(src_buffer);
    GST_BUFFER_DURATION(dst_buffer) = GST_BUFFER_DURATION(src_buffer);
    GST_BUFFER_OFFSET(dst_buffer) = GST_BUFFER_OFFSET(src_buffer);
    GST_BUFFER_OFFSET_END(dst_buffer) = GST_BUFFER_OFFSET_END(src_buffer);
    
    // Unmap buffers
    gst_buffer_unmap(dst_buffer, &dst_map);
    gst_buffer_unmap(src_buffer, &src_map);
    
    return dst_buffer;
}

/**
 * Alternative version using bilinear interpolation for better quality
 */
static GstBuffer *
scale_frame_bilinear(GstVideoInfo *src_info, GstBuffer *src_buffer, 
            gint dst_width, gint dst_height)
{
    GstMapInfo src_map;
    GstBuffer* dst_buffer = NULL;
    guint8* src_data = NULL;
    guint8* dst_data = NULL;
    
    gint src_width = 0;
    gint src_height = 0;


    src_width = GST_VIDEO_INFO_WIDTH(src_info);
    src_height = GST_VIDEO_INFO_HEIGHT(src_info);
    g_return_val_if_fail(src_buffer != NULL, NULL);
    g_return_val_if_fail(src_width > 0 && src_height > 0, NULL);
    g_return_val_if_fail(dst_width > 0 && dst_height > 0, NULL);

    /* Check if scaling is actually needed */
    if (src_width == dst_width &&
        src_height == dst_height) {
        return gst_buffer_ref(src_buffer);
    }

// Map the source buffer for reading
    if (!gst_buffer_map(src_buffer, &src_map, GST_MAP_READ)) {
        GST_ERROR("Failed to map source buffer");
        return NULL;
    }
    
    src_data = src_map.data;
    
    gsize dst_y_size = dst_width * dst_height;
    gsize dst_u_size = (dst_width / 2) * (dst_height / 2);
    gsize dst_v_size = dst_u_size;
    gsize dst_total_size = dst_y_size + dst_u_size + dst_v_size;
    
    dst_buffer = gst_buffer_new_allocate(NULL, dst_total_size, NULL);
    if (!dst_buffer) {
        gst_buffer_unmap(src_buffer, &src_map);
        return NULL;
    }
    
    GstMapInfo dst_map;
    if (!gst_buffer_map(dst_buffer, &dst_map, GST_MAP_WRITE)) {
        gst_buffer_unref(dst_buffer);
        gst_buffer_unmap(src_buffer, &src_map);
        return NULL;
    }
    
    dst_data = dst_map.data;
    
    guint8* src_y = src_data;
    guint8* src_u = src_y + src_width * src_height;
    guint8* src_v = src_u + (src_width / 2) * (src_height / 2);
    
    guint8* dst_y = dst_data;
    guint8* dst_u = dst_y + dst_y_size;
    guint8* dst_v = dst_u + dst_u_size;
    
    gfloat x_ratio = (gfloat)(src_width - 1) / dst_width;
    gfloat y_ratio = (gfloat)(src_height - 1) / dst_height;
    
    // Bilinear interpolation for Y plane
    for (gint y = 0; y < dst_height; y++) {
        for (gint x = 0; x < dst_width; x++) {
            gfloat x_src = x * x_ratio;
            gfloat y_src = y * y_ratio;
            
            gint x1 = (gint)x_src;
            gint y1 = (gint)y_src;
            gint x2 = x1 + 1;
            gint y2 = y1 + 1;
            
            if (x2 >= src_width) x2 = src_width - 1;
            if (y2 >= src_height) y2 = src_height - 1;
            
            gfloat x_diff = x_src - x1;
            gfloat y_diff = y_src - y1;
            
            guint8 a = src_y[y1 * src_width + x1];
            guint8 b = src_y[y1 * src_width + x2];
            guint8 c = src_y[y2 * src_width + x1];
            guint8 d = src_y[y2 * src_width + x2];
            
            dst_y[y * dst_width + x] = (guint8)(
                a * (1 - x_diff) * (1 - y_diff) +
                b * x_diff * (1 - y_diff) +
                c * y_diff * (1 - x_diff) +
                d * x_diff * y_diff
            );
        }
    }
    
    // Bilinear interpolation for U and V planes
    gfloat uv_x_ratio = (gfloat)(src_width / 2 - 1) / (dst_width / 2);
    gfloat uv_y_ratio = (gfloat)(src_height / 2 - 1) / (dst_height / 2);
    
    for (gint y = 0; y < dst_height / 2; y++) {
        for (gint x = 0; x < dst_width / 2; x++) {
            gfloat x_src = x * uv_x_ratio;
            gfloat y_src = y * uv_y_ratio;
            
            gint x1 = (gint)x_src;
            gint y1 = (gint)y_src;
            gint x2 = x1 + 1;
            gint y2 = y1 + 1;
            
            if (x2 >= src_width / 2) x2 = src_width / 2 - 1;
            if (y2 >= src_height / 2) y2 = src_height / 2 - 1;
            
            gfloat x_diff = x_src - x1;
            gfloat y_diff = y_src - y1;
            
            // U plane
            guint8 u_a = src_u[y1 * (src_width / 2) + x1];
            guint8 u_b = src_u[y1 * (src_width / 2) + x2];
            guint8 u_c = src_u[y2 * (src_width / 2) + x1];
            guint8 u_d = src_u[y2 * (src_width / 2) + x2];
            
            dst_u[y * (dst_width / 2) + x] = (guint8)(
                u_a * (1 - x_diff) * (1 - y_diff) +
                u_b * x_diff * (1 - y_diff) +
                u_c * y_diff * (1 - x_diff) +
                u_d * x_diff * y_diff
            );
            
            // V plane
            guint8 v_a = src_v[y1 * (src_width / 2) + x1];
            guint8 v_b = src_v[y1 * (src_width / 2) + x2];
            guint8 v_c = src_v[y2 * (src_width / 2) + x1];
            guint8 v_d = src_v[y2 * (src_width / 2) + x2];
            
            dst_v[y * (dst_width / 2) + x] = (guint8)(
                v_a * (1 - x_diff) * (1 - y_diff) +
                v_b * x_diff * (1 - y_diff) +
                v_c * y_diff * (1 - x_diff) +
                v_d * x_diff * y_diff
            );
        }
    }

    // Set buffer metadata
    GstVideoMeta* video_meta = gst_buffer_get_video_meta(src_buffer);
    if (video_meta) {
        gst_buffer_add_video_meta(dst_buffer, GST_VIDEO_FRAME_FLAG_NONE,
                                 GST_VIDEO_FORMAT_I420, dst_width, dst_height);
    }
    
    // Copy metadata
    // Copy timestamp and duration
    GST_BUFFER_PTS(dst_buffer) = GST_BUFFER_PTS(src_buffer);
    GST_BUFFER_DTS(dst_buffer) = GST_BUFFER_DTS(src_buffer);
    GST_BUFFER_DURATION(dst_buffer) = GST_BUFFER_DURATION(src_buffer);
    GST_BUFFER_OFFSET(dst_buffer) = GST_BUFFER_OFFSET(src_buffer);
    GST_BUFFER_OFFSET_END(dst_buffer) = GST_BUFFER_OFFSET_END(src_buffer);
    
    gst_buffer_unmap(dst_buffer, &dst_map);
    gst_buffer_unmap(src_buffer, &src_map);
    
    
    



    
    return dst_buffer;
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
static gboolean gst_dual_encoder_start(GstVideoEncoder *encoder) {
        GstDualEncoder *self = GST_DUAL_ENCODER(encoder);
        GST_DEBUG_OBJECT(self, "start called");

        if(!self->xeve_cdsc) {
            GST_ERROR_OBJECT(self, "Lcevc Encoder not initialized");
            return FALSE;
        }
        else {
            GST_INFO_OBJECT(self, "Lcevc Encoder already initialized");
        }

        if(!self->x264_encoder) {
            GST_ERROR_OBJECT(self, "H264 Encoder not initialized");
            return FALSE;
        }
        else {
            GST_INFO_OBJECT(self, "H264 Encoder already initialized");
        }
        return TRUE;
    }   

static gboolean gst_dual_encoder_stop(GstVideoEncoder *encoder) {
        GstDualEncoder *self = GST_DUAL_ENCODER(encoder);
        GST_DEBUG_OBJECT(self, "stop called");

        if(!self->xeve_handle) {
            xeve_delete(self->xeve_handle);
            self->xeve_handle = NULL;
            
        }




        if(!self->x264_encoder) {
            GST_ERROR_OBJECT(self, "H264 Encoder not initialized");
            return FALSE;
        }
        else {
            GST_INFO_OBJECT(self, "H264 Encoder already initialized");
        }
        return TRUE;
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
        case PROP_GOP_SIZE:
            encoder->gop_size = g_value_get_int(value);
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
        case PROP_GOP_SIZE:
            g_value_set_int(value, encoder->gop_size);
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
                         "Bitrate for LCEVC enhancement stream (kbps)", 1, G_MAXINT, 2000,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    
    g_object_class_install_property(gobject_class, PROP_GOP_SIZE,
        g_param_spec_int("gop-size", "GOP Size",
                         "Maximum GOP size (keyframe interval)", 1, G_MAXINT, 60,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /* Set element details */
    gst_element_class_set_static_metadata(element_class,
        "Dual H.264/LCEVC Encoder",
        "Codec/Encoder/Video",
        "Encode video with both H.264 baseline and LCEVC enhancement layers",
        "Le Blond Erwan <erwanleblond@gmail.com>");
    
    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);
    encoder_class->start = gst_dual_encoder_start;
    encoder_class->stop = gst_dual_encoder_stop;


    encoder_class->set_format = GST_DEBUG_FUNCPTR(gst_dual_encoder_set_format);
    encoder_class->handle_frame = GST_DEBUG_FUNCPTR(gst_dual_encoder_handle_frame);
}

/* Instance initialization */
#if 1
static void
gst_dual_encoder_init(GstDualEncoder *encoder)
{
    /* Set default properties */
    encoder->baseline_width = 640;
    encoder->baseline_height = 480;
    encoder->baseline_bitrate = 500;
    encoder->enhancement_width = 1920;
    encoder->enhancement_height = 1080;
    encoder->enhancement_bitrate = 2000;
    encoder->configured = FALSE;
    encoder->x264_encoder = NULL;
    encoder->xeve_handle = NULL;
    encoder->imgb_rec = NULL;
    encoder->bs_buf = NULL;
    encoder->gop_size = 60;
    encoder->fps_n = 30;
    encoder->fps_d = 1;

}
#endif 

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