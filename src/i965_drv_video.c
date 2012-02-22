/*
 * Copyright � 2009 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Xiang Haihao <haihao.xiang@intel.com>
 *    Zou Nan hai <nanhai.zou@intel.com>
 *
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <va/va_dricommon.h>

#include "intel_driver.h"
#include "intel_memman.h"
#include "intel_batchbuffer.h"
#include "i965_defines.h"
#include "i965_drv_video.h"

#define CONFIG_ID_OFFSET                0x01000000
#define CONTEXT_ID_OFFSET               0x02000000
#define SURFACE_ID_OFFSET               0x04000000
#define BUFFER_ID_OFFSET                0x08000000
#define IMAGE_ID_OFFSET                 0x0a000000
#define SUBPIC_ID_OFFSET                0x10000000

#define HAS_MPEG2(ctx)  (IS_G4X((ctx)->intel.device_id) ||      \
                         IS_IRONLAKE((ctx)->intel.device_id) || \
                         ((IS_GEN6((ctx)->intel.device_id) ||   \
                           IS_GEN7((ctx)->intel.device_id)) &&  \
                          (ctx)->intel.has_bsd))

#define HAS_H264(ctx)   ((IS_GEN7((ctx)->intel.device_id) ||            \
                          IS_GEN6((ctx)->intel.device_id) ||            \
                          IS_IRONLAKE((ctx)->intel.device_id)) &&       \
                         (ctx)->intel.has_bsd)

#define HAS_VC1(ctx)    ((IS_GEN7((ctx)->intel.device_id) ||    \
                          IS_GEN6((ctx)->intel.device_id)) &&   \
                         (ctx)->intel.has_bsd)

#define HAS_TILED_SURFACE(ctx) ((IS_GEN7((ctx)->intel.device_id) ||     \
                                 IS_GEN6((ctx)->intel.device_id)) &&    \
                                (ctx)->render_state.interleaved_uv)

#define HAS_ENCODER(ctx)        ((IS_GEN7((ctx)->intel.device_id) ||    \
                                  IS_GEN6((ctx)->intel.device_id)) &&   \
                                 (ctx)->intel.has_bsd)

#define HAS_VPP(ctx)    (IS_IRONLAKE((ctx)->intel.device_id) ||     \
                         IS_GEN6((ctx)->intel.device_id) ||         \
                         IS_GEN7((ctx)->intel.device_id))

#define HAS_JPEG(ctx)   (IS_GEN7((ctx)->intel.device_id) &&     \
                         (ctx)->intel.has_bsd)

#define HAS_ACCELERATED_GETIMAGE(ctx)   (IS_GEN6((ctx)->intel.device_id) ||     \
                                         IS_GEN7((ctx)->intel.device_id))

#define HAS_ACCELERATED_PUTIMAGE(ctx)   HAS_VPP(ctx)

enum {
    I965_SURFACETYPE_RGBA = 1,
    I965_SURFACETYPE_YUV,
    I965_SURFACETYPE_INDEXED
};

/* List of supported image formats */
typedef struct {
    unsigned int        type;
    VAImageFormat       va_format;
} i965_image_format_map_t;

static const i965_image_format_map_t
i965_image_formats_map[I965_MAX_IMAGE_FORMATS + 1] = {
    { I965_SURFACETYPE_YUV,
      { VA_FOURCC('Y','V','1','2'), VA_LSB_FIRST, 12, } },
    { I965_SURFACETYPE_YUV,
      { VA_FOURCC('I','4','2','0'), VA_LSB_FIRST, 12, } },
    { I965_SURFACETYPE_YUV,
      { VA_FOURCC('N','V','1','2'), VA_LSB_FIRST, 12, } },
};

/* List of supported subpicture formats */
typedef struct {
    unsigned int        type;
    unsigned int        format;
    VAImageFormat       va_format;
    unsigned int        va_flags;
} i965_subpic_format_map_t;

static const i965_subpic_format_map_t
i965_subpic_formats_map[I965_MAX_SUBPIC_FORMATS + 1] = {
    { I965_SURFACETYPE_INDEXED, I965_SURFACEFORMAT_P4A4_UNORM,
      { VA_FOURCC('I','A','4','4'), VA_MSB_FIRST, 8, },
      VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD },
    { I965_SURFACETYPE_INDEXED, I965_SURFACEFORMAT_A4P4_UNORM,
      { VA_FOURCC('A','I','4','4'), VA_MSB_FIRST, 8, },
      VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD },
    { I965_SURFACETYPE_RGBA, I965_SURFACEFORMAT_B8G8R8A8_UNORM,
      { VA_FOURCC('B','G','R','A'), VA_LSB_FIRST, 32,
        32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000 },
      VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD },
    { I965_SURFACETYPE_RGBA, I965_SURFACEFORMAT_R8G8B8A8_UNORM,
      { VA_FOURCC('R','G','B','A'), VA_LSB_FIRST, 32,
        32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000 },
      VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD },
};

static const i965_subpic_format_map_t *
get_subpic_format(const VAImageFormat *va_format)
{
    unsigned int i;
    for (i = 0; i965_subpic_formats_map[i].type != 0; i++) {
        const i965_subpic_format_map_t * const m = &i965_subpic_formats_map[i];
        if (m->va_format.fourcc == va_format->fourcc &&
            (m->type == I965_SURFACETYPE_RGBA ?
             (m->va_format.byte_order == va_format->byte_order &&
              m->va_format.red_mask   == va_format->red_mask   &&
              m->va_format.green_mask == va_format->green_mask &&
              m->va_format.blue_mask  == va_format->blue_mask  &&
              m->va_format.alpha_mask == va_format->alpha_mask) : 1))
            return m;
    }
    return NULL;
}

extern struct hw_context *i965_proc_context_init(VADriverContextP, struct object_config *);
extern struct hw_context *g4x_dec_hw_context_init(VADriverContextP, struct object_config *);
static struct hw_codec_info g4x_hw_codec_info = {
    .dec_hw_context_init = g4x_dec_hw_context_init,
    .enc_hw_context_init = NULL,
    .proc_hw_context_init = NULL,
};

extern struct hw_context *ironlake_dec_hw_context_init(VADriverContextP, struct object_config *);
static struct hw_codec_info ironlake_hw_codec_info = {
    .dec_hw_context_init = ironlake_dec_hw_context_init,
    .enc_hw_context_init = NULL,
    .proc_hw_context_init = i965_proc_context_init,
};

extern struct hw_context *gen6_dec_hw_context_init(VADriverContextP, struct object_config *);
extern struct hw_context *gen6_enc_hw_context_init(VADriverContextP, struct object_config *);
static struct hw_codec_info gen6_hw_codec_info = {
    .dec_hw_context_init = gen6_dec_hw_context_init,
    .enc_hw_context_init = gen6_enc_hw_context_init,
    .proc_hw_context_init = i965_proc_context_init,
};

extern struct hw_context *gen7_dec_hw_context_init(VADriverContextP, struct object_config *);
extern struct hw_context *gen7_enc_hw_context_init(VADriverContextP, struct object_config *);
static struct hw_codec_info gen7_hw_codec_info = {
    .dec_hw_context_init = gen7_dec_hw_context_init,
    .enc_hw_context_init = gen7_enc_hw_context_init,
    .proc_hw_context_init = i965_proc_context_init,
};

VAStatus 
i965_QueryConfigProfiles(VADriverContextP ctx,
                         VAProfile *profile_list,       /* out */
                         int *num_profiles)             /* out */
{
    struct i965_driver_data * const i965 = i965_driver_data(ctx);
    int i = 0;

    if (HAS_MPEG2(i965)) {
        profile_list[i++] = VAProfileMPEG2Simple;
        profile_list[i++] = VAProfileMPEG2Main;
    }

    if (HAS_H264(i965)) {
        profile_list[i++] = VAProfileH264Baseline;
        profile_list[i++] = VAProfileH264Main;
        profile_list[i++] = VAProfileH264High;
    }

    if (HAS_VC1(i965)) {
        profile_list[i++] = VAProfileVC1Simple;
        profile_list[i++] = VAProfileVC1Main;
        profile_list[i++] = VAProfileVC1Advanced;
    }

    if (HAS_VPP(i965)) {
        profile_list[i++] = VAProfileNone;
    }

    if (HAS_JPEG(i965)) {
        profile_list[i++] = VAProfileJPEGBaseline;
    }

    /* If the assert fails then I965_MAX_PROFILES needs to be bigger */
    assert(i <= I965_MAX_PROFILES);
    *num_profiles = i;

    return VA_STATUS_SUCCESS;
}

VAStatus 
i965_QueryConfigEntrypoints(VADriverContextP ctx,
                            VAProfile profile,
                            VAEntrypoint *entrypoint_list,      /* out */
                            int *num_entrypoints)               /* out */
{
    struct i965_driver_data * const i965 = i965_driver_data(ctx);
    int n = 0;

    switch (profile) {
    case VAProfileMPEG2Simple:
    case VAProfileMPEG2Main:
        if (HAS_MPEG2(i965))
            entrypoint_list[n++] = VAEntrypointVLD;
        break;

    case VAProfileH264Baseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        if (HAS_H264(i965))
            entrypoint_list[n++] = VAEntrypointVLD;
        
        if (HAS_ENCODER(i965))
            entrypoint_list[n++] = VAEntrypointEncSlice;

        break;

    case VAProfileVC1Simple:
    case VAProfileVC1Main:
    case VAProfileVC1Advanced:
        if (HAS_VC1(i965))
            entrypoint_list[n++] = VAEntrypointVLD;
        break;

    case VAProfileNone:
        if (HAS_VPP(i965))
            entrypoint_list[n++] = VAEntrypointVideoProc;
        break;

    case VAProfileJPEGBaseline:
        if (HAS_JPEG(i965))
            entrypoint_list[n++] = VAEntrypointVLD;
        break;

    default:
        break;
    }

    /* If the assert fails then I965_MAX_ENTRYPOINTS needs to be bigger */
    assert(n <= I965_MAX_ENTRYPOINTS);
    *num_entrypoints = n;
    return n > 0 ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
}

VAStatus 
i965_GetConfigAttributes(VADriverContextP ctx,
                         VAProfile profile,
                         VAEntrypoint entrypoint,
                         VAConfigAttrib *attrib_list,  /* in/out */
                         int num_attribs)
{
    int i;

    /* Other attributes don't seem to be defined */
    /* What to do if we don't know the attribute? */
    for (i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            attrib_list[i].value = VA_RT_FORMAT_YUV420;
            break;

        case VAConfigAttribRateControl:
            if (entrypoint == VAEntrypointEncSlice) {
                attrib_list[i].value = VA_RC_CBR | VA_RC_CQP;
                break;
            }

        case VAConfigAttribEncPackedHeaders:
            if (entrypoint == VAEntrypointEncSlice) {
                attrib_list[i].value = VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE;
                break;
            }

        default:
            /* Do nothing */
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }

    return VA_STATUS_SUCCESS;
}

static void 
i965_destroy_config(struct object_heap *heap, struct object_base *obj)
{
    object_heap_free(heap, obj);
}

static VAStatus 
i965_update_attribute(struct object_config *obj_config, VAConfigAttrib *attrib)
{
    int i;

    /* Check existing attrbiutes */
    for (i = 0; i < obj_config->num_attribs; i++) {
        if (obj_config->attrib_list[i].type == attrib->type) {
            /* Update existing attribute */
            obj_config->attrib_list[i].value = attrib->value;
            return VA_STATUS_SUCCESS;
        }
    }

    if (obj_config->num_attribs < I965_MAX_CONFIG_ATTRIBUTES) {
        i = obj_config->num_attribs;
        obj_config->attrib_list[i].type = attrib->type;
        obj_config->attrib_list[i].value = attrib->value;
        obj_config->num_attribs++;
        return VA_STATUS_SUCCESS;
    }

    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

VAStatus 
i965_CreateConfig(VADriverContextP ctx,
                  VAProfile profile,
                  VAEntrypoint entrypoint,
                  VAConfigAttrib *attrib_list,
                  int num_attribs,
                  VAConfigID *config_id)        /* out */
{
    struct i965_driver_data * const i965 = i965_driver_data(ctx);
    struct object_config *obj_config;
    int configID;
    int i;
    VAStatus vaStatus;

    /* Validate profile & entrypoint */
    switch (profile) {
    case VAProfileMPEG2Simple:
    case VAProfileMPEG2Main:
        if (HAS_MPEG2(i965) && VAEntrypointVLD == entrypoint) {
            vaStatus = VA_STATUS_SUCCESS;
        } else {
            vaStatus = VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        }
        break;

    case VAProfileH264Baseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        if ((HAS_H264(i965) && VAEntrypointVLD == entrypoint) ||
            (HAS_ENCODER(i965) && VAEntrypointEncSlice == entrypoint)) {
            vaStatus = VA_STATUS_SUCCESS;
        } else {
            vaStatus = VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        }

        break;

    case VAProfileVC1Simple:
    case VAProfileVC1Main:
    case VAProfileVC1Advanced:
        if (HAS_VC1(i965) && VAEntrypointVLD == entrypoint) {
            vaStatus = VA_STATUS_SUCCESS;
        } else {
            vaStatus = VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        }

        break;

    case VAProfileNone:
        if (HAS_VPP(i965) && VAEntrypointVideoProc == entrypoint) {
            vaStatus = VA_STATUS_SUCCESS;
        } else {
            vaStatus = VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        }

        break;

    case VAProfileJPEGBaseline:
        if (HAS_JPEG(i965) && VAEntrypointVLD == entrypoint) {
            vaStatus = VA_STATUS_SUCCESS;
        } else {
            vaStatus = VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        }

        break;

    default:
        vaStatus = VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        break;
    }

    if (VA_STATUS_SUCCESS != vaStatus) {
        return vaStatus;
    }

    configID = NEW_CONFIG_ID();
    obj_config = CONFIG(configID);

    if (NULL == obj_config) {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        return vaStatus;
    }

    obj_config->profile = profile;
    obj_config->entrypoint = entrypoint;
    obj_config->attrib_list[0].type = VAConfigAttribRTFormat;
    obj_config->attrib_list[0].value = VA_RT_FORMAT_YUV420;
    obj_config->num_attribs = 1;

    for(i = 0; i < num_attribs; i++) {
        vaStatus = i965_update_attribute(obj_config, &(attrib_list[i]));

        if (VA_STATUS_SUCCESS != vaStatus) {
            break;
        }
    }

    /* Error recovery */
    if (VA_STATUS_SUCCESS != vaStatus) {
        i965_destroy_config(&i965->config_heap, (struct object_base *)obj_config);
    } else {
        *config_id = configID;
    }

    return vaStatus;
}

VAStatus 
i965_DestroyConfig(VADriverContextP ctx, VAConfigID config_id)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_config *obj_config = CONFIG(config_id);
    VAStatus vaStatus;

    if (NULL == obj_config) {
        vaStatus = VA_STATUS_ERROR_INVALID_CONFIG;
        return vaStatus;
    }

    i965_destroy_config(&i965->config_heap, (struct object_base *)obj_config);
    return VA_STATUS_SUCCESS;
}

