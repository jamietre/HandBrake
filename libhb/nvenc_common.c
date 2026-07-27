/* nvenc_common.c
 *
 * Copyright (c) 2003-2026 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "handbrake/hbffmpeg.h"
#include "handbrake/nvenc_common.h"
#include "handbrake/handbrake.h"

#if HB_PROJECT_FEATURE_NVENC
#include <ffnvcodec/nvEncodeAPI.h>
#include <ffnvcodec/dynlink_loader.h>
#endif

static int is_nvenc_available = -1;

typedef struct {
    int has_h264;
    int has_h264_10bit;
    int has_hevc;
    int has_av1;
    int cc_major; // CUDA compute capability; 0 if it couldn't be queried
    int cc_minor;
} nvenc_caps_t;

// Per-device capabilities, so device selection can be codec-aware instead
// of always steering toward whichever device supports AV1. cc_major/minor
// are unused in .agg (there's no single "compute capability" for an
// aggregate across devices) but sharing one struct type keeps this simple.
//
// Everything the probe computes lives in one struct, published via a
// single assignment (see hb_nvenc_probe_caps) rather than as separate
// globals -- multiple independent stores have no ordering guarantee
// relative to each other on a weaker memory model (e.g. this project's
// ARM64 CI builds), so a concurrent first caller could otherwise observe
// .probed before the data it gates is actually visible.
#define HB_NVENC_MAX_DEVICES 32
typedef struct {
    int          probed;
    int          dev_count;
    nvenc_caps_t dev_caps[HB_NVENC_MAX_DEVICES];
    nvenc_caps_t agg;
} nvenc_probe_result_t;

static nvenc_probe_result_t g_nvenc = { 0 };

int hb_check_nvenc_available()
{
    if (hb_is_hardware_disabled())
    {
        return 0;
    }

    if (is_nvenc_available != -1)
    {
        return is_nvenc_available;
    }

    #if HB_PROJECT_FEATURE_NVENC
        uint32_t nvenc_ver;
        void *context = NULL;
        NvencFunctions *nvenc_dl = NULL;

        int loadErr = nvenc_load_functions(&nvenc_dl, context);
        if (loadErr < 0)
        {
            is_nvenc_available = 0;
            return 0;
        }

        NVENCSTATUS apiErr = nvenc_dl->NvEncodeAPIGetMaxSupportedVersion(&nvenc_ver);
        if (apiErr != NV_ENC_SUCCESS)
        {
            is_nvenc_available = 0;
            hb_log("nvenc: not available");
            return 0;
        }
        else
        {
            hb_log("nvenc: version %d.%d is available", nvenc_ver >> 4, nvenc_ver & 0xf);
            is_nvenc_available = 1;

            if (hb_check_nvdec_available())
            {
                hb_log("nvdec: is available");
            }
            else
            {
                hb_log("nvdec: is not compiled into this build");
            }
            return 1;
        }

        return 1;
    #else
        is_nvenc_available = 0;
        hb_log("nvenc: not available");
        return 0;
    #endif
}

#if HB_PROJECT_FEATURE_NVENC
static int hb_nvenc_probe_cap(NV_ENCODE_API_FUNCTION_LIST *fl, void *enc,
                              GUID guid, NV_ENC_CAPS cap)
{
    NV_ENC_CAPS_PARAM p;
    memset(&p, 0, sizeof(p));
    p.version     = NV_ENC_CAPS_PARAM_VER;
    p.capsToQuery = cap;
    int val = 0;
    return (fl->nvEncGetEncodeCaps(enc, guid, &p, &val) == NV_ENC_SUCCESS) ? val : 0;
}

static int hb_nvenc_guid_eq(GUID a, GUID b)
{
    return memcmp(&a, &b, sizeof(GUID)) == 0;
}

// Probes a single CUDA device and fills *caps with its encode GUIDs.
// *caps must be a fresh per-device struct; the caller ORs results across
// devices so that a GPU lacking a codec (e.g. Ampere without AV1) can't
// clear flags another GPU already set.
static void hb_nvenc_probe_device_caps(CudaFunctions *cu, NvencFunctions *nv,
                                        CUdevice dev, nvenc_caps_t *caps)
{
    CUcontext ctx = NULL;
    void     *enc = NULL;
    GUID     *guids = NULL;
    NV_ENCODE_API_FUNCTION_LIST fl;

    memset(&fl, 0, sizeof(fl));
    fl.version = NV_ENCODE_API_FUNCTION_LIST_VER;

    if (cu->cuCtxCreate(&ctx, 0, dev) != CUDA_SUCCESS)
    {
        return;
    }
    if (nv->NvEncodeAPICreateInstance(&fl) != NV_ENC_SUCCESS)
    {
        goto done;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sess;
    memset(&sess, 0, sizeof(sess));
    sess.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sess.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    sess.device     = ctx;
    sess.apiVersion = NVENCAPI_VERSION;

    if (fl.nvEncOpenEncodeSessionEx(&sess, &enc) != NV_ENC_SUCCESS || enc == NULL)
    {
        goto done;
    }

    uint32_t count = 0;
    if (fl.nvEncGetEncodeGUIDCount(enc, &count) != NV_ENC_SUCCESS || count == 0)
    {
        goto done;
    }

    guids = (GUID *)calloc(count, sizeof(GUID));
    if (guids == NULL)
    {
        goto done;
    }

    uint32_t got = 0;
    if (fl.nvEncGetEncodeGUIDs(enc, guids, count, &got) != NV_ENC_SUCCESS)
    {
        goto done;
    }

    for (uint32_t i = 0; i < got; i++)
    {
        if (hb_nvenc_guid_eq(guids[i], NV_ENC_CODEC_H264_GUID))
        {
            caps->has_h264       = 1;
            caps->has_h264_10bit = hb_nvenc_probe_cap(
                &fl, enc, NV_ENC_CODEC_H264_GUID,
                NV_ENC_CAPS_SUPPORT_10BIT_ENCODE) > 0;
        }
        else if (hb_nvenc_guid_eq(guids[i], NV_ENC_CODEC_HEVC_GUID))
        {
            caps->has_hevc = 1;
        }
        else if (hb_nvenc_guid_eq(guids[i], NV_ENC_CODEC_AV1_GUID))
        {
            caps->has_av1 = 1;
        }
    }

done:
    free(guids);
    if (enc != NULL)
    {
        fl.nvEncDestroyEncoder(enc);
    }
    cu->cuCtxDestroy(ctx);
}
#endif

static void hb_nvenc_probe_caps(void)
{
    if (g_nvenc.probed)
    {
        return;
    }

    nvenc_probe_result_t result = { 0 };

    if (hb_check_nvenc_available())
    {
#if HB_PROJECT_FEATURE_NVENC
        CudaFunctions  *cu = NULL;
        NvencFunctions *nv = NULL;

        if (cuda_load_functions(&cu, NULL) < 0)
        {
            goto done;
        }
        if (cu->cuInit(0) != CUDA_SUCCESS)
        {
            goto done;
        }
        if (nvenc_load_functions(&nv, NULL) < 0)
        {
            goto done;
        }

        int dev_count = 0;
        cu->cuDeviceGetCount(&dev_count);
        if (dev_count > HB_NVENC_MAX_DEVICES)
        {
            dev_count = HB_NVENC_MAX_DEVICES;
        }
        result.dev_count = dev_count;

        // Every installed GPU can support a different codec set (e.g. an
        // Ampere card with no AV1 encode alongside a Blackwell card that
        // has it), so every device is probed individually and kept in
        // result.dev_caps -- callers pick a device based on the codec
        // they actually need (hb_nvenc_device_index_for_codec), not just
        // whichever device happens to be the most capable overall.
        for (int i = 0; i < dev_count; i++)
        {
            CUdevice dev;
            if (cu->cuDeviceGet(&dev, i) != CUDA_SUCCESS)
            {
                continue;
            }

            nvenc_caps_t dev_caps = { 0 };
            hb_nvenc_probe_device_caps(cu, nv, dev, &dev_caps);
            cu->cuDeviceComputeCapability(&dev_caps.cc_major,
                                           &dev_caps.cc_minor, dev);

            result.dev_caps[i] = dev_caps;

            result.agg.has_h264       |= dev_caps.has_h264;
            result.agg.has_h264_10bit |= dev_caps.has_h264_10bit;
            result.agg.has_hevc       |= dev_caps.has_hevc;
            result.agg.has_av1        |= dev_caps.has_av1;
        }

        hb_log("nvenc: caps probe -> h264=%d h264_10bit=%d hevc=%d av1=%d",
               result.agg.has_h264, result.agg.has_h264_10bit,
               result.agg.has_hevc, result.agg.has_av1);

done:
        nvenc_free_functions(&nv);
        cuda_free_functions(&cu);
#endif
    }

    // Publish everything in one assignment: the array, count, aggregate,
    // and probed flag are all fields of the same struct, so there's no
    // window where a concurrent first caller could observe .probed
    // without the data it gates also being visible.
    result.probed = 1;
    g_nvenc = result;
}

static int hb_nvenc_dev_supports_codec(const nvenc_caps_t *caps, int vcodec)
{
    switch (vcodec)
    {
        case HB_VCODEC_FFMPEG_NVENC_H264:
            return caps->has_h264;
        case HB_VCODEC_FFMPEG_NVENC_H264_10BIT:
            return caps->has_h264_10bit;
        case HB_VCODEC_FFMPEG_NVENC_H265:
        case HB_VCODEC_FFMPEG_NVENC_H265_10BIT:
            return caps->has_hevc;
        case HB_VCODEC_FFMPEG_NVENC_AV1:
        case HB_VCODEC_FFMPEG_NVENC_AV1_10BIT:
            return caps->has_av1;
        default:
            return 0;
    }
}

// Returns the index of the CUDA device best suited to encode vcodec, or -1
// if no installed device supports it (or NVENC isn't available), so
// callers fall back to whatever CUDA treats as its default device.
//
// A device is only ever returned if it actually supports the requested
// codec -- a device must never be chosen just because it's the newest or
// otherwise most capable overall, since that can route a job to a device
// that can't open it at all (e.g. a newer card that dropped a legacy
// profile an older card still has). Among devices that do support it, the
// one with the highest CUDA compute capability (newest architecture) is
// preferred.
int hb_nvenc_device_index_for_codec(int vcodec)
{
    hb_nvenc_probe_caps();

    int best_index = -1;
    int best_major = -1;
    int best_minor = -1;

    for (int i = 0; i < g_nvenc.dev_count; i++)
    {
        if (!hb_nvenc_dev_supports_codec(&g_nvenc.dev_caps[i], vcodec))
        {
            continue;
        }
        if (best_index == -1 ||
            g_nvenc.dev_caps[i].cc_major > best_major ||
            (g_nvenc.dev_caps[i].cc_major == best_major &&
             g_nvenc.dev_caps[i].cc_minor > best_minor))
        {
            best_index = i;
            best_major = g_nvenc.dev_caps[i].cc_major;
            best_minor = g_nvenc.dev_caps[i].cc_minor;
        }
    }

    return best_index;
}

int hb_nvenc_h264_available()
{
    hb_nvenc_probe_caps();
    return g_nvenc.agg.has_h264;
}

int hb_nvenc_h264_10bit_available()
{
    hb_nvenc_probe_caps();
    return g_nvenc.agg.has_h264_10bit;
}

int hb_nvenc_h265_available()
{
    hb_nvenc_probe_caps();
    return g_nvenc.agg.has_hevc;
}

int hb_nvenc_av1_available()
{
    hb_nvenc_probe_caps();
    return g_nvenc.agg.has_av1;
}

int hb_check_nvdec_available()
{
    #if HB_PROJECT_FEATURE_NVDEC
        return 1;
    #else
        return 0;
    #endif
}

const char * hb_map_nvenc_preset_name (const char * preset)
{
    if (preset == NULL)
    {
        return "p4";
    }

    if (strcmp(preset, "fastest") == 0) {
      return "p1";
    }  else if (strcmp(preset, "faster") == 0) {
      return "p2";
    } else if (strcmp(preset, "fast") == 0) {
       return "p3";
    } else if (strcmp(preset, "medium") == 0) {
      return "p4";
    } else if (strcmp(preset, "slow") == 0) {
      return "p5";
    } else if (strcmp(preset, "slower") == 0) {
       return "p6";
    } else if (strcmp(preset, "slowest") == 0) {
      return "p7";
    }

    return "p4"; // Default to Medium
}

static int hb_nvenc_are_filters_supported(hb_list_t *filters)
{
    int ret = 1;

    for (int i = 0; i < hb_list_count(filters); i++)
    {
        int supported = 1;
        hb_filter_object_t *filter = hb_list_item(filters, i);

        switch (filter->id)
        {
            case HB_FILTER_VFR:
                // Mode 0 doesn't require access to the frame data
                supported = hb_dict_get_int(filter->settings, "mode") == 0;
                break;
            case HB_FILTER_FORMAT:
            case HB_FILTER_AVFILTER:
                break;
            default:
                supported = 0;
        }

        if (supported == 0)
        {
            hb_deep_log(2, "hwaccel: %s isn't yet supported for hw video frames", filter->name);
            ret = 0;
        }
    }

    return ret;
}

static const int nv_encoders[] =
{
    HB_VCODEC_FFMPEG_NVENC_H264, HB_VCODEC_FFMPEG_NVENC_H264_10BIT,
    HB_VCODEC_FFMPEG_NVENC_H265, HB_VCODEC_FFMPEG_NVENC_H265_10BIT,
    HB_VCODEC_FFMPEG_NVENC_AV1, HB_VCODEC_FFMPEG_NVENC_AV1_10BIT,
    HB_VCODEC_INVALID
};

hb_hwaccel_t hb_hwaccel_nvdec =
{
    .id         = HB_DECODE_NVDEC,
    .name       = "nvdec hwaccel",
    .encoders   = nv_encoders,
    .type       = AV_HWDEVICE_TYPE_CUDA,
    .hw_pix_fmt = AV_PIX_FMT_CUDA,
    .can_filter = hb_nvenc_are_filters_supported,
    .get_device_index_for_codec = hb_nvenc_device_index_for_codec,
    .caps       = HB_HWACCEL_CAP_SCAN | HB_HWACCEL_CAP_COLOR_RANGE
};
