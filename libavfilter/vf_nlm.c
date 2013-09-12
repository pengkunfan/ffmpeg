/*
 * Copyright (c) 2013 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file
 * Non-Local Means noise reduction filter.
 * See http://www.ipol.im/pub/art/2011/bcm_nlm/ for a description.
 */

#include <float.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
#include <emmintrin.h>
#include <assert.h>

#include <stdio.h>

#include "config.h"
#include "libavutil/attributes.h"
#include "libavutil/common.h"
#include "libavutil/pixdesc.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"


#include "libavutil/opt.h"


typedef struct
{
    uint8_t* img;
    int stride;

    int w,h;
    int border;

    uint8_t* mem_start;
} MonoImage;


typedef enum {
    ImageFormat_Mono,
    ImageFormat_YUV420,
    ImageFormat_YUV422,
    ImageFormat_YUV444,
    ImageFormat_RGB
} ImageFormat;


typedef struct
{
    MonoImage   plane[3];
    ImageFormat format;
} ColorImage;


static void alloc_and_copy_image_with_border(MonoImage* ext_img, const uint8_t* img, int stride, int w,int h, int in_border)
{
    int border = (in_border+15)/16*16;

    int inStride = (w+2*border);
    int inTotalHeight = (h+2*border);
    uint8_t* inputWithBorder = (uint8_t*)malloc(inStride*inTotalHeight);
    uint8_t* inWithBorderP = inputWithBorder+border+border*inStride;

    for (int y=0;y<h;y++) {
        memcpy(inWithBorderP+y*inStride, img+y*stride, w);
    }

    for (int k=0;k<border;k++) {
        memcpy(inWithBorderP-(k+1)*inStride, img, w);
        memcpy(inWithBorderP+(h+k)*inStride, img+(h-1)*stride, w);
    }

    for (int k=0;k<border;k++) {
        for (int y=-border;y<h+border;y++)
        {
            *(inWithBorderP  -k-1+y*inStride) = inWithBorderP[y*inStride];
            *(inWithBorderP+w+k  +y*inStride) = inWithBorderP[y*inStride+w-1];
        }
    }

    ext_img->img = inWithBorderP;
    ext_img->mem_start = inputWithBorder;
    ext_img->stride = inStride;
    ext_img->w = w;
    ext_img->h = h;
    ext_img->border = border;
}


static void free_mono_image(MonoImage* ext_img)
{
    if (ext_img->mem_start) {
        free(ext_img->mem_start);
        ext_img->mem_start=NULL;
        ext_img->img=NULL;
    }
}

static void free_color_image(ColorImage* ext_img)
{
    for (int c=0;c<3;c++) {
        free_mono_image(&(ext_img->plane[c]));
    }
}





typedef struct
{
    int    patch_size;
    int    range;
    double h_param;
    int    n_frames;
} NLMeansParams;


#define MAX_NLMeansImages 32

typedef struct {
    const AVClass *class;

    int hsub,vsub;

    NLMeansParams param;

    ColorImage images[MAX_NLMeansImages];
    int        image_available[MAX_NLMeansImages];

    void (*buildIntegralImage)(uint32_t* integral,   int integral_stride32,
                               const uint8_t* currimage, int currstride,
                               const uint8_t* image, int stride,
                               int  w,int  h,
                               int dx,int dy);
} NLMContext;



static void buildIntegralImage_scalar(uint32_t* integral,   int integral_stride32,
				      const uint8_t* currimage, int currstride,
				      const uint8_t* image, int stride,
				      int  w,int  h,
				      int dx,int dy)
{
    memset(integral-1-integral_stride32, 0, (w+1)*sizeof(uint32_t));

    for (int y=0;y<h;y++) {
        const uint8_t* p1 = currimage+y*currstride;
        const uint8_t* p2 = image+(y+dy)*stride+dx;
        uint32_t* out = integral+y*integral_stride32-1;

        *out++ = 0;

        for (int x=0;x<w;x++)
        {
            int diff = *p1++ - *p2++;
            diff= 0xFF;
            *out = *(out-1) + diff * diff;
            out++;
        }

        if (y>0) {
            out = integral+y*integral_stride32;

            for (int x=0;x<w;x++) {
                *out += *(out-integral_stride32);
                out++;
            }
        }
    }
}



/* Input image must be large enough to have valid pixels for the offset (dx,dy).
   I.e., with (dx,dy)=(-10,8), x-value up to -10 and y-values up to (h-1)+8 will be accessed.
   The integral image will be access with (x,y) in [-1,w)x[-1,h).

   Note also that we use 32bit for the integral image even though the values may overflow
   that range. However, the modulo-arithmetic used when computing the block sums later
   will be still correct when the block size is not too large.
 */