VAStatus i965_QueryConfigAttributes(VADriverContextP ctx,
                                    VAConfigID config_id,
                                    VAProfile *profile,                 /* out */
                                    VAEntrypoint *entrypoint,           /* out */
                                    VAConfigAttrib *attrib_list,        /* out */
                                    int *num_attribs)                   /* out */
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_config *obj_config = CONFIG(config_id);
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int i;

    assert(obj_config);
    *profile = obj_config->profile;
    *entrypoint = obj_config->entrypoint;
    *num_attribs = obj_config->num_attribs;

    for(i = 0; i < obj_config->num_attribs; i++) {
        attrib_list[i] = obj_config->attrib_list[i];
    }

    return vaStatus;
}

static void 
i965_destroy_surface(struct object_heap *heap, struct object_base *obj)
{
    struct object_surface *obj_surface = (struct object_surface *)obj;

    dri_bo_unreference(obj_surface->bo);
    obj_surface->bo = NULL;

    if (obj_surface->free_private_data != NULL) {
        obj_surface->free_private_data(&obj_surface->private_data);
        obj_surface->private_data = NULL;
    }

    object_heap_free(heap, obj);
}

VAStatus 
i965_CreateSurfaces(VADriverContextP ctx,
                    int width,
                    int height,
                    int format,
                    int num_surfaces,
                    VASurfaceID *surfaces)      /* out */
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    int i;
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    /* We only support one format */
    if (VA_RT_FORMAT_YUV420 != format) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    for (i = 0; i < num_surfaces; i++) {
        int surfaceID = NEW_SURFACE_ID();
        struct object_surface *obj_surface = SURFACE(surfaceID);

        if (NULL == obj_surface) {
            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
            break;
        }

        surfaces[i] = surfaceID;
        obj_surface->status = VASurfaceReady;
        obj_surface->subpic = VA_INVALID_ID;
        obj_surface->orig_width = width;
        obj_surface->orig_height = height;

        obj_surface->width = ALIGN(width, 16);
        obj_surface->height = ALIGN(height, 16);
        obj_surface->flags = SURFACE_REFERENCED;
        obj_surface->fourcc = 0;
        obj_surface->bo = NULL;
        obj_surface->locked_image_id = VA_INVALID_ID;
        obj_surface->private_data = NULL;
        obj_surface->free_private_data = NULL;
        obj_surface->subsampling = SUBSAMPLE_YUV420;
    }

    /* Error recovery */
    if (VA_STATUS_SUCCESS != vaStatus) {
        /* surfaces[i-1] was the last successful allocation */
        for (; i--; ) {
            struct object_surface *obj_surface = SURFACE(surfaces[i]);

            surfaces[i] = VA_INVALID_SURFACE;
            assert(obj_surface);
            i965_destroy_surface(&i965->surface_heap, (struct object_base *)obj_surface);
        }
    }

    return vaStatus;
}

VAStatus 
i965_DestroySurfaces(VADriverContextP ctx,
                     VASurfaceID *surface_list,
                     int num_surfaces)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    int i;

    for (i = num_surfaces; i--; ) {
        struct object_surface *obj_surface = SURFACE(surface_list[i]);

        assert(obj_surface);
        i965_destroy_surface(&i965->surface_heap, (struct object_base *)obj_surface);
    }

    return VA_STATUS_SUCCESS;
}

VAStatus 
i965_QueryImageFormats(VADriverContextP ctx,
                       VAImageFormat *format_list,      /* out */
                       int *num_formats)                /* out */
{
    int n;

    for (n = 0; i965_image_formats_map[n].va_format.fourcc != 0; n++) {
        const i965_image_format_map_t * const m = &i965_image_formats_map[n];
        if (format_list)
            format_list[n] = m->va_format;
    }

    if (num_formats)
        *num_formats = n;

    return VA_STATUS_SUCCESS;
}

/*
 * Guess the format when the usage of a VA surface is unknown
 * 1. Without a valid context: YV12
 * 2. The current context is valid:
 *    a) always NV12 on GEN6 and later
 *    b) I420 for MPEG-2 and NV12 for other codec on GEN4 & GEN5
 */
static void
i965_guess_surface_format(VADriverContextP ctx,
                          VASurfaceID surface,
                          unsigned int *fourcc,
                          unsigned int *is_tiled)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_context *obj_context = NULL;
    struct object_config *obj_config = NULL;

    *fourcc = VA_FOURCC('Y', 'V', '1', '2');
    *is_tiled = 0;

    if (i965->current_context_id == VA_INVALID_ID)
        return;

    obj_context = CONTEXT(i965->current_context_id);

    if (!obj_context || obj_context->config_id == VA_INVALID_ID)
        return;

    obj_config = CONFIG(obj_context->config_id);

    if (!obj_config)
        return;

    if (IS_GEN6(i965->intel.device_id) || IS_GEN7(i965->intel.device_id)) {
        *fourcc = VA_FOURCC('N', 'V', '1', '2');
        *is_tiled = 1;
        return;
    }

    switch (obj_config->profile) {
    case VAProfileMPEG2Simple:
    case VAProfileMPEG2Main:
        *fourcc = VA_FOURCC('I', '4', '2', '0');
        *is_tiled = 0;
        break;

    default:
        *fourcc = VA_FOURCC('N', 'V', '1', '2');
        *is_tiled = 0;
        break;
    }
}

VAStatus 
i965_QuerySubpictureFormats(VADriverContextP ctx,
                            VAImageFormat *format_list,         /* out */
                            unsigned int *flags,                /* out */
                            unsigned int *num_formats)          /* out */
{
    int n;

    for (n = 0; i965_subpic_formats_map[n].va_format.fourcc != 0; n++) {
        const i965_subpic_format_map_t * const m = &i965_subpic_formats_map[n];
        if (format_list)
            format_list[n] = m->va_format;
        if (flags)
            flags[n] = m->va_flags;
    }

    if (num_formats)
        *num_formats = n;

    return VA_STATUS_SUCCESS;
}

static void 
i965_destroy_subpic(struct object_heap *heap, struct object_base *obj)
{
    //    struct object_subpic *obj_subpic = (struct object_subpic *)obj;

    object_heap_free(heap, obj);
}

VAStatus 
i965_CreateSubpicture(VADriverContextP ctx,
                      VAImageID image,
                      VASubpictureID *subpicture)         /* out */
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    VASubpictureID subpicID = NEW_SUBPIC_ID()
    struct object_subpic *obj_subpic = SUBPIC(subpicID);

    if (!obj_subpic)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    struct object_image *obj_image = IMAGE(image);
    if (!obj_image)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    const i965_subpic_format_map_t * const m = get_subpic_format(&obj_image->image.format);
    if (!m)
        return VA_STATUS_ERROR_UNKNOWN; /* XXX: VA_STATUS_ERROR_UNSUPPORTED_FORMAT? */

    *subpicture = subpicID;
    obj_subpic->image  = image;
    obj_subpic->format = m->format;
    obj_subpic->width  = obj_image->image.width;
    obj_subpic->height = obj_image->image.height;
    obj_subpic->pitch  = obj_image->image.pitches[0];
    obj_subpic->bo     = obj_image->bo;
    return VA_STATUS_SUCCESS;
}

VAStatus 
i965_DestroySubpicture(VADriverContextP ctx,
                       VASubpictureID subpicture)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_subpic *obj_subpic = SUBPIC(subpicture);
    i965_destroy_subpic(&i965->subpic_heap, (struct object_base *)obj_subpic);
    return VA_STATUS_SUCCESS;
}

VAStatus 
i965_SetSubpictureImage(VADriverContextP ctx,
                        VASubpictureID subpicture,
                        VAImageID image)
{
    /* TODO */
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus 
i965_SetSubpictureChromakey(VADriverContextP ctx,
                            VASubpictureID subpicture,
                            unsigned int chromakey_min,
                            unsigned int chromakey_max,
                            unsigned int chromakey_mask)
{
    /* TODO */
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus 
i965_SetSubpictureGlobalAlpha(VADriverContextP ctx,
                              VASubpictureID subpicture,
                              float global_alpha)
{
    /* TODO */
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus 
i965_AssociateSubpicture(VADriverContextP ctx,
                         VASubpictureID subpicture,
                         VASurfaceID *target_surfaces,
                         int num_surfaces,
                         short src_x, /* upper left offset in subpicture */
                         short src_y,
                         unsigned short src_width,
                         unsigned short src_height,
                         short dest_x, /* upper left offset in surface */
                         short dest_y,
                         unsigned short dest_width,
                         unsigned short dest_height,
                         /*
                          * whether to enable chroma-keying or global-alpha
                          * see VA_SUBPICTURE_XXX values
                          */
                         unsigned int flags)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_subpic *obj_subpic = SUBPIC(subpicture);
    int i;

    obj_subpic->src_rect.x      = src_x;
    obj_subpic->src_rect.y      = src_y;
    obj_subpic->src_rect.width  = src_width;
    obj_subpic->src_rect.height = src_height;
    obj_subpic->dst_rect.x      = dest_x;
    obj_subpic->dst_rect.y      = dest_y;
    obj_subpic->dst_rect.width  = dest_width;
    obj_subpic->dst_rect.height = dest_height;
    obj_subpic->flags           = flags;

    for (i = 0; i < num_surfaces; i++) {
        struct object_surface *obj_surface = SURFACE(target_surfaces[i]);
        if (!obj_surface)
            return VA_STATUS_ERROR_INVALID_SURFACE;
        obj_surface->subpic = subpicture;
    }
    return VA_STATUS_SUCCESS;
}


VAStatus 
i965_DeassociateSubpicture(VADriverContextP ctx,
                           VASubpictureID subpicture,
                           VASurfaceID *target_surfaces,
                           int num_surfaces)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    int i;

    for (i = 0; i < num_surfaces; i++) {
        struct object_surface *obj_surface = SURFACE(target_surfaces[i]);
        if (!obj_surface)
            return VA_STATUS_ERROR_INVALID_SURFACE;
        if (obj_surface->subpic == subpicture)
            obj_surface->subpic = VA_INVALID_ID;
    }
    return VA_STATUS_SUCCESS;
}

void
i965_reference_buffer_store(struct buffer_store **ptr, 
                            struct buffer_store *buffer_store)
{
    assert(*ptr == NULL);

    if (buffer_store) {
        buffer_store->ref_count++;
        *ptr = buffer_store;
    }
}

void 
i965_release_buffer_store(struct buffer_store **ptr)
{
    struct buffer_store *buffer_store = *ptr;

    if (buffer_store == NULL)
        return;

    assert(buffer_store->bo || buffer_store->buffer);
    assert(!(buffer_store->bo && buffer_store->buffer));
    buffer_store->ref_count--;
    
    if (buffer_store->ref_count == 0) {
        dri_bo_unreference(buffer_store->bo);
        free(buffer_store->buffer);
        buffer_store->bo = NULL;
        buffer_store->buffer = NULL;
        free(buffer_store);
    }

    *ptr = NULL;
}

static void 
i965_destroy_context(struct object_heap *heap, struct object_base *obj)
{
    struct object_context *obj_context = (struct object_context *)obj;
    int i;

    if (obj_context->hw_context) {
        obj_context->hw_context->destroy(obj_context->hw_context);
        obj_context->hw_context = NULL;
    }

    if (obj_context->codec_type == CODEC_PROC) {
        i965_release_buffer_store(&obj_context->codec_state.proc.pipeline_param);

        for (i = 0; i < VAProcFilterCount; i++)
            i965_release_buffer_store(&obj_context->codec_state.proc.filter_param[i]);
    } else if (obj_context->codec_type == CODEC_ENC) {
        assert(obj_context->codec_state.encode.num_slice_params <= obj_context->codec_state.encode.max_slice_params);
        i965_release_buffer_store(&obj_context->codec_state.encode.pic_param);
        i965_release_buffer_store(&obj_context->codec_state.encode.seq_param);

        for (i = 0; i < obj_context->codec_state.encode.num_slice_params; i++)
            i965_release_buffer_store(&obj_context->codec_state.encode.slice_params[i]);

        free(obj_context->codec_state.encode.slice_params);

        assert(obj_context->codec_state.encode.num_slice_params_ext <= obj_context->codec_state.encode.max_slice_params_ext);
        i965_release_buffer_store(&obj_context->codec_state.encode.pic_param_ext);
        i965_release_buffer_store(&obj_context->codec_state.encode.seq_param_ext);

        for (i = 0; i < ARRAY_ELEMS(obj_context->codec_state.encode.packed_header_param); i++)
            i965_release_buffer_store(&obj_context->codec_state.encode.packed_header_param[i]);

        for (i = 0; i < ARRAY_ELEMS(obj_context->codec_state.encode.packed_header_data); i++)
            i965_release_buffer_store(&obj_context->codec_state.encode.packed_header_data[i]);

        for (i = 0; i < obj_context->codec_state.encode.num_slice_params_ext; i++)
            i965_release_buffer_store(&obj_context->codec_state.encode.slice_params_ext[i]);

        free(obj_context->codec_state.encode.slice_params_ext);
    } else {
        assert(obj_context->codec_state.decode.num_slice_params <= obj_context->codec_state.decode.max_slice_params);
        assert(obj_context->codec_state.decode.num_slice_datas <= obj_context->codec_state.decode.max_slice_datas);

        i965_release_buffer_store(&obj_context->codec_state.decode.pic_param);
        i965_release_buffer_store(&obj_context->codec_state.decode.iq_matrix);
        i965_release_buffer_store(&obj_context->codec_state.decode.bit_plane);

        for (i = 0; i < obj_context->codec_state.decode.num_slice_params; i++)
            i965_release_buffer_store(&obj_context->codec_state.decode.slice_params[i]);

        for (i = 0; i < obj_context->codec_state.decode.num_slice_datas; i++)
            i965_release_buffer_store(&obj_context->codec_state.decode.slice_datas[i]);

        free(obj_context->codec_state.decode.slice_params);
        free(obj_context->codec_state.decode.slice_datas);
    }

    free(obj_context->render_targets);
    object_heap_free(heap, obj);
}

VAStatus
i965_CreateContext(VADriverContextP ctx,
                   VAConfigID config_id,
                   int picture_width,
                   int picture_height,
                   int flag,
                   VASurfaceID *render_targets,
                   int num_render_targets,
                   VAContextID *context)                /* out */
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct i965_render_state *render_state = &i965->render_state;
    struct object_config *obj_config = CONFIG(config_id);
    struct object_context *obj_context = NULL;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int contextID;
    int i;

    if (NULL == obj_config) {
        vaStatus = VA_STATUS_ERROR_INVALID_CONFIG;
        return vaStatus;
    }

    /* Validate flag */
    /* Validate picture dimensions */
    contextID = NEW_CONTEXT_ID();
    obj_context = CONTEXT(contextID);

    if (NULL == obj_context) {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        return vaStatus;
    }

    render_state->inited = 1;

    switch (obj_config->profile) {
    case VAProfileH264Baseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        if (!HAS_H264(i965))
            return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        render_state->interleaved_uv = 1;
        break;
    default:
        render_state->interleaved_uv = !!(IS_GEN6(i965->intel.device_id) || IS_GEN7(i965->intel.device_id));
        break;
    }

    *context = contextID;
    obj_context->flags = flag;
    obj_context->context_id = contextID;
    obj_context->config_id = config_id;
    obj_context->picture_width = picture_width;
    obj_context->picture_height = picture_height;
    obj_context->num_render_targets = num_render_targets;
    obj_context->render_targets = 
        (VASurfaceID *)calloc(num_render_targets, sizeof(VASurfaceID));
    obj_context->hw_context = NULL;

    for(i = 0; i < num_render_targets; i++) {
        if (NULL == SURFACE(render_targets[i])) {
            vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
            break;
        }

        obj_context->render_targets[i] = render_targets[i];
    }

    if (VA_STATUS_SUCCESS == vaStatus) {
        if (VAEntrypointVideoProc == obj_config->entrypoint) {
            obj_context->codec_type = CODEC_PROC;
            memset(&obj_context->codec_state.proc, 0, sizeof(obj_context->codec_state.proc));
            obj_context->codec_state.proc.current_render_target = VA_INVALID_ID;
            assert(i965->codec_info->proc_hw_context_init);
            obj_context->hw_context = i965->codec_info->proc_hw_context_init(ctx, obj_config);
        } else if (VAEntrypointEncSlice == obj_config->entrypoint) { /*encode routin only*/
            obj_context->codec_type = CODEC_ENC;
            memset(&obj_context->codec_state.encode, 0, sizeof(obj_context->codec_state.encode));
            obj_context->codec_state.encode.current_render_target = VA_INVALID_ID;
            obj_context->codec_state.encode.max_slice_params = NUM_SLICES;
            obj_context->codec_state.encode.slice_params = calloc(obj_context->codec_state.encode.max_slice_params,
                                                               sizeof(*obj_context->codec_state.encode.slice_params));
            assert(i965->codec_info->enc_hw_context_init);
            obj_context->hw_context = i965->codec_info->enc_hw_context_init(ctx, obj_config);
        } else {
            obj_context->codec_type = CODEC_DEC;
            memset(&obj_context->codec_state.decode, 0, sizeof(obj_context->codec_state.decode));
            obj_context->codec_state.decode.current_render_target = -1;
            obj_context->codec_state.decode.max_slice_params = NUM_SLICES;
            obj_context->codec_state.decode.max_slice_datas = NUM_SLICES;
            obj_context->codec_state.decode.slice_params = calloc(obj_context->codec_state.decode.max_slice_params,
                                                               sizeof(*obj_context->codec_state.decode.slice_params));
            obj_context->codec_state.decode.slice_datas = calloc(obj_context->codec_state.decode.max_slice_datas,
                                                              sizeof(*obj_context->codec_state.decode.slice_datas));

            assert(i965->codec_info->dec_hw_context_init);
            obj_context->hw_context = i965->codec_info->dec_hw_context_init(ctx, obj_config);
        }
    }

    /* Error recovery */
    if (VA_STATUS_SUCCESS != vaStatus) {
        i965_destroy_context(&i965->context_heap, (struct object_base *)obj_context);
    }

    i965->current_context_id = contextID;

    return vaStatus;
}