static void buildIntegralImage_SSE(uint32_t* integral, int integral_stride32,
                                   const uint8_t* currimage, int currstride,
				   const uint8_t*  image,    int stride,
				   int  w,int  h,
				   int dx,int dy)
{
    const __m128i zero = _mm_set1_epi8(0);

    memset(integral-1-integral_stride32, 0, (w+1)*sizeof(uint32_t));

    for (int y=0;y<h;y++) {
        const uint8_t* p1 = currimage+y*currstride;
        const uint8_t* p2 = image+(y+dy)*stride+dx;

        uint32_t* out = integral+y*integral_stride32-1;

        __m128i prevadd = _mm_set1_epi32(0);
        const int nPix = 16;

        *out++ = 0;

        for (int x=0;x<w;x+=nPix)
        {
            __m128i pa, pb;
            __m128i pla, plb;
            __m128i ldiff, lldiff, lhdiff;
            __m128i ltmp,htmp;
            __m128i ladd,hadd;
            __m128i pha,phb;
            __m128i hdiff,hldiff,hhdiff;
            __m128i l2tmp,h2tmp;



            pa = _mm_loadu_si128((__m128i*)p1);
            pb = _mm_loadu_si128((__m128i*)p2);

            pla = _mm_unpacklo_epi8(pa,zero);
            plb = _mm_unpacklo_epi8(pb,zero);

            ldiff = _mm_sub_epi16(pla,plb);
            ldiff = _mm_mullo_epi16(ldiff,ldiff);

            lldiff = _mm_unpacklo_epi16(ldiff,zero);
            lhdiff = _mm_unpackhi_epi16(ldiff,zero);

            ltmp = _mm_slli_si128(lldiff, 4);
            lldiff = _mm_add_epi32(lldiff, ltmp);
            ltmp = _mm_slli_si128(lldiff, 8);
            lldiff = _mm_add_epi32(lldiff, ltmp);
            lldiff = _mm_add_epi32(lldiff, prevadd);

            ladd = _mm_shuffle_epi32(lldiff, 0xff);

            htmp = _mm_slli_si128(lhdiff, 4);
            lhdiff = _mm_add_epi32(lhdiff, htmp);
            htmp = _mm_slli_si128(lhdiff, 8);
            lhdiff = _mm_add_epi32(lhdiff, htmp);
            lhdiff = _mm_add_epi32(lhdiff, ladd);

            prevadd = _mm_shuffle_epi32(lhdiff, 0xff);

            _mm_store_si128((__m128i*)(out),  lldiff);
            _mm_store_si128((__m128i*)(out+4),lhdiff);



            pha = _mm_unpackhi_epi8(pa,zero);
            phb = _mm_unpackhi_epi8(pb,zero);
            hdiff = _mm_sub_epi16(pha,phb);

            hdiff = _mm_mullo_epi16(hdiff,hdiff);

            hldiff = _mm_unpacklo_epi16(hdiff,zero);
            hhdiff = _mm_unpackhi_epi16(hdiff,zero);
            l2tmp = _mm_slli_si128(hldiff, 4);
            hldiff = _mm_add_epi32(hldiff, l2tmp);
            l2tmp = _mm_slli_si128(hldiff, 8);
            hldiff = _mm_add_epi32(hldiff, l2tmp);
            hldiff = _mm_add_epi32(hldiff, prevadd);
            hadd = _mm_shuffle_epi32(hldiff, 0xff);
            h2tmp = _mm_slli_si128(hhdiff, 4);
            hhdiff = _mm_add_epi32(hhdiff, h2tmp);
            h2tmp = _mm_slli_si128(hhdiff, 8);
            hhdiff = _mm_add_epi32(hhdiff, h2tmp);
            hhdiff = _mm_add_epi32(hhdiff, hadd);

            prevadd = _mm_shuffle_epi32(hhdiff, 0xff);

            _mm_store_si128((__m128i*)(out+8), hldiff);
            _mm_store_si128((__m128i*)(out+12),hhdiff);


            out+=nPix;
            p1+=nPix;
            p2+=nPix;
        }

        if (y>0) {
            out = integral+y*integral_stride32;

            for (int x=0;x<w;x+=16) {
                *((__m128i*)out) = _mm_add_epi32(*(__m128i*)(out-integral_stride32),
                                                 *(__m128i*)(out));

                *((__m128i*)(out+4)) = _mm_add_epi32(*(__m128i*)(out+4-integral_stride32),
                                                     *(__m128i*)(out+4));

                *((__m128i*)(out+8)) = _mm_add_epi32(*(__m128i*)(out+8-integral_stride32),
                                                     *(__m128i*)(out+8));

                *((__m128i*)(out+12)) = _mm_add_epi32(*(__m128i*)(out+12-integral_stride32),
                                                      *(__m128i*)(out+12));

                out += 4*4;
            }
        }
    }
}