VAStatus 
i965_DestroyContext(VADriverContextP ctx, VAContextID context)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_context *obj_context = CONTEXT(context);

    assert(obj_context);

    if (i965->current_context_id == context)
        i965->current_context_id = VA_INVALID_ID;

    i965_destroy_context(&i965->context_heap, (struct object_base *)obj_context);

    return VA_STATUS_SUCCESS;
}

static void 
i965_destroy_buffer(struct object_heap *heap, struct object_base *obj)
{
    struct object_buffer *obj_buffer = (struct object_buffer *)obj;

    assert(obj_buffer->buffer_store);
    i965_release_buffer_store(&obj_buffer->buffer_store);
    object_heap_free(heap, obj);
}

static VAStatus
i965_create_buffer_internal(VADriverContextP ctx,
                            VAContextID context,
                            VABufferType type,
                            unsigned int size,
                            unsigned int num_elements,
                            void *data,
                            dri_bo *store_bo,
                            VABufferID *buf_id)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_buffer *obj_buffer = NULL;
    struct buffer_store *buffer_store = NULL;
    int bufferID;

    /* Validate type */
    switch (type) {
    case VAPictureParameterBufferType:
    case VAIQMatrixBufferType:
    case VAQMatrixBufferType:
    case VABitPlaneBufferType:
    case VASliceGroupMapBufferType:
    case VASliceParameterBufferType:
    case VASliceDataBufferType:
    case VAMacroblockParameterBufferType:
    case VAResidualDataBufferType:
    case VADeblockingParameterBufferType:
    case VAImageBufferType:
    case VAEncCodedBufferType:
    case VAEncSequenceParameterBufferType:
    case VAEncPictureParameterBufferType:
    case VAEncSliceParameterBufferType:
    case VAEncPackedHeaderParameterBufferType:
    case VAEncPackedHeaderDataBufferType:
    case VAProcPipelineParameterBufferType:
    case VAProcFilterParameterBufferType:
#ifdef HAVE_JPEG_DECODING
    case VAHuffmanTableBufferType:
#endif
        /* Ok */
        break;

    default:
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }

    bufferID = NEW_BUFFER_ID();
    obj_buffer = BUFFER(bufferID);

    if (NULL == obj_buffer) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (type == VAEncCodedBufferType) {
        size += ALIGN(sizeof(VACodedBufferSegment), 64);
        size += 0x1000; /* for upper bound check */
    }

    obj_buffer->max_num_elements = num_elements;
    obj_buffer->num_elements = num_elements;
    obj_buffer->size_element = size;
    obj_buffer->type = type;
    obj_buffer->buffer_store = NULL;
    buffer_store = calloc(1, sizeof(struct buffer_store));
    assert(buffer_store);
    buffer_store->ref_count = 1;

    if (store_bo != NULL) {
        buffer_store->bo = store_bo;
        dri_bo_reference(buffer_store->bo);
        
        if (data)
            dri_bo_subdata(buffer_store->bo, 0, size * num_elements, data);
    } else if (type == VASliceDataBufferType || 
               type == VAImageBufferType || 
               type == VAEncCodedBufferType) {
        buffer_store->bo = dri_bo_alloc(i965->intel.bufmgr, 
                                        "Buffer", 
                                        size * num_elements, 64);
        assert(buffer_store->bo);

        if (type == VAEncCodedBufferType) {
            VACodedBufferSegment *coded_buffer_segment;
            dri_bo_map(buffer_store->bo, 1);
            coded_buffer_segment = (VACodedBufferSegment *)buffer_store->bo->virtual;
            coded_buffer_segment->size = size - ALIGN(sizeof(VACodedBufferSegment), 64);
            coded_buffer_segment->bit_offset = 0;
            coded_buffer_segment->status = 0;
            coded_buffer_segment->buf = NULL;
            coded_buffer_segment->next = NULL;
            dri_bo_unmap(buffer_store->bo);
        } else if (data) {
            dri_bo_subdata(buffer_store->bo, 0, size * num_elements, data);
        }

    } else if (type == VAEncPackedHeaderParameterBufferType) {
        VAEncPackedHeaderParameterBuffer *param;

        assert(data);
        assert(num_elements == 1);
        assert(size == sizeof(*param));

        param = (VAEncPackedHeaderParameterBuffer *)data;
        memcpy(buffer_store->buffer,
               data, 
               size);
    } else {
        int msize = size;
        
        if (type == VAEncPackedHeaderDataBufferType) {
            msize = ALIGN(size, 4);
        }

        buffer_store->buffer = malloc(msize * num_elements);
        assert(buffer_store->buffer);

        if (data)
            memcpy(buffer_store->buffer, data, size * num_elements);
    }

    buffer_store->num_elements = obj_buffer->num_elements;
    i965_reference_buffer_store(&obj_buffer->buffer_store, buffer_store);
    i965_release_buffer_store(&buffer_store);
    *buf_id = bufferID;

    return VA_STATUS_SUCCESS;
}

VAStatus 
i965_CreateBuffer(VADriverContextP ctx,
                  VAContextID context,          /* in */
                  VABufferType type,            /* in */
                  unsigned int size,            /* in */
                  unsigned int num_elements,    /* in */
                  void *data,                   /* in */
                  VABufferID *buf_id)           /* out */
{
    return i965_create_buffer_internal(ctx, context, type, size, num_elements, data, NULL, buf_id);
}


VAStatus 
i965_BufferSetNumElements(VADriverContextP ctx,
                          VABufferID buf_id,           /* in */
                          unsigned int num_elements)   /* in */
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_buffer *obj_buffer = BUFFER(buf_id);
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    assert(obj_buffer);

    if ((num_elements < 0) || 
        (num_elements > obj_buffer->max_num_elements)) {
        vaStatus = VA_STATUS_ERROR_UNKNOWN;
    } else {
        obj_buffer->num_elements = num_elements;
        if (obj_buffer->buffer_store != NULL) {
            obj_buffer->buffer_store->num_elements = num_elements;
        }
    }

    return vaStatus;
}

VAStatus 
i965_MapBuffer(VADriverContextP ctx,
               VABufferID buf_id,       /* in */
               void **pbuf)             /* out */
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_buffer *obj_buffer = BUFFER(buf_id);
    VAStatus vaStatus = VA_STATUS_ERROR_UNKNOWN;

    assert(obj_buffer && obj_buffer->buffer_store);
    assert(obj_buffer->buffer_store->bo || obj_buffer->buffer_store->buffer);
    assert(!(obj_buffer->buffer_store->bo && obj_buffer->buffer_store->buffer));

    if (NULL != obj_buffer->buffer_store->bo) {
        unsigned int tiling, swizzle;

        dri_bo_get_tiling(obj_buffer->buffer_store->bo, &tiling, &swizzle);

        if (tiling != I915_TILING_NONE)
            drm_intel_gem_bo_map_gtt(obj_buffer->buffer_store->bo);
        else
            dri_bo_map(obj_buffer->buffer_store->bo, 1);

        assert(obj_buffer->buffer_store->bo->virtual);
        *pbuf = obj_buffer->buffer_store->bo->virtual;

        if (obj_buffer->type == VAEncCodedBufferType) {
            int i;
            unsigned char *buffer = NULL;
            VACodedBufferSegment *coded_buffer_segment = (VACodedBufferSegment *)(obj_buffer->buffer_store->bo->virtual);

            coded_buffer_segment->buf = buffer = (unsigned char *)(obj_buffer->buffer_store->bo->virtual) + ALIGN(sizeof(VACodedBufferSegment), 64);
            
            for (i = 0; i < obj_buffer->size_element - ALIGN(sizeof(VACodedBufferSegment), 64) - 3 - 0x1000; i++) {
                if (!buffer[i] &&
                    !buffer[i + 1] &&
                    !buffer[i + 2] &&
                    !buffer[i + 3])
                    break;
            }

            if (i == obj_buffer->size_element - ALIGN(sizeof(VACodedBufferSegment), 64) - 3 - 0x1000) {
                coded_buffer_segment->status |= VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK;
            }

            coded_buffer_segment->size = i;
        }

        vaStatus = VA_STATUS_SUCCESS;
    } else if (NULL != obj_buffer->buffer_store->buffer) {
        *pbuf = obj_buffer->buffer_store->buffer;
        vaStatus = VA_STATUS_SUCCESS;
    }

    return vaStatus;
}

VAStatus 
i965_UnmapBuffer(VADriverContextP ctx, VABufferID buf_id)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_buffer *obj_buffer = BUFFER(buf_id);
    VAStatus vaStatus = VA_STATUS_ERROR_UNKNOWN;

    assert(obj_buffer && obj_buffer->buffer_store);
    assert(obj_buffer->buffer_store->bo || obj_buffer->buffer_store->buffer);
    assert(!(obj_buffer->buffer_store->bo && obj_buffer->buffer_store->buffer));

    if (NULL != obj_buffer->buffer_store->bo) {
        unsigned int tiling, swizzle;

        dri_bo_get_tiling(obj_buffer->buffer_store->bo, &tiling, &swizzle);

        if (tiling != I915_TILING_NONE)
            drm_intel_gem_bo_unmap_gtt(obj_buffer->buffer_store->bo);
        else
            dri_bo_unmap(obj_buffer->buffer_store->bo);

        vaStatus = VA_STATUS_SUCCESS;
    } else if (NULL != obj_buffer->buffer_store->buffer) {
        /* Do nothing */
        vaStatus = VA_STATUS_SUCCESS;
    }

    return vaStatus;    
}

VAStatus 
i965_DestroyBuffer(VADriverContextP ctx, VABufferID buffer_id)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_buffer *obj_buffer = BUFFER(buffer_id);

    assert(obj_buffer);
    i965_destroy_buffer(&i965->buffer_heap, (struct object_base *)obj_buffer);

    return VA_STATUS_SUCCESS;
}