struct PixelSum
{
    float weightSum;
    float pixelSum;
};



static void NLMeans_mono_multi(uint8_t* out, int outStride,
			       const MonoImage*const* images, int nImages,
			       const NLMeansParams* param)
{
    int w = images[0]->w;
    int h = images[0]->h;

    int n = (param->patch_size|1);
    int r = (param->range     |1);

    int n2 = (n-1)/2;
    int r2 = (r-1)/2;

    struct PixelSum* tmp_data = (struct PixelSum*)calloc(w*h,sizeof(struct PixelSum));

    int integral_stride32 = w+2*16;
    uint32_t* integral_mem = (uint32_t*)malloc( integral_stride32*(h+1)*sizeof(uint32_t) );
    uint32_t* integral = integral_mem + integral_stride32 + 16;


    float weightFact = 1.0/n/n / (param->h_param * param->h_param);

#define TABSIZE 128

    const int tabSize=TABSIZE;
    float exptab[TABSIZE];

    const float stretch = tabSize/ (-log(0.0005));
    float weightFactTab = weightFact*stretch;
    int diff_max = tabSize/weightFactTab;

    for (int i=0;i<tabSize;i++)
        exptab[i] = exp(-i/stretch);
    exptab[tabSize-1]=0;



    for (int imageIdx=0; imageIdx<nImages; imageIdx++)
    {
        // copy input image

        const uint8_t* currentWithBorderP = images[0]->img;
        int currentStride = images[0]->stride;

        const uint8_t* inWithBorderP = images[imageIdx]->img;
        int inStride = images[imageIdx]->stride;

        // ...

        for (int dy=-r2;dy<=r2;dy++)
            for (int dx=-r2;dx<=r2;dx++)
            {
                // special case for no shift -> no difference -> weight 1
                // (but it is not any faster than the full code...)

                if (dx==0 && dy==0 && imageIdx==0 && 0) {
#pragma omp parallel for
                    for (int y=n2;y<h-n+n2;y++) {
                        for (int x=n2;x<w-n+n2;x++) {
                            tmp_data[y*w+x].weightSum += 1;
                            tmp_data[y*w+x].pixelSum  += inWithBorderP[y*inStride+x];
                        }
                    }

                    continue;
                }

                buildIntegralImage_SSE(integral,integral_stride32,
                                       currentWithBorderP, currentStride,
                                       inWithBorderP, inStride,
                                       w,h,
                                       dx,dy);

#pragma omp parallel for
                for (int y=0;y<=h-n;y++) {
                    const uint32_t* iPtr1 = integral+(y  -1)*integral_stride32-1;
                    const uint32_t* iPtr2 = integral+(y+n-1)*integral_stride32-1;

                    for (int x=0;x<=w-n;x++) {
                        const int xc = x+n2;
                        const int yc = y+n2;

                        int diff = (uint32_t)(iPtr2[n] - iPtr2[0] - iPtr1[n] + iPtr1[0]);

                        if (diff<diff_max) {
                            int diffidx = diff*weightFactTab;

                            //float weight = exp(-diff*weightFact);
                            float weight = exptab[diffidx];

                            tmp_data[yc*w+xc].weightSum += weight;
                            tmp_data[yc*w+xc].pixelSum  += weight * inWithBorderP[(yc+dy)*inStride+xc+dx];
                        }

                        iPtr1++;
                        iPtr2++;
                    }
                }
            }
    }



    // --- fill output image ---

    // copy border area

    {
        const uint8_t* in = images[0]->img;
        int origInStride  = images[0]->stride;

        for (int y=0;   y<n2;y++) { memcpy(out+y*outStride, in+y*origInStride, w); }
        for (int y=h-n2;y<h ;y++) { memcpy(out+y*outStride, in+y*origInStride, w); }
        for (int y=n2;y<h-n2;y++) {
            memcpy(out+y*outStride,      in+y*origInStride,      n2);
            memcpy(out+y*outStride+w-n2, in+y*origInStride+w-n2, n2);
        }
    }

    // output main image

    for (int y=n2;y<h-n2;y++) {
        for (int x=n2;x<w-n2;x++) {
            *(out+y*outStride+x) = tmp_data[y*w+x].pixelSum / tmp_data[y*w+x].weightSum;
        }
    }

    free(tmp_data);
    free(integral_mem);
}