VAStatus 
i965_BeginPicture(VADriverContextP ctx,
                  VAContextID context,
                  VASurfaceID render_target)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx); 
    struct object_context *obj_context = CONTEXT(context);
    struct object_surface *obj_surface = SURFACE(render_target);
    struct object_config *obj_config;
    VAContextID config;
    VAStatus vaStatus;
    int i;

    assert(obj_context);
    assert(obj_surface);

    config = obj_context->config_id;
    obj_config = CONFIG(config);
    assert(obj_config);

    switch (obj_config->profile) {
    case VAProfileMPEG2Simple:
    case VAProfileMPEG2Main:
        vaStatus = VA_STATUS_SUCCESS;
        break;

    case VAProfileH264Baseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        vaStatus = VA_STATUS_SUCCESS;
        break;

    case VAProfileVC1Simple:
    case VAProfileVC1Main:
    case VAProfileVC1Advanced:
        vaStatus = VA_STATUS_SUCCESS;
        break;

    case VAProfileJPEGBaseline:
        vaStatus = VA_STATUS_SUCCESS;
        break;

    case VAProfileNone:
        vaStatus = VA_STATUS_SUCCESS;
        break;

    default:
        assert(0);
        vaStatus = VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        break;
    }

    if (obj_context->codec_type == CODEC_PROC) {
        obj_context->codec_state.proc.current_render_target = render_target;
    } else if (obj_context->codec_type == CODEC_ENC) {
        i965_release_buffer_store(&obj_context->codec_state.encode.pic_param);
        i965_release_buffer_store(&obj_context->codec_state.encode.seq_param);

        for (i = 0; i < obj_context->codec_state.encode.num_slice_params; i++) {
            i965_release_buffer_store(&obj_context->codec_state.encode.slice_params[i]);
        }

        obj_context->codec_state.encode.num_slice_params = 0;

        /* ext */
        i965_release_buffer_store(&obj_context->codec_state.encode.pic_param_ext);
        i965_release_buffer_store(&obj_context->codec_state.encode.seq_param_ext);

        for (i = 0; i < ARRAY_ELEMS(obj_context->codec_state.encode.packed_header_param); i++)
            i965_release_buffer_store(&obj_context->codec_state.encode.packed_header_param[i]);

        for (i = 0; i < ARRAY_ELEMS(obj_context->codec_state.encode.packed_header_data); i++)
            i965_release_buffer_store(&obj_context->codec_state.encode.packed_header_data[i]);

        for (i = 0; i < obj_context->codec_state.encode.num_slice_params_ext; i++)
            i965_release_buffer_store(&obj_context->codec_state.encode.slice_params_ext[i]);

        obj_context->codec_state.encode.num_slice_params_ext = 0;
        obj_context->codec_state.encode.current_render_target = render_target;     /*This is input new frame*/
        obj_context->codec_state.encode.last_packed_header_type = 0;
    } else {
        obj_context->codec_state.decode.current_render_target = render_target;
        i965_release_buffer_store(&obj_context->codec_state.decode.pic_param);
        i965_release_buffer_store(&obj_context->codec_state.decode.iq_matrix);
        i965_release_buffer_store(&obj_context->codec_state.decode.bit_plane);
        i965_release_buffer_store(&obj_context->codec_state.decode.huffman_table);

        for (i = 0; i < obj_context->codec_state.decode.num_slice_params; i++) {
            i965_release_buffer_store(&obj_context->codec_state.decode.slice_params[i]);
            i965_release_buffer_store(&obj_context->codec_state.decode.slice_datas[i]);
        }

        obj_context->codec_state.decode.num_slice_params = 0;
        obj_context->codec_state.decode.num_slice_datas = 0;
    }

    return vaStatus;
}

#define I965_RENDER_BUFFER(category, name) i965_render_##category##_##name##_buffer(ctx, obj_context, obj_buffer)

#define DEF_RENDER_SINGLE_BUFFER_FUNC(category, name, member)           \
    static VAStatus                                                     \
    i965_render_##category##_##name##_buffer(VADriverContextP ctx,      \
                                             struct object_context *obj_context, \
                                             struct object_buffer *obj_buffer) \
    {                                                                   \
        struct category##_state *category = &obj_context->codec_state.category; \
        assert(obj_buffer->buffer_store->bo == NULL);                   \
        assert(obj_buffer->buffer_store->buffer);                       \
        i965_release_buffer_store(&category->member);                   \
        i965_reference_buffer_store(&category->member, obj_buffer->buffer_store); \
        return VA_STATUS_SUCCESS;                                       \
    }

#define DEF_RENDER_MULTI_BUFFER_FUNC(category, name, member)            \
    static VAStatus                                                     \
    i965_render_##category##_##name##_buffer(VADriverContextP ctx,      \
                                             struct object_context *obj_context, \
                                             struct object_buffer *obj_buffer) \
    {                                                                   \
        struct category##_state *category = &obj_context->codec_state.category; \
        if (category->num_##member == category->max_##member) {         \
            category->member = realloc(category->member, (category->max_##member + NUM_SLICES) * sizeof(*category->member)); \
            memset(category->member + category->max_##member, 0, NUM_SLICES * sizeof(*category->member)); \
            category->max_##member += NUM_SLICES;                       \
        }                                                               \
        i965_release_buffer_store(&category->member[category->num_##member]); \
        i965_reference_buffer_store(&category->member[category->num_##member], obj_buffer->buffer_store); \
        category->num_##member++;                                       \
        return VA_STATUS_SUCCESS;                                       \
    }

#define I965_RENDER_DECODE_BUFFER(name) I965_RENDER_BUFFER(decode, name)

#define DEF_RENDER_DECODE_SINGLE_BUFFER_FUNC(name, member) DEF_RENDER_SINGLE_BUFFER_FUNC(decode, name, member)
DEF_RENDER_DECODE_SINGLE_BUFFER_FUNC(picture_parameter, pic_param)
DEF_RENDER_DECODE_SINGLE_BUFFER_FUNC(iq_matrix, iq_matrix)
DEF_RENDER_DECODE_SINGLE_BUFFER_FUNC(bit_plane, bit_plane)
DEF_RENDER_DECODE_SINGLE_BUFFER_FUNC(huffman_table, huffman_table)

#define DEF_RENDER_DECODE_MULTI_BUFFER_FUNC(name, member) DEF_RENDER_MULTI_BUFFER_FUNC(decode, name, member)
DEF_RENDER_DECODE_MULTI_BUFFER_FUNC(slice_parameter, slice_params)
DEF_RENDER_DECODE_MULTI_BUFFER_FUNC(slice_data, slice_datas)

static VAStatus 
i965_decoder_render_picture(VADriverContextP ctx,
                            VAContextID context,
                            VABufferID *buffers,
                            int num_buffers)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx); 
    struct object_context *obj_context = CONTEXT(context);
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int i;

    for (i = 0; i < num_buffers && vaStatus == VA_STATUS_SUCCESS; i++) {
        struct object_buffer *obj_buffer = BUFFER(buffers[i]);
        assert(obj_buffer);

        switch (obj_buffer->type) {
        case VAPictureParameterBufferType:
            vaStatus = I965_RENDER_DECODE_BUFFER(picture_parameter);
            break;
            
        case VAIQMatrixBufferType:
            vaStatus = I965_RENDER_DECODE_BUFFER(iq_matrix);
            break;

        case VABitPlaneBufferType:
            vaStatus = I965_RENDER_DECODE_BUFFER(bit_plane);
            break;

        case VASliceParameterBufferType:
            vaStatus = I965_RENDER_DECODE_BUFFER(slice_parameter);
            break;

        case VASliceDataBufferType:
            vaStatus = I965_RENDER_DECODE_BUFFER(slice_data);
            break;

#ifdef HAVE_JPEG_DECODING
        case VAHuffmanTableBufferType:
            vaStatus = I965_RENDER_DECODE_BUFFER(huffman_table);
            break;
#endif

        default:
            vaStatus = VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
            break;
        }
    }

    return vaStatus;
}

#define I965_RENDER_ENCODE_BUFFER(name) I965_RENDER_BUFFER(encode, name)

#define DEF_RENDER_ENCODE_SINGLE_BUFFER_FUNC(name, member) DEF_RENDER_SINGLE_BUFFER_FUNC(encode, name, member)
DEF_RENDER_ENCODE_SINGLE_BUFFER_FUNC(sequence_parameter, seq_param)    
DEF_RENDER_ENCODE_SINGLE_BUFFER_FUNC(picture_parameter, pic_param)
DEF_RENDER_ENCODE_SINGLE_BUFFER_FUNC(picture_control, pic_control)
DEF_RENDER_ENCODE_SINGLE_BUFFER_FUNC(qmatrix, q_matrix)
DEF_RENDER_ENCODE_SINGLE_BUFFER_FUNC(iqmatrix, iq_matrix)
/* extended buffer */
DEF_RENDER_ENCODE_SINGLE_BUFFER_FUNC(sequence_parameter_ext, seq_param_ext)
DEF_RENDER_ENCODE_SINGLE_BUFFER_FUNC(picture_parameter_ext, pic_param_ext)

#define DEF_RENDER_ENCODE_MULTI_BUFFER_FUNC(name, member) DEF_RENDER_MULTI_BUFFER_FUNC(encode, name, member)
DEF_RENDER_ENCODE_MULTI_BUFFER_FUNC(slice_parameter, slice_params)
DEF_RENDER_ENCODE_MULTI_BUFFER_FUNC(slice_parameter_ext, slice_params_ext)

static VAStatus
i965_encoder_render_packed_header_parameter_buffer(VADriverContextP ctx,
                                                   struct object_context *obj_context,
                                                   struct object_buffer *obj_buffer,
                                                   VAEncPackedHeaderType type)
{
    struct encode_state *encode = &obj_context->codec_state.encode;

    assert(obj_buffer->buffer_store->bo == NULL);
    assert(obj_buffer->buffer_store->buffer);
    i965_release_buffer_store(&encode->packed_header_param[type]);
    i965_reference_buffer_store(&encode->packed_header_param[type], obj_buffer->buffer_store);

    return VA_STATUS_SUCCESS;
}

static VAStatus
i965_encoder_render_packed_header_data_buffer(VADriverContextP ctx,
                                              struct object_context *obj_context,
                                              struct object_buffer *obj_buffer,
                                              VAEncPackedHeaderType type)
{
    struct encode_state *encode = &obj_context->codec_state.encode;

    assert(obj_buffer->buffer_store->bo == NULL);
    assert(obj_buffer->buffer_store->buffer);
    i965_release_buffer_store(&encode->packed_header_data[type]);
    i965_reference_buffer_store(&encode->packed_header_data[type], obj_buffer->buffer_store);

    return VA_STATUS_SUCCESS;
}

static VAStatus 
i965_encoder_render_picture(VADriverContextP ctx,
                            VAContextID context,
                            VABufferID *buffers,
                            int num_buffers)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx); 
    struct object_context *obj_context = CONTEXT(context);
    VAStatus vaStatus = VA_STATUS_ERROR_UNKNOWN;
    int i;

    for (i = 0; i < num_buffers; i++) {  
        struct object_buffer *obj_buffer = BUFFER(buffers[i]);
        assert(obj_buffer);

        switch (obj_buffer->type) {
        case VAQMatrixBufferType:
            vaStatus = I965_RENDER_ENCODE_BUFFER(qmatrix);
            break;

        case VAIQMatrixBufferType:
            vaStatus = I965_RENDER_ENCODE_BUFFER(iqmatrix);
            break;

        case VAEncSequenceParameterBufferType:
            vaStatus = I965_RENDER_ENCODE_BUFFER(sequence_parameter_ext);
            break;

        case VAEncPictureParameterBufferType:
            vaStatus = I965_RENDER_ENCODE_BUFFER(picture_parameter_ext);
            break;

        case VAEncSliceParameterBufferType:
            vaStatus = I965_RENDER_ENCODE_BUFFER(slice_parameter_ext);
            break;

        case VAEncPackedHeaderParameterBufferType:
        {
            struct encode_state *encode = &obj_context->codec_state.encode;
            VAEncPackedHeaderParameterBuffer *param = (VAEncPackedHeaderParameterBuffer *)obj_buffer->buffer_store->buffer;
            encode->last_packed_header_type = param->type;

            vaStatus = i965_encoder_render_packed_header_parameter_buffer(ctx,
                                                                          obj_context,
                                                                          obj_buffer,
                                                                          encode->last_packed_header_type);
            break;
        }

        case VAEncPackedHeaderDataBufferType:
        {
            struct encode_state *encode = &obj_context->codec_state.encode;

            assert(encode->last_packed_header_type == VAEncPackedHeaderSequence ||
                   encode->last_packed_header_type == VAEncPackedHeaderPicture ||
                   encode->last_packed_header_type == VAEncPackedHeaderSlice);
            vaStatus = i965_encoder_render_packed_header_data_buffer(ctx, 
                                                                     obj_context,
                                                                     obj_buffer,
                                                                     encode->last_packed_header_type);
            break;       
        }

        default:
            vaStatus = VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
            break;
        }
    }	

    return vaStatus;
}

#define I965_RENDER_PROC_BUFFER(name) I965_RENDER_BUFFER(proc, name)

#define DEF_RENDER_PROC_SINGLE_BUFFER_FUNC(name, member) DEF_RENDER_SINGLE_BUFFER_FUNC(proc, name, member)
DEF_RENDER_PROC_SINGLE_BUFFER_FUNC(pipeline_parameter, pipeline_param)    

static VAStatus
i965_render_proc_filter_parameter_buffer(VADriverContextP ctx,
                                         struct object_context *obj_context,
                                         struct object_buffer *obj_buffer,
                                         VAProcFilterType type)
{
    struct proc_state *proc = &obj_context->codec_state.proc;

    assert(obj_buffer->buffer_store->bo == NULL);
    assert(obj_buffer->buffer_store->buffer);
    i965_release_buffer_store(&proc->filter_param[type]);
    i965_reference_buffer_store(&proc->filter_param[type], obj_buffer->buffer_store);

    return VA_STATUS_SUCCESS;
}

static VAStatus 
i965_proc_render_picture(VADriverContextP ctx,
                         VAContextID context,
                         VABufferID *buffers,
                         int num_buffers)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx); 
    struct object_context *obj_context = CONTEXT(context);
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int i;

    for (i = 0; i < num_buffers && vaStatus == VA_STATUS_SUCCESS; i++) {
        struct object_buffer *obj_buffer = BUFFER(buffers[i]);
        assert(obj_buffer);

        switch (obj_buffer->type) {
        case VAProcPipelineParameterBufferType:
            /* FIXME: */
            vaStatus = I965_RENDER_PROC_BUFFER(pipeline_parameter);
            break;

        case VAProcFilterParameterBufferType:
        {
            /* FIXME: */
            VAProcFilterParameterBuffer *param = (VAProcFilterParameterBuffer *)obj_buffer->buffer_store->buffer;
            vaStatus = i965_render_proc_filter_parameter_buffer(ctx, obj_context, obj_buffer, param->type);
            break;
        }

        default:
            vaStatus = VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
            break;
        }
    }

    return vaStatus;
}

VAStatus 
i965_RenderPicture(VADriverContextP ctx,
                   VAContextID context,
                   VABufferID *buffers,
                   int num_buffers)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_context *obj_context;
    struct object_config *obj_config;
    VAContextID config;
    VAStatus vaStatus = VA_STATUS_ERROR_UNKNOWN;

    obj_context = CONTEXT(context);
    assert(obj_context);

    config = obj_context->config_id;
    obj_config = CONFIG(config);
    assert(obj_config);

    if (VAEntrypointVideoProc == obj_config->entrypoint) {
        vaStatus = i965_proc_render_picture(ctx, context, buffers, num_buffers);
    } else if (VAEntrypointEncSlice == obj_config->entrypoint ) {
        vaStatus = i965_encoder_render_picture(ctx, context, buffers, num_buffers);
    } else {
        vaStatus = i965_decoder_render_picture(ctx, context, buffers, num_buffers);
    }

    return vaStatus;
}

VAStatus 
i965_EndPicture(VADriverContextP ctx, VAContextID context)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx); 
    struct object_context *obj_context = CONTEXT(context);
    struct object_config *obj_config;
    VAContextID config;

    assert(obj_context);
    config = obj_context->config_id;
    obj_config = CONFIG(config);
    assert(obj_config);

    if (obj_context->codec_type == CODEC_PROC) {
        assert(VAEntrypointVideoProc == obj_config->entrypoint);
    } else if (obj_context->codec_type == CODEC_ENC) {
        assert(VAEntrypointEncSlice == obj_config->entrypoint);

        assert(obj_context->codec_state.encode.pic_param ||
               obj_context->codec_state.encode.pic_param_ext);
        assert(obj_context->codec_state.encode.seq_param ||
               obj_context->codec_state.encode.seq_param_ext);
        assert(obj_context->codec_state.encode.num_slice_params >= 1 ||
               obj_context->codec_state.encode.num_slice_params_ext >= 1);
    } else {
        assert(obj_context->codec_state.decode.pic_param);
        assert(obj_context->codec_state.decode.num_slice_params >= 1);
        assert(obj_context->codec_state.decode.num_slice_datas >= 1);
        assert(obj_context->codec_state.decode.num_slice_params == obj_context->codec_state.decode.num_slice_datas);
    }

    assert(obj_context->hw_context->run);
    obj_context->hw_context->run(ctx, obj_config->profile, &obj_context->codec_state, obj_context->hw_context);

    return VA_STATUS_SUCCESS;
}

VAStatus 
i965_SyncSurface(VADriverContextP ctx,
                 VASurfaceID render_target)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx); 
    struct object_surface *obj_surface = SURFACE(render_target);

    assert(obj_surface);

    return VA_STATUS_SUCCESS;
}

VAStatus 
i965_QuerySurfaceStatus(VADriverContextP ctx,
                        VASurfaceID render_target,
                        VASurfaceStatus *status)        /* out */
{
    struct i965_driver_data *i965 = i965_driver_data(ctx); 
    struct object_surface *obj_surface = SURFACE(render_target);

    assert(obj_surface);

    /* Usually GEM will handle synchronization with the graphics hardware */
#if 0
    if (obj_surface->bo) {
        dri_bo_map(obj_surface->bo, 0);
        dri_bo_unmap(obj_surface->bo);
    }
#endif
    
    *status = obj_surface->status;

    return VA_STATUS_SUCCESS;
}


/* 
 * Query display attributes 
 * The caller must provide a "attr_list" array that can hold at
 * least vaMaxNumDisplayAttributes() entries. The actual number of attributes
 * returned in "attr_list" is returned in "num_attributes".
 */
VAStatus 
i965_QueryDisplayAttributes(VADriverContextP ctx,
                            VADisplayAttribute *attr_list,    /* out */
                            int *num_attributes)              /* out */
{
    if (num_attributes)
        *num_attributes = 0;

    return VA_STATUS_SUCCESS;
}

/* 
 * Get display attributes 
 * This function returns the current attribute values in "attr_list".
 * Only attributes returned with VA_DISPLAY_ATTRIB_GETTABLE set in the "flags" field
 * from vaQueryDisplayAttributes() can have their values retrieved.  
 */
VAStatus 
i965_GetDisplayAttributes(VADriverContextP ctx,
                          VADisplayAttribute *attr_list,    /* in/out */
                          int num_attributes)
{
    /* TODO */
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* 
 * Set display attributes 
 * Only attributes returned with VA_DISPLAY_ATTRIB_SETTABLE set in the "flags" field
 * from vaQueryDisplayAttributes() can be set.  If the attribute is not settable or 
 * the value is out of range, the function returns VA_STATUS_ERROR_ATTR_NOT_SUPPORTED
 */
VAStatus 
i965_SetDisplayAttributes(VADriverContextP ctx,
                          VADisplayAttribute *attr_list,
                          int num_attributes)
{
    /* TODO */
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus 
i965_DbgCopySurfaceToBuffer(VADriverContextP ctx,
                            VASurfaceID surface,
                            void **buffer,              /* out */
                            unsigned int *stride)       /* out */
{
    /* TODO */
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus 
i965_Init(VADriverContextP ctx)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx); 

    if (intel_driver_init(ctx) == False)
        return VA_STATUS_ERROR_UNKNOWN;

    if (IS_G4X(i965->intel.device_id))
        i965->codec_info = &g4x_hw_codec_info;
    else if (IS_IRONLAKE(i965->intel.device_id))
        i965->codec_info = &ironlake_hw_codec_info;
    else if (IS_GEN6(i965->intel.device_id))
        i965->codec_info = &gen6_hw_codec_info;
    else if (IS_GEN7(i965->intel.device_id))
        i965->codec_info = &gen7_hw_codec_info;
    else
        return VA_STATUS_ERROR_UNKNOWN;

    i965->batch = intel_batchbuffer_new(&i965->intel, I915_EXEC_RENDER);

    if (i965_post_processing_init(ctx) == False)
        return VA_STATUS_ERROR_UNKNOWN;

    if (i965_render_init(ctx) == False)
        return VA_STATUS_ERROR_UNKNOWN;

    _i965InitMutex(&i965->render_mutex);

    return VA_STATUS_SUCCESS;
}

static void
i965_destroy_heap(struct object_heap *heap, 
                  void (*func)(struct object_heap *heap, struct object_base *object))
{
    struct object_base *object;
    object_heap_iterator iter;    

    object = object_heap_first(heap, &iter);

    while (object) {
        if (func)
            func(heap, object);

        object = object_heap_next(heap, &iter);
    }

    object_heap_destroy(heap);
}


VAStatus 
i965_DestroyImage(VADriverContextP ctx, VAImageID image);

VAStatus 
i965_CreateImage(VADriverContextP ctx,
                 VAImageFormat *format,
                 int width,
                 int height,
                 VAImage *out_image)        /* out */
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_image *obj_image;
    VAStatus va_status = VA_STATUS_ERROR_OPERATION_FAILED;
    VAImageID image_id;
    unsigned int width2, height2, size2, size;

    out_image->image_id = VA_INVALID_ID;
    out_image->buf      = VA_INVALID_ID;

    image_id = NEW_IMAGE_ID();
    if (image_id == VA_INVALID_ID)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    obj_image = IMAGE(image_id);
    if (!obj_image)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    obj_image->bo         = NULL;
    obj_image->palette    = NULL;
    obj_image->derived_surface = VA_INVALID_ID;

    VAImage * const image = &obj_image->image;
    image->image_id       = image_id;
    image->buf            = VA_INVALID_ID;

    size    = width * height;
    width2  = (width  + 1) / 2;
    height2 = (height + 1) / 2;
    size2   = width2 * height2;

    image->num_palette_entries = 0;
    image->entry_bytes         = 0;
    memset(image->component_order, 0, sizeof(image->component_order));

    switch (format->fourcc) {
    case VA_FOURCC('I','A','4','4'):
    case VA_FOURCC('A','I','4','4'):
        image->num_planes = 1;
        image->pitches[0] = width;
        image->offsets[0] = 0;
        image->data_size  = image->offsets[0] + image->pitches[0] * height;
        image->num_palette_entries = 16;
        image->entry_bytes         = 3;
        image->component_order[0]  = 'R';
        image->component_order[1]  = 'G';
        image->component_order[2]  = 'B';
        break;
    case VA_FOURCC('A','R','G','B'):
    case VA_FOURCC('A','B','G','R'):
    case VA_FOURCC('B','G','R','A'):
    case VA_FOURCC('R','G','B','A'):
        image->num_planes = 1;
        image->pitches[0] = width * 4;
        image->offsets[0] = 0;
        image->data_size  = image->offsets[0] + image->pitches[0] * height;
        break;
    case VA_FOURCC('Y','V','1','2'):
        image->num_planes = 3;
        image->pitches[0] = width;
        image->offsets[0] = 0;
        image->pitches[1] = width2;
        image->offsets[1] = size + size2;
        image->pitches[2] = width2;
        image->offsets[2] = size;
        image->data_size  = size + 2 * size2;
        break;
    case VA_FOURCC('I','4','2','0'):
        image->num_planes = 3;
        image->pitches[0] = width;
        image->offsets[0] = 0;
        image->pitches[1] = width2;
        image->offsets[1] = size;
        image->pitches[2] = width2;
        image->offsets[2] = size + size2;
        image->data_size  = size + 2 * size2;
        break;
    case VA_FOURCC('N','V','1','2'):
        image->num_planes = 2;
        image->pitches[0] = width;
        image->offsets[0] = 0;
        image->pitches[1] = width;
        image->offsets[1] = size;
        image->data_size  = size + 2 * size2;
        break;
    default:
        goto error;
    }

    va_status = i965_CreateBuffer(ctx, 0, VAImageBufferType,
                                  image->data_size, 1, NULL, &image->buf);
    if (va_status != VA_STATUS_SUCCESS)
        goto error;

    obj_image->bo = BUFFER(image->buf)->buffer_store->bo;
    dri_bo_reference(obj_image->bo);

    if (image->num_palette_entries > 0 && image->entry_bytes > 0) {
        obj_image->palette = malloc(image->num_palette_entries * sizeof(obj_image->palette));
        if (!obj_image->palette)
            goto error;
    }

    image->image_id             = image_id;
    image->format               = *format;
    image->width                = width;
    image->height               = height;

    *out_image                  = *image;
    return VA_STATUS_SUCCESS;

 error:
    i965_DestroyImage(ctx, image_id);
    return va_status;
}

void 
i965_check_alloc_surface_bo(VADriverContextP ctx,
                            struct object_surface *obj_surface,
                            int tiled,
                            unsigned int fourcc,
                            unsigned int subsampling)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    int region_width, region_height;

    if (obj_surface->bo) {
        assert(obj_surface->fourcc);
        assert(obj_surface->fourcc == fourcc);
        assert(obj_surface->subsampling == subsampling);
        return;
    }

    obj_surface->x_cb_offset = 0; /* X offset is always 0 */
    obj_surface->x_cr_offset = 0;

    if (tiled) {
        assert(fourcc == VA_FOURCC('N', 'V', '1', '2') ||
               fourcc == VA_FOURCC('I', 'M', 'C', '1') ||
               fourcc == VA_FOURCC('I', 'M', 'C', '3'));

        obj_surface->width = ALIGN(obj_surface->orig_width, 128);
        obj_surface->height = ALIGN(obj_surface->orig_height, 32);
        obj_surface->cb_cr_pitch = obj_surface->width;
        region_width = obj_surface->width;
        region_height = obj_surface->height;

        if (fourcc == VA_FOURCC('N', 'V', '1', '2')) {
            assert(subsampling == SUBSAMPLE_YUV420);
            obj_surface->y_cb_offset = obj_surface->height;
            obj_surface->y_cr_offset = obj_surface->height;
            obj_surface->cb_cr_width = obj_surface->orig_width / 2;
            obj_surface->cb_cr_height = obj_surface->orig_height / 2;
            region_height = obj_surface->height + ALIGN(obj_surface->cb_cr_height, 32);
        } else if (fourcc == VA_FOURCC('I', 'M', 'C', '1') ||
                   fourcc == VA_FOURCC('I', 'M', 'C', '3')) {
            switch (subsampling) {
            case SUBSAMPLE_YUV400:
                obj_surface->cb_cr_width = 0;
                obj_surface->cb_cr_height = 0;
                break;

            case SUBSAMPLE_YUV420:
                obj_surface->cb_cr_width = obj_surface->orig_width / 2;
                obj_surface->cb_cr_height = obj_surface->orig_height / 2;
                break;

            case SUBSAMPLE_YUV422H:
                obj_surface->cb_cr_width = obj_surface->orig_width / 2;
                obj_surface->cb_cr_height = obj_surface->orig_height;
                break;

            case SUBSAMPLE_YUV422V:
                obj_surface->cb_cr_width = obj_surface->orig_width;
                obj_surface->cb_cr_height = obj_surface->orig_height / 2;
                break;

            case SUBSAMPLE_YUV444:
                obj_surface->cb_cr_width = obj_surface->orig_width;
                obj_surface->cb_cr_height = obj_surface->orig_height;
                break;

            case SUBSAMPLE_YUV411:
                obj_surface->cb_cr_width = obj_surface->orig_width / 4;
                obj_surface->cb_cr_height = obj_surface->orig_height;
                break;

            default:
                assert(0);
                break;
            }

            region_height = obj_surface->height + ALIGN(obj_surface->cb_cr_height, 32) * 2;

            if (fourcc == VA_FOURCC('I', 'M', 'C', '1')) {
                obj_surface->y_cr_offset = obj_surface->height;
                obj_surface->y_cb_offset = obj_surface->y_cr_offset + ALIGN(obj_surface->cb_cr_height, 32);
            } else {
                obj_surface->y_cb_offset = obj_surface->height;
                obj_surface->y_cr_offset = obj_surface->y_cb_offset + ALIGN(obj_surface->cb_cr_height, 32);
            }
        }
    } else {
        assert(fourcc != VA_FOURCC('I', 'M', 'C', '1') &&
               fourcc != VA_FOURCC('I', 'M', 'C', '3'));
        assert(subsampling == SUBSAMPLE_YUV420);

        region_width = obj_surface->width;
        region_height = obj_surface->height;

        switch (fourcc) {
        case VA_FOURCC('N', 'V', '1', '2'):
            obj_surface->y_cb_offset = obj_surface->height;
            obj_surface->y_cr_offset = obj_surface->height;
            obj_surface->cb_cr_width = obj_surface->orig_width / 2;
            obj_surface->cb_cr_height = obj_surface->orig_height / 2;
            obj_surface->cb_cr_pitch = obj_surface->width;
            region_height = obj_surface->height + obj_surface->height / 2;
            break;

        case VA_FOURCC('Y', 'V', '1', '2'):
        case VA_FOURCC('I', '4', '2', '0'):
            if (fourcc == VA_FOURCC('Y', 'V', '1', '2')) {
                obj_surface->y_cr_offset = obj_surface->height;
                obj_surface->y_cb_offset = obj_surface->height + obj_surface->height / 4;
            } else {
                obj_surface->y_cb_offset = obj_surface->height;
                obj_surface->y_cr_offset = obj_surface->height + obj_surface->height / 4;
            }

            obj_surface->cb_cr_width = obj_surface->orig_width / 2;
            obj_surface->cb_cr_height = obj_surface->orig_height / 2;
            obj_surface->cb_cr_pitch = obj_surface->width / 2;
            region_height = obj_surface->height + obj_surface->height / 2;
            break;

        default:
            assert(0);
            break;
        }
    }

    obj_surface->size = ALIGN(region_width * region_height, 0x1000);

    if (tiled) {
        uint32_t tiling_mode = I915_TILING_Y; /* always uses Y-tiled format */
        unsigned long pitch;

        obj_surface->bo = drm_intel_bo_alloc_tiled(i965->intel.bufmgr, 
                                                   "vaapi surface",
                                                   region_width,
                                                   region_height,
                                                   1,
                                                   &tiling_mode,
                                                   &pitch,
                                                   0);
        assert(tiling_mode == I915_TILING_Y);
        assert(pitch == obj_surface->width);
    } else {
        obj_surface->bo = dri_bo_alloc(i965->intel.bufmgr,
                                       "vaapi surface",
                                       obj_surface->size,
                                       0x1000);
    }

    obj_surface->fourcc = fourcc;
    obj_surface->subsampling = subsampling;
    assert(obj_surface->bo);
}