static void NLMeans_color_auto(uint8_t** out, int* outStride,
			       const ColorImage* img, // function takes ownership
			       NLMContext* ctx)
{
    assert(ctx->param.n_frames >= 1);
    assert(ctx->param.n_frames <= DN_MAX_NLMeansImages);

    // free oldest image

    free_color_image(&ctx->images[ctx->param.n_frames-1]);

    // shift old images one down and put new image into entry [0]

    for (int i=ctx->param.n_frames-1; i>0; i--) {
        ctx->images[i] = ctx->images[i-1];
        ctx->image_available[i] = ctx->image_available[i-1];
    }

    ctx->images[0] = *img;
    ctx->image_available[0] = 1;


    // process color planes separately

    for (int c=0;c<3;c++)
        if (ctx->images[0].plane[c].img != NULL)
        {
            const MonoImage* images[MAX_NLMeansImages];
            int i;
            for (i=0; ctx->image_available[i]; i++) {
                images[i] = &ctx->images[i].plane[c];
            }

            NLMeans_mono_multi(out[c], outStride[c],
                               images, i, &ctx->param);
        }
}







static av_cold int init(AVFilterContext *ctx)
{
    NLMContext *nlm = ctx->priv;

    for (int i=0;i<MAX_NLMeansImages;i++) {
        nlm->image_available[i] = 0;
    }


    // TODO: choose computation function based on CPU capabilities

    nlm->buildIntegralImage = buildIntegralImage_SSE;
    // buildIntegralImage_scalar // fallback


    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    NLMContext *nlm = ctx->priv;

    for (int i=0;i<MAX_NLMeansImages;i++) {
        if (nlm->image_available[i]) {
            free_color_image(&(nlm->images[i]));

            nlm->image_available[i] = 0;
        }
    }
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_YUV440P,

        AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUVJ422P,
        AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_YUVJ440P,

        AV_PIX_FMT_NONE
    };

    ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));

    return 0;
}

static int config_input(AVFilterLink *inlink)
{
    NLMContext *nlm = inlink->dst->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    nlm->hsub  = desc->log2_chroma_w;
    nlm->vsub  = desc->log2_chroma_h;

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    NLMContext *nlm = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];

    ColorImage borderedImg;

    AVFrame *out;
    int direct, c;

    if (av_frame_is_writable(in)) {
        direct = 1;
        out = in;
    } else {
        direct = 0;
        out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }

        av_frame_copy_props(out, in);
    }

    for (c = 0; c < 3; c++) {
        int w = FF_CEIL_RSHIFT(in->width,  (!!c * nlm->hsub));
        int h = FF_CEIL_RSHIFT(in->height, (!!c * nlm->vsub));
        int border = nlm->param.range/2;

        alloc_and_copy_image_with_border(&borderedImg.plane[c],
                                         in->data[c], in->linesize[c],
                                         w,h,border);
    }

    NLMeans_color_auto(out->data, out->linesize,
		       &borderedImg,
		       nlm);


    if (!direct)
        av_frame_free(&in);

    return ff_filter_frame(outlink, out);
}

#define OFFSET(x) offsetof(NLMContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM
static const AVOption options[] = {
    { "h",           "averaging weight decay parameter", OFFSET(param.h_param),    AV_OPT_TYPE_DOUBLE, { .dbl = 8.0 }, 0.1, 100.0, FLAGS },
    { "patchsize",   "patch width/height",               OFFSET(param.patch_size), AV_OPT_TYPE_INT,    { .i64 = 7   },   3, 255,   FLAGS },
    { "range",       "search range",                     OFFSET(param.range),      AV_OPT_TYPE_INT,    { .i64 = 3   },   3, 255,   FLAGS },
    { "temporal",    "temporal search range",            OFFSET(param.n_frames),   AV_OPT_TYPE_INT,    { .i64 = 2   },   1, MAX_NLMeansImages,   FLAGS },
    { NULL },
};


static const AVClass nlm_class = {
    .class_name = "nlm",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};


static const AVFilterPad avfilter_vf_nlm_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input,
        .filter_frame = filter_frame,
    },
    { NULL }
};


static const AVFilterPad avfilter_vf_nlm_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO
    },
    { NULL }
};

AVFilter avfilter_vf_nlmeans = {
    .name          = "nlmeans",
    .description   = NULL_IF_CONFIG_SMALL("Apply a Non-Local Means filter."),

    .priv_size     = sizeof(NLMContext),
    .priv_class    = &nlm_class,
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,

    .inputs    = avfilter_vf_nlm_inputs,
    .outputs   = avfilter_vf_nlm_outputs,
};