VAStatus i965_DeriveImage(VADriverContextP ctx,
                          VASurfaceID surface,
                          VAImage *out_image)        /* out */
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_image *obj_image;
    struct object_surface *obj_surface; 
    VAImageID image_id;
    unsigned int w_pitch, h_pitch;
    VAStatus va_status = VA_STATUS_ERROR_OPERATION_FAILED;

    out_image->image_id = VA_INVALID_ID;
    obj_surface = SURFACE(surface);

    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    if (!obj_surface->bo) {
        unsigned int is_tiled = 0;
        unsigned int fourcc = VA_FOURCC('Y', 'V', '1', '2');
        i965_guess_surface_format(ctx, surface, &fourcc, &is_tiled);
        i965_check_alloc_surface_bo(ctx, obj_surface, is_tiled, fourcc, SUBSAMPLE_YUV420);
    }

    assert(obj_surface->fourcc);

    w_pitch = obj_surface->width;
    h_pitch = obj_surface->height;

    image_id = NEW_IMAGE_ID();

    if (image_id == VA_INVALID_ID)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    obj_image = IMAGE(image_id);
    
    if (!obj_image)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    obj_image->bo = NULL;
    obj_image->palette = NULL;
    obj_image->derived_surface = VA_INVALID_ID;

    VAImage * const image = &obj_image->image;
    
    memset(image, 0, sizeof(*image));
    image->image_id = image_id;
    image->buf = VA_INVALID_ID;
    image->num_palette_entries = 0;
    image->entry_bytes = 0;
    image->width = obj_surface->orig_width;
    image->height = obj_surface->orig_height;
    image->data_size = obj_surface->size;

    image->format.fourcc = obj_surface->fourcc;
    image->format.byte_order = VA_LSB_FIRST;
    image->format.bits_per_pixel = 12;

    switch (image->format.fourcc) {
    case VA_FOURCC('Y', 'V', '1', '2'):
        image->num_planes = 3;
        image->pitches[0] = w_pitch; /* Y */
        image->offsets[0] = 0;
        image->pitches[1] = obj_surface->cb_cr_pitch; /* V */
        image->offsets[1] = w_pitch * obj_surface->y_cr_offset;
        image->pitches[2] = obj_surface->cb_cr_pitch; /* U */
        image->offsets[2] = w_pitch * obj_surface->y_cb_offset;
        break;

    case VA_FOURCC('N', 'V', '1', '2'):
        image->num_planes = 2;
        image->pitches[0] = w_pitch; /* Y */
        image->offsets[0] = 0;
        image->pitches[1] = obj_surface->cb_cr_pitch; /* UV */
        image->offsets[1] = w_pitch * obj_surface->y_cb_offset;
        break;

    case VA_FOURCC('I', '4', '2', '0'):
        image->num_planes = 3;
        image->pitches[0] = w_pitch; /* Y */
        image->offsets[0] = 0;
        image->pitches[1] = obj_surface->cb_cr_pitch; /* U */
        image->offsets[1] = w_pitch * obj_surface->y_cb_offset;
        image->pitches[2] = obj_surface->cb_cr_pitch; /* V */
        image->offsets[2] = w_pitch * obj_surface->y_cr_offset;
        break;

    default:
        goto error;
    }

    va_status = i965_create_buffer_internal(ctx, 0, VAImageBufferType,
                                            obj_surface->size, 1, NULL, obj_surface->bo, &image->buf);
    if (va_status != VA_STATUS_SUCCESS)
        goto error;

    obj_image->bo = BUFFER(image->buf)->buffer_store->bo;
    dri_bo_reference(obj_image->bo);

    if (image->num_palette_entries > 0 && image->entry_bytes > 0) {
        obj_image->palette = malloc(image->num_palette_entries * sizeof(obj_image->palette));
        if (!obj_image->palette) {
            va_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
            goto error;
        }
    }

    *out_image = *image;
    obj_surface->flags |= SURFACE_DERIVED;
    obj_image->derived_surface = surface;

    return VA_STATUS_SUCCESS;

 error:
    i965_DestroyImage(ctx, image_id);
    return va_status;
}

static void 
i965_destroy_image(struct object_heap *heap, struct object_base *obj)
{
    object_heap_free(heap, obj);
}


VAStatus 
i965_DestroyImage(VADriverContextP ctx, VAImageID image)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_image *obj_image = IMAGE(image); 
    struct object_surface *obj_surface; 

    if (!obj_image)
        return VA_STATUS_SUCCESS;

    dri_bo_unreference(obj_image->bo);
    obj_image->bo = NULL;

    if (obj_image->image.buf != VA_INVALID_ID) {
        i965_DestroyBuffer(ctx, obj_image->image.buf);
        obj_image->image.buf = VA_INVALID_ID;
    }

    if (obj_image->palette) {
        free(obj_image->palette);
        obj_image->palette = NULL;
    }

    obj_surface = SURFACE(obj_image->derived_surface);

    if (obj_surface) {
        obj_surface->flags &= ~SURFACE_DERIVED;
    }

    i965_destroy_image(&i965->image_heap, (struct object_base *)obj_image);

    return VA_STATUS_SUCCESS;
}

/*
 * pointer to an array holding the palette data.  The size of the array is
 * num_palette_entries * entry_bytes in size.  The order of the components
 * in the palette is described by the component_order in VASubpicture struct
 */
VAStatus 
i965_SetImagePalette(VADriverContextP ctx,
                     VAImageID image,
                     unsigned char *palette)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    unsigned int i;

    struct object_image *obj_image = IMAGE(image);
    if (!obj_image)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    if (!obj_image->palette)
        return VA_STATUS_ERROR_ALLOCATION_FAILED; /* XXX: unpaletted/error */

    for (i = 0; i < obj_image->image.num_palette_entries; i++)
        obj_image->palette[i] = (((unsigned int)palette[3*i + 0] << 16) |
                                 ((unsigned int)palette[3*i + 1] <<  8) |
                                 (unsigned int)palette[3*i + 2]);
    return VA_STATUS_SUCCESS;
}

static inline void
memcpy_pic(uint8_t *dst, unsigned int dst_stride,
           const uint8_t *src, unsigned int src_stride,
           unsigned int len, unsigned int height)
{
    unsigned int i;

    for (i = 0; i < height; i++) {
        memcpy(dst, src, len);
        dst += dst_stride;
        src += src_stride;
    }
}

static void
get_image_i420(struct object_image *obj_image, uint8_t *image_data,
               struct object_surface *obj_surface,
               const VARectangle *rect)
{
    uint8_t *dst[3], *src[3];
    const int Y = 0;
    const int U = obj_image->image.format.fourcc == obj_surface->fourcc ? 1 : 2;
    const int V = obj_image->image.format.fourcc == obj_surface->fourcc ? 2 : 1;
    unsigned int tiling, swizzle;

    if (!obj_surface->bo)
        return;

    assert(obj_surface->fourcc);
    dri_bo_get_tiling(obj_surface->bo, &tiling, &swizzle);

    if (tiling != I915_TILING_NONE)
        drm_intel_gem_bo_map_gtt(obj_surface->bo);
    else
        dri_bo_map(obj_surface->bo, 0);

    if (!obj_surface->bo->virtual)
        return;

    /* Dest VA image has either I420 or YV12 format.
       Source VA surface alway has I420 format */
    dst[Y] = image_data + obj_image->image.offsets[Y];
    src[0] = (uint8_t *)obj_surface->bo->virtual;
    dst[U] = image_data + obj_image->image.offsets[U];
    src[1] = src[0] + obj_surface->width * obj_surface->height;
    dst[V] = image_data + obj_image->image.offsets[V];
    src[2] = src[1] + (obj_surface->width / 2) * (obj_surface->height / 2);

    /* Y plane */
    dst[Y] += rect->y * obj_image->image.pitches[Y] + rect->x;
    src[0] += rect->y * obj_surface->width + rect->x;
    memcpy_pic(dst[Y], obj_image->image.pitches[Y],
               src[0], obj_surface->width,
               rect->width, rect->height);

    /* U plane */
    dst[U] += (rect->y / 2) * obj_image->image.pitches[U] + rect->x / 2;
    src[1] += (rect->y / 2) * obj_surface->width / 2 + rect->x / 2;
    memcpy_pic(dst[U], obj_image->image.pitches[U],
               src[1], obj_surface->width / 2,
               rect->width / 2, rect->height / 2);

    /* V plane */
    dst[V] += (rect->y / 2) * obj_image->image.pitches[V] + rect->x / 2;
    src[2] += (rect->y / 2) * obj_surface->width / 2 + rect->x / 2;
    memcpy_pic(dst[V], obj_image->image.pitches[V],
               src[2], obj_surface->width / 2,
               rect->width / 2, rect->height / 2);

    if (tiling != I915_TILING_NONE)
        drm_intel_gem_bo_unmap_gtt(obj_surface->bo);
    else
        dri_bo_unmap(obj_surface->bo);
}

static void
get_image_nv12(struct object_image *obj_image, uint8_t *image_data,
               struct object_surface *obj_surface,
               const VARectangle *rect)
{
    uint8_t *dst[2], *src[2];
    unsigned int tiling, swizzle;

    if (!obj_surface->bo)
        return;

    assert(obj_surface->fourcc);
    dri_bo_get_tiling(obj_surface->bo, &tiling, &swizzle);

    if (tiling != I915_TILING_NONE)
        drm_intel_gem_bo_map_gtt(obj_surface->bo);
    else
        dri_bo_map(obj_surface->bo, 0);

    if (!obj_surface->bo->virtual)
        return;

    /* Both dest VA image and source surface have NV12 format */
    dst[0] = image_data + obj_image->image.offsets[0];
    src[0] = (uint8_t *)obj_surface->bo->virtual;
    dst[1] = image_data + obj_image->image.offsets[1];
    src[1] = src[0] + obj_surface->width * obj_surface->height;

    /* Y plane */
    dst[0] += rect->y * obj_image->image.pitches[0] + rect->x;
    src[0] += rect->y * obj_surface->width + rect->x;
    memcpy_pic(dst[0], obj_image->image.pitches[0],
               src[0], obj_surface->width,
               rect->width, rect->height);

    /* UV plane */
    dst[1] += (rect->y / 2) * obj_image->image.pitches[1] + (rect->x & -2);
    src[1] += (rect->y / 2) * obj_surface->width + (rect->x & -2);
    memcpy_pic(dst[1], obj_image->image.pitches[1],
               src[1], obj_surface->width,
               rect->width, rect->height / 2);

    if (tiling != I915_TILING_NONE)
        drm_intel_gem_bo_unmap_gtt(obj_surface->bo);
    else
        dri_bo_unmap(obj_surface->bo);
}

static VAStatus 
i965_sw_getimage(VADriverContextP ctx,
                 VASurfaceID surface,
                 int x,   /* coordinates of the upper left source pixel */
                 int y,
                 unsigned int width,      /* width and height of the region */
                 unsigned int height,
                 VAImageID image)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct i965_render_state *render_state = &i965->render_state;

    struct object_surface *obj_surface = SURFACE(surface);
    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    struct object_image *obj_image = IMAGE(image);
    if (!obj_image)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    if (x < 0 || y < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (x + width > obj_surface->orig_width ||
        y + height > obj_surface->orig_height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (x + width > obj_image->image.width ||
        y + height > obj_image->image.height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    VAStatus va_status;
    void *image_data = NULL;

    va_status = i965_MapBuffer(ctx, obj_image->image.buf, &image_data);
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    VARectangle rect;
    rect.x = x;
    rect.y = y;
    rect.width = width;
    rect.height = height;

    switch (obj_image->image.format.fourcc) {
    case VA_FOURCC('Y','V','1','2'):
    case VA_FOURCC('I','4','2','0'):
        /* I420 is native format for MPEG-2 decoded surfaces */
        if (render_state->interleaved_uv)
            goto operation_failed;
        get_image_i420(obj_image, image_data, obj_surface, &rect);
        break;
    case VA_FOURCC('N','V','1','2'):
        /* NV12 is native format for H.264 decoded surfaces */
        if (!render_state->interleaved_uv)
            goto operation_failed;
        get_image_nv12(obj_image, image_data, obj_surface, &rect);
        break;
    default:
    operation_failed:
        va_status = VA_STATUS_ERROR_OPERATION_FAILED;
        break;
    }

    i965_UnmapBuffer(ctx, obj_image->image.buf);
    return va_status;
}

static VAStatus 
i965_hw_getimage(VADriverContextP ctx,
                 VASurfaceID surface,
                 int x,   /* coordinates of the upper left source pixel */
                 int y,
                 unsigned int width,      /* width and height of the region */
                 unsigned int height,
                 VAImageID image)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct i965_surface src_surface;
    struct i965_surface dst_surface;
    VAStatus va_status;
    VARectangle rect;
    struct object_surface *obj_surface = SURFACE(surface);
    struct object_image *obj_image = IMAGE(image);

    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    if (!obj_image)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    if (x < 0 || y < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (x + width > obj_surface->orig_width ||
        y + height > obj_surface->orig_height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (x + width > obj_image->image.width ||
        y + height > obj_image->image.height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (!obj_surface->bo)
        return VA_STATUS_SUCCESS;

    rect.x = x;
    rect.y = y;
    rect.width = width;
    rect.height = height;

    src_surface.id = surface;
    src_surface.type = I965_SURFACE_TYPE_SURFACE;
    src_surface.flags = I965_SURFACE_FLAG_FRAME;

    dst_surface.id = image;
    dst_surface.type = I965_SURFACE_TYPE_IMAGE;
    dst_surface.flags = I965_SURFACE_FLAG_FRAME;

    va_status = i965_image_processing(ctx,
                                      &src_surface,
                                      &rect,
                                      &dst_surface,
                                      &rect);


    return va_status;
}

VAStatus 
i965_GetImage(VADriverContextP ctx,
              VASurfaceID surface,
              int x,   /* coordinates of the upper left source pixel */
              int y,
              unsigned int width,      /* width and height of the region */
              unsigned int height,
              VAImageID image)
{
    struct i965_driver_data * const i965 = i965_driver_data(ctx);
    VAStatus va_status;

    if (HAS_ACCELERATED_GETIMAGE(i965))
        va_status = i965_hw_getimage(ctx,
                                     surface,
                                     x, y,
                                     width, height,
                                     image);
    else
        va_status = i965_sw_getimage(ctx,
                                     surface,
                                     x, y,
                                     width, height,
                                     image);

    return va_status;
}

static void
put_image_i420(struct object_surface *obj_surface,
               const VARectangle *dst_rect,
               struct object_image *obj_image, uint8_t *image_data,
               const VARectangle *src_rect)
{
    uint8_t *dst[3], *src[3];
    const int Y = 0;
    const int U = obj_image->image.format.fourcc == obj_surface->fourcc ? 1 : 2;
    const int V = obj_image->image.format.fourcc == obj_surface->fourcc ? 2 : 1;
    unsigned int tiling, swizzle;

    if (!obj_surface->bo)
        return;

    assert(obj_surface->fourcc);
    assert(dst_rect->width == src_rect->width);
    assert(dst_rect->height == src_rect->height);
    dri_bo_get_tiling(obj_surface->bo, &tiling, &swizzle);

    if (tiling != I915_TILING_NONE)
        drm_intel_gem_bo_map_gtt(obj_surface->bo);
    else
        dri_bo_map(obj_surface->bo, 0);

    if (!obj_surface->bo->virtual)
        return;

    /* Dest VA image has either I420 or YV12 format.
       Source VA surface alway has I420 format */
    dst[0] = (uint8_t *)obj_surface->bo->virtual;
    src[Y] = image_data + obj_image->image.offsets[Y];
    dst[1] = dst[0] + obj_surface->width * obj_surface->height;
    src[U] = image_data + obj_image->image.offsets[U];
    dst[2] = dst[1] + (obj_surface->width / 2) * (obj_surface->height / 2);
    src[V] = image_data + obj_image->image.offsets[V];

    /* Y plane */
    dst[0] += dst_rect->y * obj_surface->width + dst_rect->x;
    src[Y] += src_rect->y * obj_image->image.pitches[Y] + src_rect->x;
    memcpy_pic(dst[0], obj_surface->width,
               src[Y], obj_image->image.pitches[Y],
               src_rect->width, src_rect->height);

    /* U plane */
    dst[1] += (dst_rect->y / 2) * obj_surface->width / 2 + dst_rect->x / 2;
    src[U] += (src_rect->y / 2) * obj_image->image.pitches[U] + src_rect->x / 2;
    memcpy_pic(dst[1], obj_surface->width / 2,
               src[U], obj_image->image.pitches[U],
               src_rect->width / 2, src_rect->height / 2);

    /* V plane */
    dst[2] += (dst_rect->y / 2) * obj_surface->width / 2 + dst_rect->x / 2;
    src[V] += (src_rect->y / 2) * obj_image->image.pitches[V] + src_rect->x / 2;
    memcpy_pic(dst[2], obj_surface->width / 2,
               src[V], obj_image->image.pitches[V],
               src_rect->width / 2, src_rect->height / 2);

    if (tiling != I915_TILING_NONE)
        drm_intel_gem_bo_unmap_gtt(obj_surface->bo);
    else
        dri_bo_unmap(obj_surface->bo);
}

static void
put_image_nv12(struct object_surface *obj_surface,
               const VARectangle *dst_rect,
               struct object_image *obj_image, uint8_t *image_data,
               const VARectangle *src_rect)
{
    uint8_t *dst[2], *src[2];
    unsigned int tiling, swizzle;

    if (!obj_surface->bo)
        return;

    assert(obj_surface->fourcc);
    assert(dst_rect->width == src_rect->width);
    assert(dst_rect->height == src_rect->height);
    dri_bo_get_tiling(obj_surface->bo, &tiling, &swizzle);

    if (tiling != I915_TILING_NONE)
        drm_intel_gem_bo_map_gtt(obj_surface->bo);
    else
        dri_bo_map(obj_surface->bo, 0);

    if (!obj_surface->bo->virtual)
        return;

    /* Both dest VA image and source surface have NV12 format */
    dst[0] = (uint8_t *)obj_surface->bo->virtual;
    src[0] = image_data + obj_image->image.offsets[0];
    dst[1] = dst[0] + obj_surface->width * obj_surface->height;
    src[1] = image_data + obj_image->image.offsets[1];

    /* Y plane */
    dst[0] += dst_rect->y * obj_surface->width + dst_rect->x;
    src[0] += src_rect->y * obj_image->image.pitches[0] + src_rect->x;
    memcpy_pic(dst[0], obj_surface->width,
               src[0], obj_image->image.pitches[0],
               src_rect->width, src_rect->height);

    /* UV plane */
    dst[1] += (dst_rect->y / 2) * obj_surface->width + (dst_rect->x & -2);
    src[1] += (src_rect->y / 2) * obj_image->image.pitches[1] + (src_rect->x & -2);
    memcpy_pic(dst[1], obj_surface->width,
               src[1], obj_image->image.pitches[1],
               src_rect->width, src_rect->height / 2);

    if (tiling != I915_TILING_NONE)
        drm_intel_gem_bo_unmap_gtt(obj_surface->bo);
    else
        dri_bo_unmap(obj_surface->bo);
}

static VAStatus
i965_sw_putimage(VADriverContextP ctx,
                 VASurfaceID surface,
                 VAImageID image,
                 int src_x,
                 int src_y,
                 unsigned int src_width,
                 unsigned int src_height,
                 int dest_x,
                 int dest_y,
                 unsigned int dest_width,
                 unsigned int dest_height)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_surface *obj_surface = SURFACE(surface);

    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    struct object_image *obj_image = IMAGE(image);
    if (!obj_image)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    if (src_x < 0 || src_y < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (src_x + src_width > obj_image->image.width ||
        src_y + src_height > obj_image->image.height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (dest_x < 0 || dest_y < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (dest_x + dest_width > obj_surface->orig_width ||
        dest_y + dest_height > obj_surface->orig_height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* XXX: don't allow scaling */
    if (src_width != dest_width || src_height != dest_height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (obj_surface->fourcc) {
        /* Don't allow format mismatch */
        if (obj_surface->fourcc != obj_image->image.format.fourcc)
            return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    }

    else {
        /* VA is surface not used for decoding, use same VA image format */
        i965_check_alloc_surface_bo(
            ctx,
            obj_surface,
            0, /* XXX: don't use tiled surface */
            obj_image->image.format.fourcc,
            SUBSAMPLE_YUV420);
    }

    VAStatus va_status;
    void *image_data = NULL;

    va_status = i965_MapBuffer(ctx, obj_image->image.buf, &image_data);
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    VARectangle src_rect, dest_rect;
    src_rect.x       = src_x;
    src_rect.y       = src_y;
    src_rect.width   = src_width;
    src_rect.height  = src_height;
    dest_rect.x      = dest_x;
    dest_rect.y      = dest_y;
    dest_rect.width  = dest_width;
    dest_rect.height = dest_height;
     
    switch (obj_image->image.format.fourcc) {
    case VA_FOURCC('Y','V','1','2'):
    case VA_FOURCC('I','4','2','0'):
        put_image_i420(obj_surface, &dest_rect, obj_image, image_data, &src_rect);
        break;
    case VA_FOURCC('N','V','1','2'):
        put_image_nv12(obj_surface, &dest_rect, obj_image, image_data, &src_rect);
        break;
    default:
        va_status = VA_STATUS_ERROR_OPERATION_FAILED;
        break;
    }

    i965_UnmapBuffer(ctx, obj_image->image.buf);
    return va_status;
}

static VAStatus 
i965_hw_putimage(VADriverContextP ctx,
                 VASurfaceID surface,
                 VAImageID image,
                 int src_x,
                 int src_y,
                 unsigned int src_width,
                 unsigned int src_height,
                 int dest_x,
                 int dest_y,
                 unsigned int dest_width,
                 unsigned int dest_height)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_surface *obj_surface = SURFACE(surface);
    struct object_image *obj_image = IMAGE(image);
    struct i965_surface src_surface, dst_surface;
    VAStatus va_status = VA_STATUS_SUCCESS;
    VARectangle src_rect, dst_rect;

    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    if (!obj_image || !obj_image->bo)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    if (src_x < 0 ||
        src_y < 0 ||
        src_x + src_width > obj_image->image.width ||
        src_y + src_height > obj_image->image.height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (dest_x < 0 ||
        dest_y < 0 ||
        dest_x + dest_width > obj_surface->orig_width ||
        dest_y + dest_height > obj_surface->orig_height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (!obj_surface->bo) {
        unsigned int tiling, swizzle;
        dri_bo_get_tiling(obj_image->bo, &tiling, &swizzle);

        i965_check_alloc_surface_bo(ctx,
                                    obj_surface,
                                    !!tiling,
                                    obj_image->image.format.fourcc,
                                    SUBSAMPLE_YUV420);
    }

    assert(obj_surface->fourcc);

    src_surface.id = image;
    src_surface.type = I965_SURFACE_TYPE_IMAGE;
    src_surface.flags = I965_SURFACE_FLAG_FRAME;
    src_rect.x = src_x;
    src_rect.y = src_y;
    src_rect.width = src_width;
    src_rect.height = src_height;

    dst_surface.id = surface;
    dst_surface.type = I965_SURFACE_TYPE_SURFACE;
    dst_surface.flags = I965_SURFACE_FLAG_FRAME;
    dst_rect.x = dest_x;
    dst_rect.y = dest_y;
    dst_rect.width = dest_width;
    dst_rect.height = dest_height;

    va_status = i965_image_processing(ctx,
                                      &src_surface,
                                      &src_rect,
                                      &dst_surface,
                                      &dst_rect);

    return  va_status;
}

static VAStatus 
i965_PutImage(VADriverContextP ctx,
              VASurfaceID surface,
              VAImageID image,
              int src_x,
              int src_y,
              unsigned int src_width,
              unsigned int src_height,
              int dest_x,
              int dest_y,
              unsigned int dest_width,
              unsigned int dest_height)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    VAStatus va_status = VA_STATUS_SUCCESS;

    if (HAS_ACCELERATED_PUTIMAGE(i965))
        va_status = i965_hw_putimage(ctx,
                                     surface,
                                     image,
                                     src_x,
                                     src_y,
                                     src_width,
                                     src_height,
                                     dest_x,
                                     dest_y,
                                     dest_width,
                                     dest_height);
    else 
        va_status = i965_sw_putimage(ctx,
                                     surface,
                                     image,
                                     src_x,
                                     src_y,
                                     src_width,
                                     src_height,
                                     dest_x,
                                     dest_y,
                                     dest_width,
                                     dest_height);

    return va_status;
}

VAStatus 
i965_PutSurface(VADriverContextP ctx,
                VASurfaceID surface,
                void *draw, /* X Drawable */
                short srcx,
                short srcy,
                unsigned short srcw,
                unsigned short srch,
                short destx,
                short desty,
                unsigned short destw,
                unsigned short desth,
                VARectangle *cliprects, /* client supplied clip list */
                unsigned int number_cliprects, /* number of clip rects in the clip list */
                unsigned int flags) /* de-interlacing flags */
{
    struct i965_driver_data *i965 = i965_driver_data(ctx); 
    struct dri_state *dri_state = (struct dri_state *)ctx->dri_state;
    struct i965_render_state *render_state = &i965->render_state;
    struct dri_drawable *dri_drawable;
    union dri_buffer *buffer;
    struct intel_region *dest_region;
    struct object_surface *obj_surface; 
    VARectangle src_rect, dst_rect;
    int ret;
    uint32_t name;
    Bool new_region = False;
    int pp_flag = 0;

    /* Currently don't support DRI1 */
    if (dri_state->driConnectedFlag != VA_DRI2)
        return VA_STATUS_ERROR_UNKNOWN;

    /* Some broken sources such as H.264 conformance case FM2_SVA_C
     * will get here
     */
    obj_surface = SURFACE(surface);
    if (!obj_surface || !obj_surface->bo)
        return VA_STATUS_SUCCESS;

    _i965LockMutex(&i965->render_mutex);

    dri_drawable = dri_get_drawable(ctx, (Drawable)draw);
    assert(dri_drawable);

    buffer = dri_get_rendering_buffer(ctx, dri_drawable);
    assert(buffer);
    
    dest_region = render_state->draw_region;

    if (dest_region) {
        assert(dest_region->bo);
        dri_bo_flink(dest_region->bo, &name);
        
        if (buffer->dri2.name != name) {
            new_region = True;
            dri_bo_unreference(dest_region->bo);
        }
    } else {
        dest_region = (struct intel_region *)calloc(1, sizeof(*dest_region));
        assert(dest_region);
        render_state->draw_region = dest_region;
        new_region = True;
    }

    if (new_region) {
        dest_region->x = dri_drawable->x;
        dest_region->y = dri_drawable->y;
        dest_region->width = dri_drawable->width;
        dest_region->height = dri_drawable->height;
        dest_region->cpp = buffer->dri2.cpp;
        dest_region->pitch = buffer->dri2.pitch;

        dest_region->bo = intel_bo_gem_create_from_name(i965->intel.bufmgr, "rendering buffer", buffer->dri2.name);
        assert(dest_region->bo);

        ret = dri_bo_get_tiling(dest_region->bo, &(dest_region->tiling), &(dest_region->swizzle));
        assert(ret == 0);
    }

    if ((flags & VA_FILTER_SCALING_MASK) == VA_FILTER_SCALING_NL_ANAMORPHIC)
        pp_flag |= I965_PP_FLAG_AVS;

    if (flags & VA_TOP_FIELD)
        pp_flag |= I965_PP_FLAG_DEINTERLACING_TOP_FISRT;
    else if (flags & VA_BOTTOM_FIELD)
        pp_flag |= I965_PP_FLAG_DEINTERLACING_BOTTOM_FIRST;

    src_rect.x      = srcx;
    src_rect.y      = srcy;
    src_rect.width  = srcw;
    src_rect.height = srch;

    dst_rect.x      = destx;
    dst_rect.y      = desty;
    dst_rect.width  = destw;
    dst_rect.height = desth;

    intel_render_put_surface(ctx, surface, &src_rect, &dst_rect, pp_flag);

    if(obj_surface->subpic != VA_INVALID_ID) {
        intel_render_put_subpicture(ctx, surface, &src_rect, &dst_rect);
    }

    dri_swap_buffer(ctx, dri_drawable);
    obj_surface->flags |= SURFACE_DISPLAYED;

    if ((obj_surface->flags & SURFACE_ALL_MASK) == SURFACE_DISPLAYED) {
        dri_bo_unreference(obj_surface->bo);
        obj_surface->bo = NULL;
        obj_surface->flags &= ~SURFACE_REF_DIS_MASK;

        if (obj_surface->free_private_data)
            obj_surface->free_private_data(&obj_surface->private_data);
    }

    _i965UnlockMutex(&i965->render_mutex);

    return VA_STATUS_SUCCESS;
}

VAStatus 
i965_Terminate(VADriverContextP ctx)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);

    if (i965->batch)
        intel_batchbuffer_free(i965->batch);

    _i965DestroyMutex(&i965->render_mutex);

    if (i965_render_terminate(ctx) == False)
        return VA_STATUS_ERROR_UNKNOWN;

    if (i965_post_processing_terminate(ctx) == False)
        return VA_STATUS_ERROR_UNKNOWN;

    if (intel_driver_terminate(ctx) == False)
        return VA_STATUS_ERROR_UNKNOWN;

    i965_destroy_heap(&i965->buffer_heap, i965_destroy_buffer);
    i965_destroy_heap(&i965->image_heap, i965_destroy_image);
    i965_destroy_heap(&i965->subpic_heap, i965_destroy_subpic);
    i965_destroy_heap(&i965->surface_heap, i965_destroy_surface);
    i965_destroy_heap(&i965->context_heap, i965_destroy_context);
    i965_destroy_heap(&i965->config_heap, i965_destroy_config);

    free(ctx->pDriverData);
    ctx->pDriverData = NULL;

    return VA_STATUS_SUCCESS;
}

static VAStatus
i965_BufferInfo(
    VADriverContextP ctx,       /* in */
    VABufferID buf_id,          /* in */
    VABufferType *type,         /* out */
    unsigned int *size,         /* out */
    unsigned int *num_elements  /* out */
)
{
    struct i965_driver_data *i965 = NULL;
    struct object_buffer *obj_buffer = NULL;

    i965 = i965_driver_data(ctx);
    obj_buffer = BUFFER(buf_id);

    *type = obj_buffer->type;
    *size = obj_buffer->size_element;
    *num_elements = obj_buffer->num_elements;

    return VA_STATUS_SUCCESS;
}

static VAStatus
i965_LockSurface(
    VADriverContextP ctx,           /* in */
    VASurfaceID surface,            /* in */
    unsigned int *fourcc,           /* out */
    unsigned int *luma_stride,      /* out */
    unsigned int *chroma_u_stride,  /* out */
    unsigned int *chroma_v_stride,  /* out */
    unsigned int *luma_offset,      /* out */
    unsigned int *chroma_u_offset,  /* out */
    unsigned int *chroma_v_offset,  /* out */
    unsigned int *buffer_name,      /* out */
    void **buffer                   /* out */
)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_surface *obj_surface = NULL;
    VAImage tmpImage;

    assert(fourcc);
    assert(luma_stride);
    assert(chroma_u_stride);
    assert(chroma_v_stride);
    assert(luma_offset);
    assert(chroma_u_offset);
    assert(chroma_v_offset);
    assert(buffer_name);
    assert(buffer);

    tmpImage.image_id = VA_INVALID_ID;

    obj_surface = SURFACE(surface);
    if (obj_surface == NULL) {
        // Surface is absent.
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
        goto error;
    }

    // Lock functionality is absent now.
    if (obj_surface->locked_image_id != VA_INVALID_ID) {
        // Surface is locked already.
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
        goto error;
    }

    vaStatus = i965_DeriveImage(
        ctx,
        surface,
        &tmpImage);
    if (vaStatus != VA_STATUS_SUCCESS) {
        goto error;
    }

    obj_surface->locked_image_id = tmpImage.image_id;

    vaStatus = i965_MapBuffer(
        ctx,
        tmpImage.buf,
        buffer);
    if (vaStatus != VA_STATUS_SUCCESS) {
        goto error;
    }

    *fourcc = tmpImage.format.fourcc;
    *luma_offset = tmpImage.offsets[0];
    *luma_stride = tmpImage.pitches[0];
    *chroma_u_offset = tmpImage.offsets[1];
    *chroma_u_stride = tmpImage.pitches[1];
    *chroma_v_offset = tmpImage.offsets[2];
    *chroma_v_stride = tmpImage.pitches[2];
    *buffer_name = tmpImage.buf;

error:
    if (vaStatus != VA_STATUS_SUCCESS) {
        buffer = NULL;
    }

    return vaStatus;
}

static VAStatus
i965_UnlockSurface(
    VADriverContextP ctx,   /* in */
    VASurfaceID surface     /* in */
)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_image *locked_img = NULL;
    struct object_surface *obj_surface = NULL;

    obj_surface = SURFACE(surface);

    if (obj_surface == NULL) {
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;   // Surface is absent
        goto error;
    }
    if (obj_surface->locked_image_id == VA_INVALID_ID) {
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;   // Surface is not locked
        goto error;
    }

    locked_img = IMAGE(obj_surface->locked_image_id);
    if (locked_img == NULL || (locked_img->image.image_id == VA_INVALID_ID)) {
        // Work image was deallocated before i965_UnlockSurface()
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
        goto error;
    }

    vaStatus = i965_UnmapBuffer(
        ctx,
        locked_img->image.buf);
    if (vaStatus != VA_STATUS_SUCCESS) {
        goto error;
    }

    vaStatus = i965_DestroyImage(
        ctx,
        locked_img->image.image_id);
    if (vaStatus != VA_STATUS_SUCCESS) {
        goto error;
    }

    locked_img->image.image_id = VA_INVALID_ID;

 error:
    return vaStatus;
}

/* 
 * Query video processing pipeline 
 */
VAStatus i965_QueryVideoProcFilters(
    VADriverContextP    ctx,
    VAContextID         context,
    VAProcFilterType   *filters,
    unsigned int       *num_filters
    )
{
    struct i965_driver_data *const i965 = i965_driver_data(ctx);
    unsigned int i = 0;
    
    if (HAS_VPP(i965)) {
        filters[i++] = VAProcFilterNoiseReduction;
        filters[i++] = VAProcFilterDeinterlacing;
    }

    *num_filters = i;

    return VA_STATUS_SUCCESS;
}

VAStatus i965_QueryVideoProcFilterCaps(
    VADriverContextP    ctx,
    VAContextID         context,
    VAProcFilterType    type,
    void               *filter_caps,
    unsigned int       *num_filter_caps
    )
{
    unsigned int i = 0;

    if (type == VAProcFilterNoiseReduction) {
        VAProcFilterCap *cap = filter_caps;

        cap->range.min_value = 0.0;
        cap->range.max_value = 1.0;
        cap->range.default_value = 0.5;
        cap->range.step = 0.03125; /* 1.0 / 32 */
        i++;
    } else if (type == VAProcFilterDeinterlacing) {
        VAProcFilterCapDeinterlacing *cap = filter_caps;
        
        cap->type = VAProcDeinterlacingBob;
        i++;
        cap++;
        cap->type = VAProcDeinterlacingWeave;
        i++;
        cap++;
    }

    *num_filter_caps = i;

    return VA_STATUS_SUCCESS;
}

VAStatus i965_QueryVideoProcPipelineCaps(
    VADriverContextP ctx,
    VAContextID context,
    VABufferID *filters,
    unsigned int num_filters,
    VAProcPipelineCaps *pipeline_cap     /* out */
    )
{
    struct i965_driver_data * const i965 = i965_driver_data(ctx);
    unsigned int i = 0;

    pipeline_cap->flags = 0;
    pipeline_cap->pipeline_flags = 0;
    pipeline_cap->filter_flags = 0;
    pipeline_cap->num_forward_references = 0;
    pipeline_cap->num_backward_references = 0;
    pipeline_cap->num_input_color_standards = 0;
    pipeline_cap->input_color_standards[pipeline_cap->num_input_color_standards++] = VAProcColorStandardBT601;
    pipeline_cap->num_output_color_standards = 0;
    pipeline_cap->output_color_standards[pipeline_cap->num_output_color_standards++] = VAProcColorStandardBT601;

    for (i = 0; i < num_filters; i++) {
        struct object_buffer *obj_buffer = BUFFER(filters[i]);
        VAProcFilterParameterBufferBase *base = (VAProcFilterParameterBufferBase *)obj_buffer->buffer_store->buffer;

        if (base->type == VAProcFilterNoiseReduction) {
            VAProcFilterParameterBuffer *denoise = (VAProcFilterParameterBuffer *)base;
            (void)denoise;
        } else if (base->type == VAProcFilterDeinterlacing) {
            VAProcFilterParameterBufferDeinterlacing *deint = (VAProcFilterParameterBufferDeinterlacing *)base;

            assert(deint->algorithm == VAProcDeinterlacingWeave ||
                   deint->algorithm == VAProcDeinterlacingBob);
        }
    }

    return VA_STATUS_SUCCESS;
}

VAStatus DLL_EXPORT
VA_DRIVER_INIT_FUNC(VADriverContextP ctx);

VAStatus 
VA_DRIVER_INIT_FUNC(  VADriverContextP ctx )
{
    struct VADriverVTable * const vtable = ctx->vtable;
    struct VADriverVTableVPP * const vtable_vpp = ctx->vtable_vpp;

    struct i965_driver_data *i965;
    int result;

    ctx->version_major = VA_MAJOR_VERSION;
    ctx->version_minor = VA_MINOR_VERSION;
    ctx->max_profiles = I965_MAX_PROFILES;
    ctx->max_entrypoints = I965_MAX_ENTRYPOINTS;
    ctx->max_attributes = I965_MAX_CONFIG_ATTRIBUTES;
    ctx->max_image_formats = I965_MAX_IMAGE_FORMATS;
    ctx->max_subpic_formats = I965_MAX_SUBPIC_FORMATS;
    ctx->max_display_attributes = I965_MAX_DISPLAY_ATTRIBUTES;

    vtable->vaTerminate = i965_Terminate;
    vtable->vaQueryConfigEntrypoints = i965_QueryConfigEntrypoints;
    vtable->vaQueryConfigProfiles = i965_QueryConfigProfiles;
    vtable->vaQueryConfigEntrypoints = i965_QueryConfigEntrypoints;
    vtable->vaQueryConfigAttributes = i965_QueryConfigAttributes;
    vtable->vaCreateConfig = i965_CreateConfig;
    vtable->vaDestroyConfig = i965_DestroyConfig;
    vtable->vaGetConfigAttributes = i965_GetConfigAttributes;
    vtable->vaCreateSurfaces = i965_CreateSurfaces;
    vtable->vaDestroySurfaces = i965_DestroySurfaces;
    vtable->vaCreateContext = i965_CreateContext;
    vtable->vaDestroyContext = i965_DestroyContext;
    vtable->vaCreateBuffer = i965_CreateBuffer;
    vtable->vaBufferSetNumElements = i965_BufferSetNumElements;
    vtable->vaMapBuffer = i965_MapBuffer;
    vtable->vaUnmapBuffer = i965_UnmapBuffer;
    vtable->vaDestroyBuffer = i965_DestroyBuffer;
    vtable->vaBeginPicture = i965_BeginPicture;
    vtable->vaRenderPicture = i965_RenderPicture;
    vtable->vaEndPicture = i965_EndPicture;
    vtable->vaSyncSurface = i965_SyncSurface;
    vtable->vaQuerySurfaceStatus = i965_QuerySurfaceStatus;
    vtable->vaPutSurface = i965_PutSurface;
    vtable->vaQueryImageFormats = i965_QueryImageFormats;
    vtable->vaCreateImage = i965_CreateImage;
    vtable->vaDeriveImage = i965_DeriveImage;
    vtable->vaDestroyImage = i965_DestroyImage;
    vtable->vaSetImagePalette = i965_SetImagePalette;
    vtable->vaGetImage = i965_GetImage;
    vtable->vaPutImage = i965_PutImage;
    vtable->vaQuerySubpictureFormats = i965_QuerySubpictureFormats;
    vtable->vaCreateSubpicture = i965_CreateSubpicture;
    vtable->vaDestroySubpicture = i965_DestroySubpicture;
    vtable->vaSetSubpictureImage = i965_SetSubpictureImage;
    vtable->vaSetSubpictureChromakey = i965_SetSubpictureChromakey;
    vtable->vaSetSubpictureGlobalAlpha = i965_SetSubpictureGlobalAlpha;
    vtable->vaAssociateSubpicture = i965_AssociateSubpicture;
    vtable->vaDeassociateSubpicture = i965_DeassociateSubpicture;
    vtable->vaQueryDisplayAttributes = i965_QueryDisplayAttributes;
    vtable->vaGetDisplayAttributes = i965_GetDisplayAttributes;
    vtable->vaSetDisplayAttributes = i965_SetDisplayAttributes;
    vtable->vaBufferInfo = i965_BufferInfo;
    vtable->vaLockSurface = i965_LockSurface;
    vtable->vaUnlockSurface = i965_UnlockSurface;

    vtable_vpp->vaQueryVideoProcFilters = i965_QueryVideoProcFilters;
    vtable_vpp->vaQueryVideoProcFilterCaps = i965_QueryVideoProcFilterCaps;
    vtable_vpp->vaQueryVideoProcPipelineCaps = i965_QueryVideoProcPipelineCaps;

    i965 = (struct i965_driver_data *)calloc(1, sizeof(*i965));
    assert(i965);
    ctx->pDriverData = (void *)i965;

    result = object_heap_init(&i965->config_heap, 
                              sizeof(struct object_config), 
                              CONFIG_ID_OFFSET);
    assert(result == 0);

    result = object_heap_init(&i965->context_heap, 
                              sizeof(struct object_context), 
                              CONTEXT_ID_OFFSET);
    assert(result == 0);

    result = object_heap_init(&i965->surface_heap, 
                              sizeof(struct object_surface), 
                              SURFACE_ID_OFFSET);
    assert(result == 0);

    result = object_heap_init(&i965->buffer_heap, 
                              sizeof(struct object_buffer), 
                              BUFFER_ID_OFFSET);
    assert(result == 0);

    result = object_heap_init(&i965->image_heap, 
                              sizeof(struct object_image), 
                              IMAGE_ID_OFFSET);
    assert(result == 0);

    result = object_heap_init(&i965->subpic_heap, 
                              sizeof(struct object_subpic), 
                              SUBPIC_ID_OFFSET);
    assert(result == 0);

    sprintf(i965->va_vendor, "%s %s driver - %d.%d.%d",
            INTEL_STR_DRIVER_VENDOR,
            INTEL_STR_DRIVER_NAME,
            INTEL_DRIVER_MAJOR_VERSION,
            INTEL_DRIVER_MINOR_VERSION,
            INTEL_DRIVER_MICRO_VERSION);

    if (INTEL_DRIVER_PRE_VERSION > 0) {
        const int len = strlen(i965->va_vendor);
        sprintf(&i965->va_vendor[len], ".pre%d", INTEL_DRIVER_PRE_VERSION);
    }

    i965->current_context_id = VA_INVALID_ID;

    ctx->str_vendor = i965->va_vendor;
    
    return i965_Init(ctx);
}
