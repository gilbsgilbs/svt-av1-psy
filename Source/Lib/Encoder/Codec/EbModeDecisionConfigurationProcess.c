/*
* Copyright(c) 2019 Intel Corporation
* Copyright (c) 2016, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#include <stdlib.h>

#include "EbEncHandle.h"
#include "EbUtility.h"
#include "EbPictureControlSet.h"
#include "EbModeDecisionConfigurationProcess.h"
#include "EbRateControlResults.h"
#include "EbEncDecTasks.h"
#include "EbReferenceObject.h"
#include "EbModeDecisionProcess.h"
#include "av1me.h"
#include "EbQMatrices.h"
#include "EbLog.h"
#include "EbCoefficients.h"
#include "EbCommonUtils.h"
#include "EbResize.h"
#include "EbInvTransforms.h"
uint8_t get_bypass_encdec(EncMode enc_mode, uint8_t hbd_md, uint8_t encoder_bit_depth);

#define MAX_MESH_SPEED 5 // Max speed setting for mesh motion method
static MeshPattern good_quality_mesh_patterns[MAX_MESH_SPEED + 1][MAX_MESH_STEP] = {
    {{64, 8}, {28, 4}, {15, 1}, {7, 1}},
    {{64, 8}, {28, 4}, {15, 1}, {7, 1}},
    {{64, 8}, {14, 2}, {7, 1}, {7, 1}},
    {{64, 16}, {24, 8}, {12, 4}, {7, 1}},
    {{64, 16}, {24, 8}, {12, 4}, {7, 1}},
    {{64, 16}, {24, 8}, {12, 4}, {7, 1}},
};
static unsigned char good_quality_max_mesh_pct[MAX_MESH_SPEED + 1] = {50, 50, 25, 15, 5, 1};
// TODO: These settings are pretty relaxed, tune them for
// each speed setting
static MeshPattern intrabc_mesh_patterns[MAX_MESH_SPEED + 1][MAX_MESH_STEP] = {
    {{256, 1}, {256, 1}, {0, 0}, {0, 0}},
    {{256, 1}, {256, 1}, {0, 0}, {0, 0}},
    {{64, 1}, {64, 1}, {0, 0}, {0, 0}},
    {{64, 1}, {64, 1}, {0, 0}, {0, 0}},
    {{64, 4}, {16, 1}, {0, 0}, {0, 0}},
    {{64, 4}, {16, 1}, {0, 0}, {0, 0}},
};
static uint8_t intrabc_max_mesh_pct[MAX_MESH_SPEED + 1] = {100, 100, 100, 25, 25, 10};
void           set_global_motion_field(PictureControlSet *pcs) {
    // Init Global Motion Vector
    uint8_t frame_index;
    for (frame_index = INTRA_FRAME; frame_index <= ALTREF_FRAME; ++frame_index) {
        pcs->ppcs->global_motion[frame_index].wmtype   = IDENTITY;
        pcs->ppcs->global_motion[frame_index].alpha    = 0;
        pcs->ppcs->global_motion[frame_index].beta     = 0;
        pcs->ppcs->global_motion[frame_index].delta    = 0;
        pcs->ppcs->global_motion[frame_index].gamma    = 0;
        pcs->ppcs->global_motion[frame_index].invalid  = 0;
        pcs->ppcs->global_motion[frame_index].wmmat[0] = 0;
        pcs->ppcs->global_motion[frame_index].wmmat[1] = 0;
        pcs->ppcs->global_motion[frame_index].wmmat[2] = (1 << WARPEDMODEL_PREC_BITS);
        pcs->ppcs->global_motion[frame_index].wmmat[3] = 0;
        pcs->ppcs->global_motion[frame_index].wmmat[4] = 0;
        pcs->ppcs->global_motion[frame_index].wmmat[5] = (1 << WARPEDMODEL_PREC_BITS);
        pcs->ppcs->global_motion[frame_index].wmmat[6] = 0;
        pcs->ppcs->global_motion[frame_index].wmmat[7] = 0;
    }

    //Update MV
    PictureParentControlSet *ppcs = pcs->ppcs;
    for (frame_index = INTRA_FRAME; frame_index <= ALTREF_FRAME; ++frame_index) {
        if (ppcs->is_global_motion[get_list_idx(frame_index)][get_ref_frame_idx(frame_index)])
            ppcs->global_motion[frame_index] = ppcs->global_motion_estimation[get_list_idx(
                frame_index)][get_ref_frame_idx(frame_index)];

        // Upscale the translation parameters by 2, because the search is done on a down-sampled
        // version of the source picture (with a down-sampling factor of 2 in each dimension).
        if (ppcs->gm_downsample_level == GM_DOWN16) {
            ppcs->global_motion[frame_index].wmmat[0] *= 4;
            ppcs->global_motion[frame_index].wmmat[1] *= 4;
            ppcs->global_motion[frame_index].wmmat[0] = (int32_t)clamp(
                ppcs->global_motion[frame_index].wmmat[0],
                GM_TRANS_MIN * GM_TRANS_DECODE_FACTOR,
                GM_TRANS_MAX * GM_TRANS_DECODE_FACTOR);
            ppcs->global_motion[frame_index].wmmat[1] = (int32_t)clamp(
                ppcs->global_motion[frame_index].wmmat[1],
                GM_TRANS_MIN * GM_TRANS_DECODE_FACTOR,
                GM_TRANS_MAX * GM_TRANS_DECODE_FACTOR);
        } else if (ppcs->gm_downsample_level == GM_DOWN) {
            ppcs->global_motion[frame_index].wmmat[0] *= 2;
            ppcs->global_motion[frame_index].wmmat[1] *= 2;
            ppcs->global_motion[frame_index].wmmat[0] = (int32_t)clamp(
                ppcs->global_motion[frame_index].wmmat[0],
                GM_TRANS_MIN * GM_TRANS_DECODE_FACTOR,
                GM_TRANS_MAX * GM_TRANS_DECODE_FACTOR);
            ppcs->global_motion[frame_index].wmmat[1] = (int32_t)clamp(
                ppcs->global_motion[frame_index].wmmat[1],
                GM_TRANS_MIN * GM_TRANS_DECODE_FACTOR,
                GM_TRANS_MAX * GM_TRANS_DECODE_FACTOR);
        }
    }
}

void svt_av1_build_quantizer(EbBitDepth bit_depth, int32_t y_dc_delta_q, int32_t u_dc_delta_q,
                             int32_t u_ac_delta_q, int32_t v_dc_delta_q, int32_t v_ac_delta_q,
                             Quants *const quants, Dequants *const deq) {
    int32_t i, q, quant_qtx;

    for (q = 0; q < QINDEX_RANGE; q++) {
        const int32_t qzbin_factor     = get_qzbin_factor(q, bit_depth);
        const int32_t qrounding_factor = q == 0 ? 64 : 48;

        for (i = 0; i < 2; ++i) {
            int32_t qrounding_factor_fp = 64;
            quant_qtx                   = i == 0 ? svt_aom_dc_quant_qtx(q, y_dc_delta_q, bit_depth)
                                                 : svt_aom_ac_quant_qtx(q, 0, bit_depth);
            invert_quant(&quants->y_quant[q][i], &quants->y_quant_shift[q][i], quant_qtx);
            quants->y_quant_fp[q][i] = (int16_t)((1 << 16) / quant_qtx);
            quants->y_round_fp[q][i] = (int16_t)((qrounding_factor_fp * quant_qtx) >> 7);
            quants->y_zbin[q][i]     = (int16_t)ROUND_POWER_OF_TWO(qzbin_factor * quant_qtx, 7);
            quants->y_round[q][i]    = (int16_t)((qrounding_factor * quant_qtx) >> 7);
            deq->y_dequant_qtx[q][i] = (int16_t)quant_qtx;
            quant_qtx                = i == 0 ? svt_aom_dc_quant_qtx(q, u_dc_delta_q, bit_depth)
                                              : svt_aom_ac_quant_qtx(q, u_ac_delta_q, bit_depth);
            invert_quant(&quants->u_quant[q][i], &quants->u_quant_shift[q][i], quant_qtx);
            quants->u_quant_fp[q][i] = (int16_t)((1 << 16) / quant_qtx);
            quants->u_round_fp[q][i] = (int16_t)((qrounding_factor_fp * quant_qtx) >> 7);
            quants->u_zbin[q][i]     = (int16_t)ROUND_POWER_OF_TWO(qzbin_factor * quant_qtx, 7);
            quants->u_round[q][i]    = (int16_t)((qrounding_factor * quant_qtx) >> 7);
            deq->u_dequant_qtx[q][i] = (int16_t)quant_qtx;
            quant_qtx                = i == 0 ? svt_aom_dc_quant_qtx(q, v_dc_delta_q, bit_depth)
                                              : svt_aom_ac_quant_qtx(q, v_ac_delta_q, bit_depth);
            invert_quant(&quants->v_quant[q][i], &quants->v_quant_shift[q][i], quant_qtx);
            quants->v_quant_fp[q][i] = (int16_t)((1 << 16) / quant_qtx);
            quants->v_round_fp[q][i] = (int16_t)((qrounding_factor_fp * quant_qtx) >> 7);
            quants->v_zbin[q][i]     = (int16_t)ROUND_POWER_OF_TWO(qzbin_factor * quant_qtx, 7);
            quants->v_round[q][i]    = (int16_t)((qrounding_factor * quant_qtx) >> 7);
            deq->v_dequant_qtx[q][i] = (int16_t)quant_qtx;
        }

        for (i = 2; i < 8; i++) { // 8: SIMD width
            quants->y_quant[q][i]       = quants->y_quant[q][1];
            quants->y_quant_fp[q][i]    = quants->y_quant_fp[q][1];
            quants->y_round_fp[q][i]    = quants->y_round_fp[q][1];
            quants->y_quant_shift[q][i] = quants->y_quant_shift[q][1];
            quants->y_zbin[q][i]        = quants->y_zbin[q][1];
            quants->y_round[q][i]       = quants->y_round[q][1];
            deq->y_dequant_qtx[q][i]    = deq->y_dequant_qtx[q][1];

            quants->u_quant[q][i]       = quants->u_quant[q][1];
            quants->u_quant_fp[q][i]    = quants->u_quant_fp[q][1];
            quants->u_round_fp[q][i]    = quants->u_round_fp[q][1];
            quants->u_quant_shift[q][i] = quants->u_quant_shift[q][1];
            quants->u_zbin[q][i]        = quants->u_zbin[q][1];
            quants->u_round[q][i]       = quants->u_round[q][1];
            deq->u_dequant_qtx[q][i]    = deq->u_dequant_qtx[q][1];
            quants->v_quant[q][i]       = quants->u_quant[q][1];
            quants->v_quant_fp[q][i]    = quants->v_quant_fp[q][1];
            quants->v_round_fp[q][i]    = quants->v_round_fp[q][1];
            quants->v_quant_shift[q][i] = quants->v_quant_shift[q][1];
            quants->v_zbin[q][i]        = quants->v_zbin[q][1];
            quants->v_round[q][i]       = quants->v_round[q][1];
            deq->v_dequant_qtx[q][i]    = deq->v_dequant_qtx[q][1];
        }
    }
}

// Reduce the large number of quantizers to a smaller number of levels for which
// different matrices may be defined
static INLINE int aom_get_qmlevel(int qindex, int first, int last) {
    // mapping qindex(0, 255) to QM level(first, last)
    return first + (qindex * (last + 1 - first)) / QINDEX_RANGE;
}

void svt_av1_qm_init(PictureParentControlSet *pcs) {
    const uint8_t num_planes = 3; // MAX_MB_PLANE;// NM- No monochroma
    uint8_t       q, c, t;
    int32_t       current;
    for (q = 0; q < NUM_QM_LEVELS; ++q) {
        for (c = 0; c < num_planes; ++c) {
            current = 0;
            for (t = 0; t < TX_SIZES_ALL; ++t) {
                const int32_t size       = tx_size_2d[t];
                const TxSize  qm_tx_size = av1_get_adjusted_tx_size(t);
                if (q == NUM_QM_LEVELS - 1) {
                    pcs->gqmatrix[q][c][t]  = NULL;
                    pcs->giqmatrix[q][c][t] = NULL;
                } else if (t != qm_tx_size) { // Reuse matrices for 'qm_tx_size'
                    pcs->gqmatrix[q][c][t]  = pcs->gqmatrix[q][c][qm_tx_size];
                    pcs->giqmatrix[q][c][t] = pcs->giqmatrix[q][c][qm_tx_size];
                } else {
                    assert(current + size <= QM_TOTAL_SIZE);
                    pcs->gqmatrix[q][c][t]  = &wt_matrix_ref[q][c >= 1][current];
                    pcs->giqmatrix[q][c][t] = &iwt_matrix_ref[q][c >= 1][current];
                    current += size;
                }
            }
        }
    }

    if (pcs->frm_hdr.quantization_params.using_qmatrix) {
        const int32_t min_qmlevel = pcs->scs->static_config.min_qm_level;
        const int32_t max_qmlevel = pcs->scs->static_config.max_qm_level;
        const int32_t base_qindex = pcs->frm_hdr.quantization_params.base_q_idx;

        pcs->frm_hdr.quantization_params.qm[AOM_PLANE_Y] = aom_get_qmlevel(
            base_qindex, min_qmlevel, max_qmlevel);
        pcs->frm_hdr.quantization_params.qm[AOM_PLANE_U] = aom_get_qmlevel(
            base_qindex + pcs->frm_hdr.quantization_params.delta_q_ac[AOM_PLANE_U],
            min_qmlevel,
            max_qmlevel);
        pcs->frm_hdr.quantization_params.qm[AOM_PLANE_V] = aom_get_qmlevel(
            base_qindex + pcs->frm_hdr.quantization_params.delta_q_ac[AOM_PLANE_V],
            min_qmlevel,
            max_qmlevel);
#if DEBUG_QM_LEVEL
        SVT_LOG("\n[svt_av1_qm_init] Frame %d - qindex %d, qmlevel %d %d %d\n",
                (int)pcs->picture_number,
                base_qindex,
                pcs->frm_hdr.quantization_params.qm[AOM_PLANE_Y],
                pcs->frm_hdr.quantization_params.qm[AOM_PLANE_U],
                pcs->frm_hdr.quantization_params.qm[AOM_PLANE_V]);
#endif
    }
}

/******************************************************
* Set the reference sg ep for a given picture
******************************************************/
void set_reference_sg_ep(PictureControlSet *pcs) {
    Av1Common         *cm = pcs->ppcs->av1_cm;
    EbReferenceObject *ref_obj_l0, *ref_obj_l1;
    memset(cm->sg_frame_ep_cnt, 0, SGRPROJ_PARAMS * sizeof(int32_t));
    cm->sg_frame_ep = 0;

    // NADER: set cm->sg_ref_frame_ep[0] = cm->sg_ref_frame_ep[1] = -1 to perform all iterations
    switch (pcs->slice_type) {
    case I_SLICE:
        cm->sg_ref_frame_ep[0] = -1;
        cm->sg_ref_frame_ep[1] = -1;
        break;
    case B_SLICE:
        ref_obj_l0 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
        ref_obj_l1 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
        cm->sg_ref_frame_ep[0] = ref_obj_l0->sg_frame_ep;
        cm->sg_ref_frame_ep[1] = ref_obj_l1->sg_frame_ep;
        break;
    case P_SLICE:
        ref_obj_l0 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
        cm->sg_ref_frame_ep[0] = ref_obj_l0->sg_frame_ep;
        cm->sg_ref_frame_ep[1] = 0;
        break;
    default: SVT_LOG("SG: Not supported picture type"); break;
    }
}

void mode_decision_configuration_init_qp_update(PictureControlSet *pcs) {
    FrameHeader *frm_hdr  = &pcs->ppcs->frm_hdr;
    pcs->intra_coded_area = 0;
    pcs->skip_coded_area  = 0;
    // Init block selection
    // Set reference sg ep
    set_reference_sg_ep(pcs);
    set_global_motion_field(pcs);

    svt_av1_qm_init(pcs->ppcs);
    MdRateEstimationContext *md_rate_estimation_array;

    md_rate_estimation_array = pcs->md_rate_estimation_array;

    if (pcs->ppcs->frm_hdr.primary_ref_frame != PRIMARY_REF_NONE)
        memcpy(&pcs->md_frame_context,
               &pcs->ref_frame_context[pcs->ppcs->frm_hdr.primary_ref_frame],
               sizeof(FRAME_CONTEXT));
    else {
        svt_av1_default_coef_probs(&pcs->md_frame_context, frm_hdr->quantization_params.base_q_idx);
        init_mode_probs(&pcs->md_frame_context);
    }
    // Initial Rate Estimation of the syntax elements
    av1_estimate_syntax_rate(md_rate_estimation_array,
                             pcs->slice_type == I_SLICE ? TRUE : FALSE,
                             pcs->pic_filter_intra_level,
                             pcs->ppcs->frm_hdr.allow_screen_content_tools,
                             pcs->ppcs->enable_restoration,
                             pcs->ppcs->frm_hdr.allow_intrabc,
                             pcs->ppcs->partition_contexts,
                             &pcs->md_frame_context);
    // Initial Rate Estimation of the Motion vectors
    av1_estimate_mv_rate(pcs, md_rate_estimation_array, &pcs->md_frame_context);
    // Initial Rate Estimation of the quantized coefficients
    av1_estimate_coefficients_rate(md_rate_estimation_array, &pcs->md_frame_context);
}

/******************************************************
* Compute Tc, and Beta offsets for a given picture
******************************************************/

static void mode_decision_configuration_context_dctor(EbPtr p) {
    EbThreadContext                  *thread_context_ptr = (EbThreadContext *)p;
    ModeDecisionConfigurationContext *obj                = (ModeDecisionConfigurationContext *)
                                                thread_context_ptr->priv;

    EB_FREE_ARRAY(obj);
}
/******************************************************
 * Mode Decision Configuration Context Constructor
 ******************************************************/
EbErrorType mode_decision_configuration_context_ctor(EbThreadContext   *thread_context_ptr,
                                                     const EbEncHandle *enc_handle_ptr,
                                                     int input_index, int output_index) {
    ModeDecisionConfigurationContext *context_ptr;
    EB_CALLOC_ARRAY(context_ptr, 1);
    thread_context_ptr->priv  = context_ptr;
    thread_context_ptr->dctor = mode_decision_configuration_context_dctor;

    // Input/Output System Resource Manager FIFOs
    context_ptr->rate_control_input_fifo_ptr = svt_system_resource_get_consumer_fifo(
        enc_handle_ptr->rate_control_results_resource_ptr, input_index);
    context_ptr->mode_decision_configuration_output_fifo_ptr =
        svt_system_resource_get_producer_fifo(enc_handle_ptr->enc_dec_tasks_resource_ptr,
                                              output_index);
    return EB_ErrorNone;
}

void set_cdf_controls(PictureControlSet *pcs, uint8_t update_cdf_level) {
    CdfControls *ctrl = &pcs->cdf_ctrl;
    switch (update_cdf_level) {
    case 0:
        ctrl->update_mv   = 0;
        ctrl->update_se   = 0;
        ctrl->update_coef = 0;
        break;
    case 1:
        ctrl->update_mv   = 1;
        ctrl->update_se   = 1;
        ctrl->update_coef = 1;
        break;
    case 2:
        ctrl->update_mv   = 0;
        ctrl->update_se   = 1;
        ctrl->update_coef = 1;
        break;
    case 3:
        ctrl->update_mv   = 0;
        ctrl->update_se   = 1;
        ctrl->update_coef = 0;
        break;
    default: assert(0); break;
    }

    ctrl->update_mv = pcs->slice_type == I_SLICE ? 0 : ctrl->update_mv;
    ctrl->enabled   = ctrl->update_coef | ctrl->update_mv | ctrl->update_se;
}
/******************************************************
* Derive Mode Decision Config Settings for OQ
Input   : encoder mode and tune
Output  : EncDec Kernel signal(s)
******************************************************/
EbErrorType rtime_alloc_ec_ctx_array(PictureControlSet *pcs, uint16_t all_sb) {
    EB_MALLOC_ARRAY(pcs->ec_ctx_array, all_sb);
    return EB_ErrorNone;
}
#if OPT_LD_M11
uint8_t get_nic_level(EncMode enc_mode, uint8_t is_base, uint8_t hierarchical_levels,
                      bool rtc_tune);
#else
uint8_t get_nic_level(EncMode enc_mode, uint8_t is_base, uint8_t hierarchical_levels);
#endif
uint8_t get_update_cdf_level(EncMode enc_mode, SliceType is_islice, uint8_t is_base) {
    uint8_t update_cdf_level = 0;
    if (enc_mode <= ENC_M2)
        update_cdf_level = 1;
    else if (enc_mode <= ENC_M5)
        update_cdf_level = is_base ? 1 : 3;
    else if (enc_mode <= ENC_M10)
        update_cdf_level = is_islice ? 1 : 0;
    else
        update_cdf_level = 0;

    return update_cdf_level;
}

uint8_t svt_aom_get_chroma_level(EncMode enc_mode) {
    uint8_t chroma_level = 0;
    if (enc_mode <= ENC_MRS)
        chroma_level = 1;
    else if (enc_mode <= ENC_M2)
        chroma_level = 2;
    else if (enc_mode <= ENC_M6)
        chroma_level = 3;
    else
        chroma_level = 5;

    return chroma_level;
}

EbErrorType signal_derivation_mode_decision_config_kernel_oq(SequenceControlSet *scs,
                                                             PictureControlSet  *pcs) {
    EbErrorType              return_error        = EB_ErrorNone;
    PictureParentControlSet *ppcs                = pcs->ppcs;
    const EncMode            enc_mode            = pcs->enc_mode;
    const uint8_t            is_ref              = ppcs->is_used_as_reference_flag;
    const uint8_t            is_base             = ppcs->temporal_layer_index == 0;
    const uint8_t            is_layer1           = ppcs->temporal_layer_index == 1;
    const EbInputResolution  input_resolution    = ppcs->input_resolution;
    const uint8_t            is_islice           = pcs->slice_type == I_SLICE;
    const SliceType          slice_type          = pcs->slice_type;
    const Bool               fast_decode         = scs->static_config.fast_decode;
    const uint32_t           hierarchical_levels = scs->static_config.hierarchical_levels;
    const Bool               transition_present  = (ppcs->transition_present == 1);
#if OPT_LD_M13
    const bool rtc_tune = (scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) ? true
                                                                                          : false;
#endif
    //MFMV
    if (is_islice || scs->mfmv_enabled == 0 || pcs->ppcs->frm_hdr.error_resilient_mode) {
        ppcs->frm_hdr.use_ref_frame_mvs = 0;
    } else {
        if (fast_decode == 0) {
            if (enc_mode <= ENC_M9)
                ppcs->frm_hdr.use_ref_frame_mvs = 1;
            else {
                uint64_t avg_me_dist = 0;
                for (uint16_t b64_idx = 0; b64_idx < ppcs->b64_total_count; b64_idx++) {
                    avg_me_dist += ppcs->me_64x64_distortion[b64_idx];
                }
                avg_me_dist /= ppcs->b64_total_count;
                avg_me_dist /= pcs->picture_qp;

                ppcs->frm_hdr.use_ref_frame_mvs = avg_me_dist < 200 ||
                        input_resolution <= INPUT_SIZE_360p_RANGE
                    ? 1
                    : 0;
            }
        } else {
            if (enc_mode <= ENC_M9) {
                uint64_t avg_me_dist = 0;
                for (uint16_t b64_idx = 0; b64_idx < ppcs->b64_total_count; b64_idx++) {
                    avg_me_dist += ppcs->me_64x64_distortion[b64_idx];
                }
                avg_me_dist /= ppcs->b64_total_count;
                avg_me_dist /= pcs->picture_qp;

                ppcs->frm_hdr.use_ref_frame_mvs = avg_me_dist < 50 ||
                        input_resolution <= INPUT_SIZE_360p_RANGE
                    ? 1
                    : 0;
            } else {
                ppcs->frm_hdr.use_ref_frame_mvs = input_resolution <= INPUT_SIZE_360p_RANGE ? 1 : 0;
            }
        }
    }

    uint8_t update_cdf_level = get_update_cdf_level(enc_mode, is_islice, is_base);
    //set the conrols uisng the required level
    set_cdf_controls(pcs, update_cdf_level);

    if (pcs->cdf_ctrl.enabled) {
        const uint16_t picture_sb_w = ppcs->picture_sb_width;
        const uint16_t picture_sb_h = ppcs->picture_sb_height;
        const uint16_t all_sb       = picture_sb_w * picture_sb_h;
        rtime_alloc_ec_ctx_array(pcs, all_sb);
    }
    //Filter Intra Mode : 0: OFF  1: ON
    // pic_filter_intra_level specifies whether filter intra would be active
    // for a given picture.

    // pic_filter_intra_level | Settings
    // 0                      | OFF
    // 1                      | ON
    if (scs->filter_intra_level == DEFAULT) {
        if (scs->seq_header.filter_intra_level) {
            if (pcs->enc_mode <= ENC_M4)
                pcs->pic_filter_intra_level = 1;
            else
                pcs->pic_filter_intra_level = 0;
        } else
            pcs->pic_filter_intra_level = 0;
    } else
        pcs->pic_filter_intra_level = scs->filter_intra_level;

    if (fast_decode == 0 || input_resolution <= INPUT_SIZE_360p_RANGE) {
        if (pcs->enc_mode <= ENC_M5)
            pcs->ppcs->partition_contexts = PARTITION_CONTEXTS;
        else
            pcs->ppcs->partition_contexts = 4;
    } else {
        pcs->ppcs->partition_contexts = 4;
    }
    FrameHeader *frm_hdr             = &ppcs->frm_hdr;
    frm_hdr->allow_high_precision_mv = frm_hdr->quantization_params.base_q_idx <
                HIGH_PRECISION_MV_QTHRESH &&
            (scs->input_resolution <= INPUT_SIZE_480p_RANGE)
        ? 1
        : 0;
    // Set Warped Motion level and enabled flag
    pcs->wm_level = 0;
    if (frm_hdr->frame_type == KEY_FRAME || frm_hdr->frame_type == INTRA_ONLY_FRAME ||
        frm_hdr->error_resilient_mode || pcs->ppcs->frame_superres_enabled ||
        pcs->ppcs->frame_resize_enabled) {
        pcs->wm_level = 0;
    } else {
        if (fast_decode == 0 || input_resolution <= INPUT_SIZE_360p_RANGE) {
            if (enc_mode <= ENC_M3) {
                pcs->wm_level = 1;
            } else if (enc_mode <= ENC_M6) {
                if (hierarchical_levels <= 3)
                    pcs->wm_level = is_base ? 1 : 0;
                else
                    pcs->wm_level = (is_base || is_layer1) ? 1 : 0;
            } else if (enc_mode <= ENC_M7) {
                if (hierarchical_levels <= 4)
                    pcs->wm_level = is_base ? 1 : 0;
                else
                    pcs->wm_level = (is_base || is_layer1) ? 1 : 0;
            } else if (enc_mode <= ENC_M10) {
                if (input_resolution <= INPUT_SIZE_720p_RANGE)
                    pcs->wm_level = is_base ? 1 : 0;
                else
                    pcs->wm_level = is_base ? 2 : 0;
            } else {
                pcs->wm_level = is_base ? 2 : 0;
            }
        } else {
            if (enc_mode <= ENC_M7) {
                pcs->wm_level = is_base ? 1 : 0;
            } else if (enc_mode <= ENC_M10) {
                if (input_resolution <= INPUT_SIZE_720p_RANGE)
                    pcs->wm_level = is_base ? 1 : 0;
                else
                    pcs->wm_level = is_base ? 2 : 0;
            } else {
                pcs->wm_level = is_base ? 2 : 0;
            }
        }
    }
    if (hierarchical_levels <= 2) {
        pcs->wm_level = enc_mode <= ENC_M7 ? pcs->wm_level : 0;
    }
    Bool enable_wm = pcs->wm_level ? 1 : 0;
    if (scs->enable_warped_motion != DEFAULT)
        enable_wm = (Bool)scs->enable_warped_motion;
    // Note: local warp should be disabled when super-res or resize is ON
    // according to the AV1 spec 5.11.27
    frm_hdr->allow_warped_motion = enable_wm &&
        !(frm_hdr->frame_type == KEY_FRAME || frm_hdr->frame_type == INTRA_ONLY_FRAME) &&
        !frm_hdr->error_resilient_mode && !pcs->ppcs->frame_superres_enabled &&
        scs->static_config.resize_mode == RESIZE_NONE;

    frm_hdr->is_motion_mode_switchable = frm_hdr->allow_warped_motion;

    // pic_obmc_level - pic_obmc_level is used to define md_pic_obmc_level.
    // The latter determines the OBMC settings in the function set_obmc_controls.
    // Please check the definitions of the flags/variables in the function
    // set_obmc_controls corresponding to the pic_obmc_level settings.
    //  pic_obmc_level  | Default Encoder Settings
    //         0        | OFF subject to possible constraints
    //       > 1        | Faster level subject to possible constraints
    if (scs->obmc_level == DEFAULT) {
        if (fast_decode == 0 || input_resolution <= INPUT_SIZE_360p_RANGE) {
            if (ppcs->enc_mode <= ENC_M2)
                ppcs->pic_obmc_level = 1;
            else if (ppcs->enc_mode <= ENC_M5)
                ppcs->pic_obmc_level = 2;
            else if (enc_mode <= ENC_M6)
                ppcs->pic_obmc_level = 3;
            else
                ppcs->pic_obmc_level = 0;
        } else {
            if (ppcs->enc_mode <= ENC_M3)
                ppcs->pic_obmc_level = 1;
            else if (ppcs->enc_mode <= ENC_M4)
                ppcs->pic_obmc_level = 3;
            else if (ppcs->enc_mode <= ENC_M6)
                ppcs->pic_obmc_level = is_ref ? 3 : 0;
            else
                ppcs->pic_obmc_level = 0;
        }
    } else
        pcs->ppcs->pic_obmc_level = scs->obmc_level;

    // Switchable Motion Mode
    frm_hdr->is_motion_mode_switchable = frm_hdr->is_motion_mode_switchable || ppcs->pic_obmc_level;

    ppcs->bypass_cost_table_gen = 0;
    if (enc_mode <= ENC_M11)
        pcs->approx_inter_rate = 0;
    else
        pcs->approx_inter_rate = 1;
    if (is_islice || transition_present)
        pcs->skip_intra = 0;
#if OPT_LD_M9
    else if ((enc_mode <= ENC_M8) || (rtc_tune && (enc_mode <= ENC_M9)))
#else
    else if (enc_mode <= ENC_M8)
#endif
        pcs->skip_intra = 0;
    else
        pcs->skip_intra = (is_ref || pcs->ref_intra_percentage > 50) ? 0 : 1;

    // Set the level for the candidate(s) reduction feature
    pcs->cand_reduction_level = 0;
    if (is_islice)
        pcs->cand_reduction_level = 0;
    else if (enc_mode <= ENC_M3)
        pcs->cand_reduction_level = 0;
    else if (enc_mode <= ENC_M5)
        pcs->cand_reduction_level = 1;
    else if (enc_mode <= ENC_M6) {
        if (pcs->coeff_lvl == LOW_LVL)
            pcs->cand_reduction_level = 1;
        else
            pcs->cand_reduction_level = 2;
    } else if (enc_mode <= ENC_M9) {
        if (pcs->coeff_lvl == LOW_LVL)
            pcs->cand_reduction_level = 1;
        else
            pcs->cand_reduction_level = 3;
    } else
        pcs->cand_reduction_level = 3;
    if (scs->rc_stat_gen_pass_mode)
        pcs->cand_reduction_level = 7;
    // Set the level for the txt search
    pcs->txt_level = 0;
    if (enc_mode <= ENC_MR)
        pcs->txt_level = 1;
    else if (enc_mode <= ENC_M1)
        pcs->txt_level = 2;
    else if (enc_mode <= ENC_M2)
        pcs->txt_level = 3;
    else if (enc_mode <= ENC_M6) {
        if (pcs->coeff_lvl == LOW_LVL) {
            pcs->txt_level = 4;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            pcs->txt_level = 7;
        } else { // regular
            pcs->txt_level = is_base ? 4 : 5;
        }
    } else if (enc_mode <= ENC_M11) {
        if (pcs->coeff_lvl == LOW_LVL) {
            pcs->txt_level = is_base ? 4 : 5;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            pcs->txt_level = is_base ? 8 : 10;
        } else { // regular
            pcs->txt_level = 7;
        }
    }
#if OPT_LD_M13
    else if ((enc_mode <= ENC_M12) || (rtc_tune && (enc_mode <= ENC_M13))) {
#else
    else if (enc_mode <= ENC_M12) {
#endif
        if (pcs->coeff_lvl == LOW_LVL) {
            pcs->txt_level = is_base ? 8 : 10;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            pcs->txt_level = is_base ? 8 : 10;
            if (pcs->ref_intra_percentage < 85 && !pcs->ppcs->sc_class1) {
                pcs->txt_level = 0;
            }
        } else { // regular
            pcs->txt_level = is_base ? 8 : 10;
            if (pcs->ref_intra_percentage < 85 && !is_base && !pcs->ppcs->sc_class1) {
                pcs->txt_level = 0;
            }
        }
    } else {
        pcs->txt_level = is_base ? 8 : 10;
        if (pcs->ref_intra_percentage < 85 && !pcs->ppcs->sc_class1) {
            pcs->txt_level = 0;
        }
    }
    // Set the level for the txt shortcut feature
    // Any tx_shortcut_level having the chroma detector off in REF frames should be reserved for M13+
    pcs->tx_shortcut_level = 0;
    if (enc_mode <= ENC_M4)
        pcs->tx_shortcut_level = 0;
    else if (enc_mode <= ENC_M5)
        pcs->tx_shortcut_level = is_base ? 0 : 1;
    else if (enc_mode <= ENC_M10)
        pcs->tx_shortcut_level = is_islice ? 0 : 1;
    else
        pcs->tx_shortcut_level = is_islice ? 0 : 4;
    // Set the level the interpolation search
    pcs->interpolation_search_level = 0;

    if (enc_mode <= ENC_MR)
        pcs->interpolation_search_level = 2;
    else if (enc_mode <= ENC_M6)
        pcs->interpolation_search_level = 4;
    else {
        pcs->interpolation_search_level = 4;
        if (!is_base) {
            const uint8_t th[INPUT_SIZE_COUNT] = {100, 100, 85, 50, 30, 30, 30};
            const uint8_t skip_area            = pcs->ref_skip_percentage;
            if (skip_area > th[input_resolution])
                pcs->interpolation_search_level = 0;
        }
    }

    pcs->chroma_level = svt_aom_get_chroma_level(enc_mode);
    // Set the level for cfl
    pcs->cfl_level = 0;
    if (pcs->ppcs->sc_class1) {
        if (enc_mode <= ENC_M6)
            pcs->cfl_level = 1;
        else
            pcs->cfl_level = is_base ? 2 : 0;
    } else if (enc_mode <= ENC_M4)
        pcs->cfl_level = 1;
    else if (enc_mode <= ENC_M9)
        pcs->cfl_level = is_base ? 2 : 0;
    else if (enc_mode <= ENC_M11) {
        if (hierarchical_levels <= 3)
            pcs->cfl_level = is_islice ? 2 : 0;
        else
            pcs->cfl_level = is_base ? 2 : 0;
    } else if (enc_mode <= ENC_M12)
        pcs->cfl_level = is_islice ? 2 : 0;
    else
        pcs->cfl_level = 0;
#if OPT_LD_LATENCY_MD
    if (pcs->scs->low_latency_kf && is_islice)
        pcs->cfl_level = 0;
#endif
    // Set the level for new/nearest/near injection
    if (scs->new_nearest_comb_inject == DEFAULT)
        if (enc_mode <= ENC_M0)
            pcs->new_nearest_near_comb_injection = 1;
        else
            pcs->new_nearest_near_comb_injection = 0;
    else
        pcs->new_nearest_near_comb_injection = scs->new_nearest_comb_inject;

    // Set the level for unipred3x3 injection
    if (enc_mode <= ENC_M0)
        pcs->unipred3x3_injection = 1;
    else
        pcs->unipred3x3_injection = 0;

    // Set the level for bipred3x3 injection
    if (scs->bipred_3x3_inject == DEFAULT) {
        if (enc_mode <= ENC_M0)
            pcs->bipred3x3_injection = 1;
        else if (enc_mode <= ENC_M2)
            pcs->bipred3x3_injection = 2;
        else
            pcs->bipred3x3_injection = 0;
    } else {
        pcs->bipred3x3_injection = scs->bipred_3x3_inject;
    }

    // Set the level for inter-inter compound
    if (scs->compound_mode) {
        if (scs->compound_level == DEFAULT) {
            if (enc_mode <= ENC_MR)
                pcs->inter_compound_mode = 1;
            else if (enc_mode <= ENC_M1)
                pcs->inter_compound_mode = 3;
            else if (enc_mode <= ENC_M3)
                pcs->inter_compound_mode = 4;
            else
                pcs->inter_compound_mode = 0;
        } else {
            pcs->inter_compound_mode = scs->compound_level;
        }
    } else {
        pcs->inter_compound_mode = 0;
    }

    // Set the level for the distance-based red pruning
    if (pcs->ppcs->ref_list0_count_try > 1 || pcs->ppcs->ref_list1_count_try > 1) {
        if (enc_mode <= ENC_MR)
            pcs->dist_based_ref_pruning = 1;
        else if (enc_mode <= ENC_M0)
            pcs->dist_based_ref_pruning = is_base ? 1 : 2;
        else if (enc_mode <= ENC_M4)
            pcs->dist_based_ref_pruning = is_base ? 2 : 5;
        else if (enc_mode <= ENC_M5)
            pcs->dist_based_ref_pruning = is_base ? 2 : 6;
        else {
            if (pcs->coeff_lvl == LOW_LVL) {
                pcs->dist_based_ref_pruning = is_base ? 2 : 6;
            } else {
                pcs->dist_based_ref_pruning = is_base ? 3 : 6;
            }
        }
    } else {
        pcs->dist_based_ref_pruning = 0;
    }

    // Set the level the spatial sse @ full-loop
    pcs->spatial_sse_full_loop_level = 0;
    if (scs->spatial_sse_full_loop_level == DEFAULT)
        if (pcs->ppcs->sc_class1)
            pcs->spatial_sse_full_loop_level = 1;
        else if (enc_mode <= ENC_M11)
            pcs->spatial_sse_full_loop_level = 1;
        else
            pcs->spatial_sse_full_loop_level = 0;
    else
        pcs->spatial_sse_full_loop_level = scs->spatial_sse_full_loop_level;
    //set the nsq_level
    pcs->nsq_level = get_nsq_level(enc_mode, is_islice, is_base, pcs->coeff_lvl);
    // Set the level for enable_inter_intra
    // Block level switch, has to follow the picture level
    // inter intra pred                      Settings
    // 0                                     OFF
    // 1                                     FULL
    // 2                                     FAST 1 : Do not inject for unipred3x3 or PME inter candidates
    // 3                                     FAST 2 : Level 1 + do not inject for non-closest ref frames or ref frames with high distortion
    if (pcs->ppcs->slice_type != I_SLICE && scs->seq_header.enable_interintra_compound) {
        if (enc_mode <= ENC_M1)
            pcs->md_inter_intra_level = 1;
        else if (enc_mode <= ENC_M2)
            pcs->md_inter_intra_level = transition_present ? 1 : (is_base ? 1 : 0);
        else if (enc_mode <= ENC_M11)
            pcs->md_inter_intra_level = transition_present ? 1 : 0;
        else
            pcs->md_inter_intra_level = 0;
    } else
        pcs->md_inter_intra_level = 0;

    if (enc_mode <= ENC_MRS)
        pcs->txs_level = 1;
    else if (enc_mode <= ENC_MR)
        pcs->txs_level = 2;
    else if (enc_mode <= ENC_M2)
        pcs->txs_level = is_base ? 2 : 3;
    else if (enc_mode <= ENC_M7)
        pcs->txs_level = is_base ? 2 : 0;
#if OPT_LD_M10
    else if ((enc_mode <= ENC_M9) || (rtc_tune && enc_mode <= ENC_M10))
#else
    else if (enc_mode <= ENC_M9)
#endif
        pcs->txs_level = is_islice ? 3 : 0;
    else if (enc_mode <= ENC_M10) {
        if (hierarchical_levels <= 3)
            pcs->txs_level = is_islice ? 5 : 0;
        else
            pcs->txs_level = is_islice ? 3 : 0;
    } else
        pcs->txs_level = is_islice ? 5 : 0;
#if OPT_LD_LATENCY_MD
    if (pcs->scs->low_latency_kf && pcs->slice_type == I_SLICE)
        pcs->txs_level = 5;
#endif
    // Set tx_mode for the frame header
    frm_hdr->tx_mode = (pcs->txs_level) ? TX_MODE_SELECT : TX_MODE_LARGEST;
    // Set the level for nic
#if OPT_LD_M11
    pcs->nic_level = get_nic_level(enc_mode, is_base, hierarchical_levels, rtc_tune);
#else
    pcs->nic_level = get_nic_level(enc_mode, is_base, hierarchical_levels);
#endif
    // Set the level for SQ me-search
    if (enc_mode <= ENC_M0)
        pcs->md_sq_mv_search_level = 1;
    else
        pcs->md_sq_mv_search_level = 0;

    // Set the level for NSQ me-search
    if (enc_mode <= ENC_MRS)
        pcs->md_nsq_mv_search_level = 2;
    else
        pcs->md_nsq_mv_search_level = 4;
    // Set the level for PME search
    if (enc_mode <= ENC_MR)
        pcs->md_pme_level = 1;
    else if (enc_mode <= ENC_M3)
        pcs->md_pme_level = 2;
    else if (enc_mode <= ENC_M5) {
        if (hierarchical_levels <= 3)
            pcs->md_pme_level = 6;
        else
            pcs->md_pme_level = 3;
    } else if (enc_mode <= ENC_M7)
        pcs->md_pme_level = 5;
    else
        pcs->md_pme_level = 6;

    // Set the level for mds0
    pcs->mds0_level = 0;
#if OPT_LD_M10
    if ((enc_mode <= ENC_M9) || (rtc_tune && enc_mode <= ENC_M10))
#else
    if (enc_mode <= ENC_M9)
#endif
        pcs->mds0_level = 2;
    else if (enc_mode <= ENC_M11) {
        if (hierarchical_levels <= 3)
            pcs->mds0_level = is_islice ? 2 : 4;
        else
            pcs->mds0_level = 2;
    } else
        pcs->mds0_level = is_islice ? 2 : 4;
    /*
       disallow_4x4
    */
    pcs->pic_disallow_4x4 = svt_aom_get_disallow_4x4(enc_mode, slice_type);
    /*
       Bypassing EncDec
    */
#if OPT_LD_P2
    // In low delay mode, bypassing encdec in base layer is disabled as it provides bad trade offs
#endif
    // TODO: Bypassing EncDec doesn't work if NSQ is enabled for 10bit content (causes r2r).
    // TODO: This signal can only be modified per picture right now, not per SB.  Per SB requires
    // neighbour array updates at EncDec for all SBs, that are currently skipped if EncDec is bypassed.
    // TODO: Bypassing EncDec doesn't work if pcs->cdf_ctrl.update_coef is enabled for non-ISLICE frames (causes r2r)
#if OPT_LD_P2
    if (!(is_base && scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) &&
        (scs->static_config.encoder_bit_depth == EB_EIGHT_BIT || !pcs->nsq_level) &&
#else
    if ((scs->static_config.encoder_bit_depth == EB_EIGHT_BIT || !pcs->nsq_level) &&
#endif
        (!pcs->cdf_ctrl.update_coef || is_islice) &&
        !ppcs->frm_hdr.segmentation_params.segmentation_enabled) {
        pcs->pic_bypass_encdec = get_bypass_encdec(
            enc_mode, ppcs->hbd_md, scs->static_config.encoder_bit_depth);
    } else
        pcs->pic_bypass_encdec = 0;

    /*
        set lpd0_level
    */
    if (enc_mode <= ENC_M2)
        pcs->pic_lpd0_lvl = 0;
    else if (enc_mode <= ENC_M5)
        pcs->pic_lpd0_lvl = 1;
    else if (enc_mode <= ENC_M9) {
        if (pcs->coeff_lvl == LOW_LVL) {
            pcs->pic_lpd0_lvl = 1;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            pcs->pic_lpd0_lvl = (is_base || transition_present) ? 2 : 4;
        } else { // Regular
            pcs->pic_lpd0_lvl = 2;
        }
    } else if (enc_mode <= ENC_M10) {
        if (pcs->coeff_lvl == LOW_LVL) {
            pcs->pic_lpd0_lvl = 2;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 6;
        } else { // Regular
            pcs->pic_lpd0_lvl = (is_base || transition_present) ? 2 : 4;
        }
    } else if (enc_mode <= ENC_M11) {
#if OPT_LD_M11
        if (rtc_tune) {
            if (pcs->coeff_lvl == LOW_LVL) {
                pcs->pic_lpd0_lvl = 2;
            } else if (pcs->coeff_lvl == HIGH_LVL) {
                pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 7;
            } else { // Regular
                pcs->pic_lpd0_lvl = (is_base || transition_present) ? 4 : 6;
            }
        } else {
            if (pcs->coeff_lvl == LOW_LVL) {
                pcs->pic_lpd0_lvl = (is_base || transition_present) ? 2 : 4;
            } else if (pcs->coeff_lvl == HIGH_LVL) {
                pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 7;
            } else { // Regular
                pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 6;
            }
        }
#else
        if (pcs->coeff_lvl == LOW_LVL) {
            pcs->pic_lpd0_lvl = (is_base || transition_present) ? 2 : 4;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 7;
        } else { // Regular
            pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 6;
        }
#endif
    } else {
#if OPT_LD_P2
        if (rtc_tune) {
            if (enc_mode <= ENC_M12) {
                if (pcs->coeff_lvl == LOW_LVL) {
                    pcs->pic_lpd0_lvl = 4;
                } else if (pcs->coeff_lvl == HIGH_LVL) {
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 7;
                } else { // Regular
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? 4 : 6;
                }
            } else
                pcs->pic_lpd0_lvl = is_islice ? 4 : 7;
        } else {
#endif
            if (pcs->coeff_lvl == LOW_LVL) {
                pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 6;
            } else if (pcs->coeff_lvl == HIGH_LVL) {
                pcs->pic_lpd0_lvl = 7;
            } else { // regular
                pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 7;
            }
#if OPT_LD_P2
        }
#endif
    }

    if (pcs->ppcs->sc_class1 || scs->static_config.pass == ENC_MIDDLE_PASS)
        pcs->pic_skip_pd0 = 0;
    else if (enc_mode <= ENC_M13)
        pcs->pic_skip_pd0 = 0;
    else
        pcs->pic_skip_pd0 = is_base ? 0 : 1;
#if OPT_LD_M9
    pcs->pic_disallow_below_16x16 = svt_aom_get_disallow_below_16x16_picture_level(
        enc_mode, input_resolution, is_islice, ppcs->sc_class1, is_ref, rtc_tune);
#else
    pcs->pic_disallow_below_16x16 = svt_aom_get_disallow_below_16x16_picture_level(
        enc_mode, input_resolution, is_islice, ppcs->sc_class1, is_ref);
#endif

    if (scs->super_block_size == 64) {
        if (is_islice || transition_present) {
            pcs->pic_depth_removal_level = 0;
        } else {
            // Set depth_removal_level_controls
            if (pcs->ppcs->sc_class1) {
                if (enc_mode <= ENC_M8)
                    pcs->pic_depth_removal_level = 0;
                else if (enc_mode <= ENC_M10) {
                    pcs->pic_depth_removal_level = is_base ? 0 : 6;
                } else if (enc_mode <= ENC_M12) {
                    pcs->pic_depth_removal_level = is_base ? 4 : 6;
                } else {
                    pcs->pic_depth_removal_level = is_base ? 5 : 14;
                }
            } else if (fast_decode == 0) {
                if (enc_mode <= ENC_M1)
                    pcs->pic_depth_removal_level = 0;
                else if (enc_mode <= ENC_M5) {
                    if (input_resolution <= INPUT_SIZE_480p_RANGE)
                        pcs->pic_depth_removal_level = 1;
                    else
                        pcs->pic_depth_removal_level = 2;
                } else if (enc_mode <= ENC_M7) {
                    if (pcs->coeff_lvl == LOW_LVL) {
                        if (input_resolution <= INPUT_SIZE_480p_RANGE)
                            pcs->pic_depth_removal_level = 1;
                        else
                            pcs->pic_depth_removal_level = 2;
                    } else {
                        if (input_resolution <= INPUT_SIZE_480p_RANGE)
                            pcs->pic_depth_removal_level = 1;
                        else
                            pcs->pic_depth_removal_level = 2;
                    }
                } else if (enc_mode <= ENC_M8) {
                    if (pcs->coeff_lvl == LOW_LVL) {
                        if (input_resolution <= INPUT_SIZE_480p_RANGE)
                            pcs->pic_depth_removal_level = 1;
                        else
                            pcs->pic_depth_removal_level = 2;
                    } else {
                        if (input_resolution <= INPUT_SIZE_480p_RANGE)
                            pcs->pic_depth_removal_level = 1;
                        else
                            pcs->pic_depth_removal_level = is_base ? 2 : 6;
                    }
                } else if (enc_mode <= ENC_M9) {
                    if (pcs->coeff_lvl == LOW_LVL) {
                        if (input_resolution <= INPUT_SIZE_480p_RANGE)
                            pcs->pic_depth_removal_level = 1;
                        else
                            pcs->pic_depth_removal_level = 2;
                    } else {
                        if (input_resolution <= INPUT_SIZE_360p_RANGE)
                            pcs->pic_depth_removal_level = is_base ? 2 : 3;
                        else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                            pcs->pic_depth_removal_level = is_base ? 2 : 5;
                        else
                            pcs->pic_depth_removal_level = is_base ? 2 : 6;
                    }
                } else if (enc_mode <= ENC_M11) {
                    if (input_resolution <= INPUT_SIZE_360p_RANGE)
                        pcs->pic_depth_removal_level = is_base ? 2 : 3;
                    else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                        pcs->pic_depth_removal_level = is_base ? 2 : 5;
                    else if (input_resolution <= INPUT_SIZE_720p_RANGE)
                        pcs->pic_depth_removal_level = is_base ? 2 : 6;
                    else if (input_resolution <= INPUT_SIZE_1080p_RANGE)
                        pcs->pic_depth_removal_level = is_base ? 3 : 8;
                    else
                        pcs->pic_depth_removal_level = is_base ? 9 : 14;
                } else {
                    if (input_resolution <= INPUT_SIZE_360p_RANGE)
                        pcs->pic_depth_removal_level = is_base ? 2 : 3;
                    else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                        pcs->pic_depth_removal_level = is_base ? 9 : 11;
                    else
                        pcs->pic_depth_removal_level = is_base ? 9 : 14;
                }
            } else {
                if (enc_mode <= ENC_M2)
                    pcs->pic_depth_removal_level = 0;
                else if (enc_mode <= ENC_M5) {
                    if (input_resolution <= INPUT_SIZE_480p_RANGE)
                        pcs->pic_depth_removal_level = 1;
                    else
                        pcs->pic_depth_removal_level = 2;
                } else if (enc_mode <= ENC_M7) {
                    if (input_resolution <= INPUT_SIZE_1080p_RANGE)
                        pcs->pic_depth_removal_level = 1;
                    else
                        pcs->pic_depth_removal_level = 2;
                } else if (enc_mode <= ENC_M8) {
                    if (input_resolution <= INPUT_SIZE_360p_RANGE)
                        pcs->pic_depth_removal_level = is_base ? 2 : 3;
                    else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                        pcs->pic_depth_removal_level = is_base ? 2 : 5;
                    else
                        pcs->pic_depth_removal_level = is_base ? 2 : 6;
                } else if (enc_mode <= ENC_M11) {
                    if (input_resolution <= INPUT_SIZE_360p_RANGE)
                        pcs->pic_depth_removal_level = is_base ? 2 : 4;
                    else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                        pcs->pic_depth_removal_level = is_base ? 2 : 5;
                    else if (input_resolution <= INPUT_SIZE_720p_RANGE)
                        pcs->pic_depth_removal_level = is_base ? 2 : 6;
                    else if (input_resolution <= INPUT_SIZE_1080p_RANGE)
                        pcs->pic_depth_removal_level = is_base ? 3 : 8;
                    else
                        pcs->pic_depth_removal_level = is_base ? 9 : 14;
                } else {
                    if (input_resolution <= INPUT_SIZE_360p_RANGE)
                        pcs->pic_depth_removal_level = 7;
                    else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                        pcs->pic_depth_removal_level = is_base ? 9 : 11;
                    else
                        pcs->pic_depth_removal_level = is_base ? 9 : 14;
                }
            }
        }
    }
    if (pcs->ppcs->sc_class1) {
        if (enc_mode <= ENC_M6)
            pcs->pic_block_based_depth_refinement_level = 0;
        else if (enc_mode <= ENC_M9)
            pcs->pic_block_based_depth_refinement_level = is_base ? 0 : 5;
        else if (enc_mode <= ENC_M10)
            pcs->pic_block_based_depth_refinement_level = is_islice ? 1 : 5;
        else
            pcs->pic_block_based_depth_refinement_level = is_islice ? 7 : 12;
    } else if (enc_mode <= ENC_M2)
        pcs->pic_block_based_depth_refinement_level = 0;
    else if (enc_mode <= ENC_M5) {
        if (pcs->coeff_lvl == LOW_LVL)
            pcs->pic_block_based_depth_refinement_level = is_base ? 0 : 2;
        else
            pcs->pic_block_based_depth_refinement_level = is_base ? 0 : 4;
    } else {
#if OPT_LD_M9
        if (rtc_tune) {
            if (enc_mode <= ENC_M8) {
                if (pcs->coeff_lvl == LOW_LVL) {
                    pcs->pic_block_based_depth_refinement_level = is_base ? 1 : 4;
                } else { // regular
                    pcs->pic_block_based_depth_refinement_level = is_base ? 2 : 5;
                }
            } else {
                if (pcs->coeff_lvl == LOW_LVL) {
                    pcs->pic_block_based_depth_refinement_level = is_base ? 2 : 6;
                } else { // regular
                    pcs->pic_block_based_depth_refinement_level = is_base ? 7 : 10;
                }
            }
        } else {
            if (pcs->coeff_lvl == LOW_LVL) {
                pcs->pic_block_based_depth_refinement_level = is_base ? 1 : 4;
            } else { // regular
                pcs->pic_block_based_depth_refinement_level = is_base ? 2 : 5;
            }
        }
#else
        if (pcs->coeff_lvl == LOW_LVL) {
            pcs->pic_block_based_depth_refinement_level = is_base ? 1 : 4;
        } else { // regular
            pcs->pic_block_based_depth_refinement_level = is_base ? 2 : 5;
        }
#endif
    }
#if FIX_LAYER_SIGNAL
    if (ppcs->hierarchical_levels == (EB_MAX_TEMPORAL_LAYERS - 1)) {
        pcs->pic_block_based_depth_refinement_level = MAX(
            0, pcs->pic_block_based_depth_refinement_level - 1);
    }
#else
    if (scs->max_heirachical_level == (EB_MAX_TEMPORAL_LAYERS - 1)) {
        pcs->pic_block_based_depth_refinement_level = MAX(
            0, pcs->pic_block_based_depth_refinement_level - 1);
    }
#endif
    if (enc_mode <= ENC_M1)
        pcs->pic_depth_early_exit_th = 0;
    else
        pcs->pic_depth_early_exit_th = 90;

    if (pcs->ppcs->sc_class1) {
        if (enc_mode <= ENC_M7)
            pcs->pic_lpd1_lvl = 0;
        else if (enc_mode <= ENC_M9)
            pcs->pic_lpd1_lvl = is_ref ? 0 : 1;
        else if (enc_mode <= ENC_M10)
            pcs->pic_lpd1_lvl = is_ref ? 0 : 2;
        else if (enc_mode <= ENC_M11)
            pcs->pic_lpd1_lvl = is_base ? 0 : 2;
        else
            pcs->pic_lpd1_lvl = is_base ? 0 : 4;
    } else if (enc_mode <= ENC_M7)
        pcs->pic_lpd1_lvl = 0;
    else if (enc_mode <= ENC_M9) {
        if (pcs->coeff_lvl == LOW_LVL) {
            pcs->pic_lpd1_lvl = 0;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            pcs->pic_lpd1_lvl = is_base ? 0 : 2;
        } else { // Regular
            pcs->pic_lpd1_lvl = is_ref ? 0 : 1;
        }
    } else if (enc_mode <= ENC_M10) {
        if (pcs->coeff_lvl == LOW_LVL) {
            pcs->pic_lpd1_lvl = is_ref ? 0 : 1;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            pcs->pic_lpd1_lvl = is_base ? 0 : 3;
        } else { // Regular
            pcs->pic_lpd1_lvl = is_base ? 0 : 2;
        }
#if OPT_LD_M12_13
    } else if ((enc_mode <= ENC_M11) || (rtc_tune && (enc_mode <= ENC_M12))) {
#else
    } else if (enc_mode <= ENC_M11) {
#endif
#if OPT_LD_M11
        if (rtc_tune) {
            if (pcs->coeff_lvl == LOW_LVL) {
                pcs->pic_lpd1_lvl = is_ref ? 0 : 2;
            } else if (pcs->coeff_lvl == HIGH_LVL) {
                pcs->pic_lpd1_lvl = is_base ? 0 : 4;
            } else { // Regular
                pcs->pic_lpd1_lvl = is_base ? 0 : 3;
            }
        } else {
            if (pcs->coeff_lvl == LOW_LVL) {
                pcs->pic_lpd1_lvl = is_base ? 0 : 2;
            } else if (pcs->coeff_lvl == HIGH_LVL) {
                pcs->pic_lpd1_lvl = is_base ? 0 : 4;
            } else { // Regular
                pcs->pic_lpd1_lvl = is_base ? 0 : 3;
            }
        }
#else
        if (pcs->coeff_lvl == LOW_LVL) {
            pcs->pic_lpd1_lvl = is_base ? 0 : 2;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            pcs->pic_lpd1_lvl = is_base ? 0 : 4;
        } else { // Regular
            pcs->pic_lpd1_lvl = is_base ? 0 : 3;
        }
#endif
#if OPT_LD_M13
    } else if ((enc_mode <= ENC_M12) || (rtc_tune && (enc_mode <= ENC_M13))) {
#else
    } else if (enc_mode <= ENC_M12) {
#endif
        if (pcs->coeff_lvl == LOW_LVL) {
            pcs->pic_lpd1_lvl = is_base ? 0 : 3;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            pcs->pic_lpd1_lvl = is_base ? 0 : 5;
        } else { // regular
            pcs->pic_lpd1_lvl = is_base ? 0 : 4;
        }
    } else {
        if (input_resolution <= INPUT_SIZE_1080p_RANGE &&
            scs->static_config.encoder_bit_depth == EB_EIGHT_BIT)
            pcs->pic_lpd1_lvl = is_base ? 0 : 6;
        else
            pcs->pic_lpd1_lvl = is_base ? 0 : 5;
    }
    // Can only use light-PD1 under the following conditions
    // There is another check before PD1 is called; pred_depth_only is not checked here, because some modes
    // may force pred_depth_only at the light-pd1 detector
    if (pcs->pic_lpd1_lvl &&
        !(ppcs->hbd_md == 0 && !pcs->nsq_level && pcs->pic_disallow_4x4 == TRUE &&
          scs->super_block_size == 64)) {
        pcs->pic_lpd1_lvl = 0;
    }

    // Use the me-SAD-to-SATD deviation (of the 32x32 blocks) to detect the presence of isolated edges.
    // An SB is tagged as problematic when the deviation is higher than the normal (i.e. when me-sad and satd are not correlated)
    // For the detected SB(s), apply a better level for Depth-removal, LPD0, LPD1, and TXT of regular PD1.
    // Not applicable for I_SLICE and for SB 128x128
    if (pcs->slice_type == I_SLICE || scs->super_block_size == 128) {
        pcs->vq_ctrls.detect_high_freq_lvl = 0;
    } else {
        if (pcs->ppcs->sc_class1) {
            pcs->vq_ctrls.detect_high_freq_lvl = 1;
        } else {
#if OPT_LD_M13
            if ((enc_mode <= ENC_M12) || (rtc_tune && (enc_mode <= ENC_M13)))
#else
            if (enc_mode <= ENC_M12)
#endif
                pcs->vq_ctrls.detect_high_freq_lvl = 2;
            else
                pcs->vq_ctrls.detect_high_freq_lvl = 0;
        }
    }

    return return_error;
}

/******************************************************
* Derive Mode Decision Config Settings for first pass
Input   : encoder mode and tune
Output  : EncDec Kernel signal(s)
******************************************************/
EbErrorType       first_pass_signal_derivation_mode_decision_config_kernel(PictureControlSet *pcs);
static INLINE int get_relative_dist(const OrderHintInfo *oh, int a, int b) {
    if (!oh->enable_order_hint)
        return 0;

    const int bits = oh->order_hint_bits;

    assert(bits >= 1);
    assert(a >= 0 && a < (1 << bits));
    assert(b >= 0 && b < (1 << bits));

    int       diff = a - b;
    const int m    = 1 << (bits - 1);
    diff           = (diff & (m - 1)) - (diff & m);
    return diff;
}

static int get_block_position(Av1Common *cm, int *mi_r, int *mi_c, int blk_row, int blk_col, MV mv,
                              int sign_bias) {
    const int base_blk_row = (blk_row >> 3) << 3;
    const int base_blk_col = (blk_col >> 3) << 3;

    const int row_offset = (mv.row >= 0) ? (mv.row >> (4 + MI_SIZE_LOG2))
                                         : -((-mv.row) >> (4 + MI_SIZE_LOG2));

    const int col_offset = (mv.col >= 0) ? (mv.col >> (4 + MI_SIZE_LOG2))
                                         : -((-mv.col) >> (4 + MI_SIZE_LOG2));

    const int row = (sign_bias == 1) ? blk_row - row_offset : blk_row + row_offset;
    const int col = (sign_bias == 1) ? blk_col - col_offset : blk_col + col_offset;

    if (row < 0 || row >= (cm->mi_rows >> 1) || col < 0 || col >= (cm->mi_cols >> 1))
        return 0;

    if (row < base_blk_row - (MAX_OFFSET_HEIGHT >> 3) ||
        row >= base_blk_row + 8 + (MAX_OFFSET_HEIGHT >> 3) ||
        col < base_blk_col - (MAX_OFFSET_WIDTH >> 3) ||
        col >= base_blk_col + 8 + (MAX_OFFSET_WIDTH >> 3))
        return 0;

    *mi_r = row;
    *mi_c = col;

    return 1;
}

#define MFMV_STACK_SIZE 3

// Note: motion_filed_projection finds motion vectors of current frame's
// reference frame, and projects them to current frame. To make it clear,
// let's call current frame's reference frame as start frame.
// Call Start frame's reference frames as reference frames.
// Call ref_offset as frame distances between start frame and its reference
// frames.
static int motion_field_projection(Av1Common *cm, PictureControlSet *pcs,
                                   MvReferenceFrame start_frame, int dir) {
    TPL_MV_REF *tpl_mvs_base           = pcs->tpl_mvs;
    int         ref_offset[REF_FRAMES] = {0};

    uint8_t list_idx0, ref_idx_l0;
    list_idx0  = get_list_idx(start_frame);
    ref_idx_l0 = get_ref_frame_idx(start_frame);
    EbReferenceObject *start_frame_buf =
        (EbReferenceObject *)pcs->ref_pic_ptr_array[list_idx0][ref_idx_l0]->object_ptr;

    if (start_frame_buf == NULL)
        return 0;

    if (start_frame_buf->frame_type == KEY_FRAME || start_frame_buf->frame_type == INTRA_ONLY_FRAME)
        return 0;

    // MFMV is not applied when the reference picture is of a different spatial resolution
    // (described in the AV1 spec section 7.9.2.)
    if (start_frame_buf->mi_rows != cm->mi_rows || start_frame_buf->mi_cols != cm->mi_cols) {
        return 0;
    }

    const int                 start_frame_order_hint        = start_frame_buf->order_hint;
    const unsigned int *const ref_order_hints               = &start_frame_buf->ref_order_hint[0];
    int                       start_to_current_frame_offset = get_relative_dist(
        &pcs->ppcs->scs->seq_header.order_hint_info,
        start_frame_order_hint,
        pcs->ppcs->cur_order_hint);

    for (int i = LAST_FRAME; i <= INTER_REFS_PER_FRAME; ++i)
        ref_offset[i] = get_relative_dist(&pcs->ppcs->scs->seq_header.order_hint_info,
                                          start_frame_order_hint,
                                          ref_order_hints[i - LAST_FRAME]);

    if (dir == 2)
        start_to_current_frame_offset = -start_to_current_frame_offset;

    const MV_REF *const mv_ref_base = start_frame_buf->mvs;
    const int           mvs_rows    = (cm->mi_rows + 1) >> 1;
    const int           mvs_cols    = (cm->mi_cols + 1) >> 1;

    for (int blk_row = 0; blk_row < mvs_rows; ++blk_row) {
        for (int blk_col = 0; blk_col < mvs_cols; ++blk_col) {
            const MV_REF *const mv_ref = &mv_ref_base[blk_row * mvs_cols + blk_col];
            MV                  fwd_mv = mv_ref->mv.as_mv;

            if (mv_ref->ref_frame > INTRA_FRAME) {
                MV        this_mv;
                int       mi_r, mi_c;
                const int ref_frame_offset = ref_offset[mv_ref->ref_frame];

                int pos_valid = abs(ref_frame_offset) <= MAX_FRAME_DISTANCE &&
                    ref_frame_offset > 0 &&
                    abs(start_to_current_frame_offset) <= MAX_FRAME_DISTANCE;

                if (pos_valid) {
                    get_mv_projection(
                        &this_mv, fwd_mv, start_to_current_frame_offset, ref_frame_offset);
                    pos_valid = get_block_position(
                        cm, &mi_r, &mi_c, blk_row, blk_col, this_mv, dir >> 1);
                }

                if (pos_valid) {
                    const int mi_offset = mi_r * (cm->mi_stride >> 1) + mi_c;

                    tpl_mvs_base[mi_offset].mfmv0.as_mv.row  = fwd_mv.row;
                    tpl_mvs_base[mi_offset].mfmv0.as_mv.col  = fwd_mv.col;
                    tpl_mvs_base[mi_offset].ref_frame_offset = ref_frame_offset;
                }
            }
        }
    }

    return 1;
}
static void av1_setup_motion_field(Av1Common *cm, PictureControlSet *pcs) {
    const OrderHintInfo *const order_hint_info = &pcs->ppcs->scs->seq_header.order_hint_info;
    memset(pcs->ref_frame_side, 0, sizeof(pcs->ref_frame_side));
    if (!order_hint_info->enable_order_hint)
        return;

    TPL_MV_REF *tpl_mvs_base = pcs->tpl_mvs;
    int         size         = ((cm->mi_rows + MAX_MIB_SIZE) >> 1) * (cm->mi_stride >> 1);

    const int                cur_order_hint = pcs->ppcs->cur_order_hint;
    const EbReferenceObject *ref_buf[INTER_REFS_PER_FRAME];
    int                      ref_order_hint[INTER_REFS_PER_FRAME];

    for (int ref_frame = LAST_FRAME; ref_frame <= ALTREF_FRAME; ref_frame++) {
        const int ref_idx    = ref_frame - LAST_FRAME;
        int       order_hint = 0;
        uint8_t   list_idx0, ref_idx_l0;
        list_idx0  = get_list_idx(ref_frame);
        ref_idx_l0 = get_ref_frame_idx(ref_frame);
        EbReferenceObject *buf =
            (EbReferenceObject *)pcs->ref_pic_ptr_array[list_idx0][ref_idx_l0]->object_ptr;

        if (buf != NULL)
            order_hint = buf->order_hint;

        ref_buf[ref_idx]        = buf;
        ref_order_hint[ref_idx] = order_hint;

        if (get_relative_dist(order_hint_info, order_hint, cur_order_hint) > 0)
            pcs->ref_frame_side[ref_frame] = 1;
        else if (order_hint == cur_order_hint)
            pcs->ref_frame_side[ref_frame] = -1;
    }

    //for a frame based mfmv, we need to keep computing the ref_frame_side regardless mfmv is used or no
    if (!pcs->ppcs->frm_hdr.use_ref_frame_mvs)
        return;

    for (int idx = 0; idx < size; ++idx) {
        tpl_mvs_base[idx].mfmv0.as_int     = INVALID_MV;
        tpl_mvs_base[idx].ref_frame_offset = 0;
    }

    int ref_stamp = MFMV_STACK_SIZE - 1;
    if (ref_buf[0 /*LAST_FRAME - LAST_FRAME*/] != NULL) {
        const int alt_of_lst_order_hint =
            ref_buf[0 /*LAST_FRAME - LAST_FRAME*/]->ref_order_hint[ALTREF_FRAME - LAST_FRAME];
        const int is_lst_overlay = (alt_of_lst_order_hint ==
                                    ref_order_hint[GOLDEN_FRAME - LAST_FRAME]);
        if (!is_lst_overlay)
            motion_field_projection(cm, pcs, LAST_FRAME, 2);

        --ref_stamp;
    }

    if (get_relative_dist(
            order_hint_info, ref_order_hint[BWDREF_FRAME - LAST_FRAME], cur_order_hint) > 0) {
        if (motion_field_projection(cm, pcs, BWDREF_FRAME, 0))
            --ref_stamp;
    }

    if (get_relative_dist(
            order_hint_info, ref_order_hint[ALTREF2_FRAME - LAST_FRAME], cur_order_hint) > 0) {
        if (motion_field_projection(cm, pcs, ALTREF2_FRAME, 0))
            --ref_stamp;
    }

    if (get_relative_dist(
            order_hint_info, ref_order_hint[ALTREF_FRAME - LAST_FRAME], cur_order_hint) > 0 &&
        ref_stamp >= 0)
        if (motion_field_projection(cm, pcs, ALTREF_FRAME, 0))
            --ref_stamp;

    if (ref_stamp >= 0)
        motion_field_projection(cm, pcs, LAST2_FRAME, 2);
}
EbErrorType svt_av1_hash_table_create(HashTable *p_hash_table);
void       *rtime_alloc_block_hash_block_is_same(size_t size) { return malloc(size); }

// Use me_8x8_distortion and QP to predict the coeff level per frame
static void predict_frame_coeff_lvl(struct PictureControlSet *pcs) {
    uint64_t tot_me_8x8_dis = 0;
    for (uint32_t b64_idx = 0; b64_idx < pcs->b64_total_count; b64_idx++) {
        tot_me_8x8_dis += pcs->ppcs->me_8x8_distortion[b64_idx];
    }
    tot_me_8x8_dis = tot_me_8x8_dis / pcs->picture_qp;

    pcs->coeff_lvl = NORMAL_LVL;

    if (tot_me_8x8_dis < COEFF_LVL_TH_0) {
        pcs->coeff_lvl = LOW_LVL;
    } else if (tot_me_8x8_dis > COEFF_LVL_TH_1) {
        pcs->coeff_lvl = HIGH_LVL;
    }
}

/* Mode Decision Configuration Kernel */

/*********************************************************************************
*
* @brief
*  The Mode Decision Configuration Process involves a number of initialization steps,
*  setting flags for a number of features, and determining the blocks to be considered
*  in subsequent MD stages.
*
* @par Description:
*  The Mode Decision Configuration Process involves a number of initialization steps,
*  setting flags for a number of features, and determining the blocks to be considered
*  in subsequent MD stages. Examples of flags that are set are the flags for filter intra,
*  eighth-pel, OBMC and warped motion and flags for updating the cumulative density functions
*  Examples of initializations include initializations for picture chroma QP offsets,
*  CDEF strength, self-guided restoration filter parameters, quantization parameters,
*  lambda arrays, mv and coefficient rate estimation arrays.
*
*  The set of blocks to be processed in subsequent MD stages is decided in this process as a
*  function of the picture depth mode (pic_depth_mode).
*
* @param[in] Configurations
*  Configuration flags that are to be set
*
* @param[out] Initializations
*  Initializations for various flags and variables
*
********************************************************************************/
void *mode_decision_configuration_kernel(void *input_ptr) {
    // Context & SCS & PCS
    EbThreadContext                  *thread_context_ptr = (EbThreadContext *)input_ptr;
    ModeDecisionConfigurationContext *context_ptr        = (ModeDecisionConfigurationContext *)
                                                        thread_context_ptr->priv;
    // Input
    EbObjectWrapper *rate_control_results_wrapper_ptr;

    // Output
    EbObjectWrapper *enc_dec_tasks_wrapper_ptr;

    for (;;) {
        // Get RateControl Results
        EB_GET_FULL_OBJECT(context_ptr->rate_control_input_fifo_ptr,
                           &rate_control_results_wrapper_ptr);

        RateControlResults *rate_control_results_ptr =
            (RateControlResults *)rate_control_results_wrapper_ptr->object_ptr;
        PictureControlSet *pcs = (PictureControlSet *)
                                     rate_control_results_ptr->pcs_wrapper_ptr->object_ptr;
        SequenceControlSet *scs = pcs->scs;

        pcs->coeff_lvl = INVALID_LVL;
        if (scs->static_config.pass != ENC_FIRST_PASS) {
            if (pcs->slice_type != I_SLICE && !pcs->ppcs->sc_class1) {
                predict_frame_coeff_lvl(pcs);
            }
        }
        // -------
        // Scale references if resolution of the reference is different than the input
        // super-res reference frame size is same as original input size, only check current frame scaled flag;
        // reference scaling resizes reference frame to different size, need check each reference frame for scaling
        // -------
        if ((pcs->ppcs->frame_superres_enabled == 1 ||
             scs->static_config.resize_mode != RESIZE_NONE) &&
            pcs->slice_type != I_SLICE) {
            if (pcs->ppcs->is_used_as_reference_flag == TRUE &&
                pcs->ppcs->reference_picture_wrapper_ptr != NULL) {
                // update mi_rows and mi_cols for the reference pic wrapper (used in mfmv for other pictures)
                EbReferenceObject *reference_object =
                    pcs->ppcs->reference_picture_wrapper_ptr->object_ptr;
                reference_object->mi_rows = pcs->ppcs->aligned_height >> MI_SIZE_LOG2;
                reference_object->mi_cols = pcs->ppcs->aligned_width >> MI_SIZE_LOG2;
            }

            scale_rec_references(pcs, pcs->ppcs->enhanced_picture_ptr, pcs->hbd_md);
        }

        FrameHeader *frm_hdr = &pcs->ppcs->frm_hdr;
        // Mode Decision Configuration Kernel Signal(s) derivation
        if (scs->static_config.pass == ENC_FIRST_PASS)
            first_pass_signal_derivation_mode_decision_config_kernel(pcs);
        else
            signal_derivation_mode_decision_config_kernel_oq(scs, pcs);

        if (pcs->slice_type != I_SLICE && scs->mfmv_enabled)
            av1_setup_motion_field(pcs->ppcs->av1_cm, pcs);

        pcs->intra_coded_area = 0;
        pcs->skip_coded_area  = 0;
        // Init block selection
        // Set reference sg ep
        set_reference_sg_ep(pcs);
        set_global_motion_field(pcs);

        svt_av1_qm_init(pcs->ppcs);
        MdRateEstimationContext *md_rate_estimation_array;

        // QP
        context_ptr->qp = pcs->picture_qp;

        // QP Index
        context_ptr->qp_index = (uint8_t)frm_hdr->quantization_params.base_q_idx;

        md_rate_estimation_array = pcs->md_rate_estimation_array;
        if (pcs->ppcs->frm_hdr.primary_ref_frame != PRIMARY_REF_NONE)
            memcpy(&pcs->md_frame_context,
                   &pcs->ref_frame_context[pcs->ppcs->frm_hdr.primary_ref_frame],
                   sizeof(FRAME_CONTEXT));
        else {
            svt_av1_default_coef_probs(&pcs->md_frame_context,
                                       frm_hdr->quantization_params.base_q_idx);
            init_mode_probs(&pcs->md_frame_context);
        }
        // Initial Rate Estimation of the syntax elements
        av1_estimate_syntax_rate(md_rate_estimation_array,
                                 pcs->slice_type == I_SLICE ? TRUE : FALSE,
                                 pcs->pic_filter_intra_level,
                                 pcs->ppcs->frm_hdr.allow_screen_content_tools,
                                 pcs->ppcs->enable_restoration,
                                 pcs->ppcs->frm_hdr.allow_intrabc,
                                 pcs->ppcs->partition_contexts,
                                 &pcs->md_frame_context);
        // Initial Rate Estimation of the Motion vectors
        if (scs->static_config.pass != ENC_FIRST_PASS) {
            av1_estimate_mv_rate(pcs, md_rate_estimation_array, &pcs->md_frame_context);
            // Initial Rate Estimation of the quantized coefficients
            av1_estimate_coefficients_rate(md_rate_estimation_array, &pcs->md_frame_context);
        }
        if (frm_hdr->allow_intrabc) {
            int            i;
            int            speed          = 1;
            SpeedFeatures *sf             = &pcs->sf;
            sf->allow_exhaustive_searches = 1;

            const int mesh_speed           = AOMMIN(speed, MAX_MESH_SPEED);
            sf->exhaustive_searches_thresh = (1 << 25);

            sf->max_exaustive_pct = good_quality_max_mesh_pct[mesh_speed];
            if (mesh_speed > 0)
                sf->exhaustive_searches_thresh = sf->exhaustive_searches_thresh << 1;

            for (i = 0; i < MAX_MESH_STEP; ++i) {
                sf->mesh_patterns[i].range    = good_quality_mesh_patterns[mesh_speed][i].range;
                sf->mesh_patterns[i].interval = good_quality_mesh_patterns[mesh_speed][i].interval;
            }

            if (pcs->slice_type == I_SLICE) {
                for (i = 0; i < MAX_MESH_STEP; ++i) {
                    sf->mesh_patterns[i].range    = intrabc_mesh_patterns[mesh_speed][i].range;
                    sf->mesh_patterns[i].interval = intrabc_mesh_patterns[mesh_speed][i].interval;
                }
                sf->max_exaustive_pct = intrabc_max_mesh_pct[mesh_speed];
            }

            {
                // add to hash table
                const int pic_width  = pcs->ppcs->aligned_width;
                const int pic_height = pcs->ppcs->aligned_height;

                uint32_t *block_hash_values[2][2];
                int8_t   *is_block_same[2][3];
                int       k, j;

                for (k = 0; k < 2; k++) {
                    for (j = 0; j < 2; j++)
                        block_hash_values[k][j] = rtime_alloc_block_hash_block_is_same(
                            sizeof(uint32_t) * pic_width * pic_height);
                    for (j = 0; j < 3; j++)
                        is_block_same[k][j] = rtime_alloc_block_hash_block_is_same(
                            sizeof(int8_t) * pic_width * pic_height);
                }
                rtime_alloc_svt_av1_hash_table_create(&pcs->hash_table);
                Yv12BufferConfig cpi_source;
                link_eb_to_aom_buffer_desc_8bit(pcs->ppcs->enhanced_picture_ptr, &cpi_source);

                svt_av1_crc_calculator_init(&pcs->crc_calculator1, 24, 0x5D6DCB);
                svt_av1_crc_calculator_init(&pcs->crc_calculator2, 24, 0x864CFB);

                svt_av1_generate_block_2x2_hash_value(
                    &cpi_source, block_hash_values[0], is_block_same[0], pcs);
                uint8_t       src_idx     = 0;
                const uint8_t max_sb_size = pcs->ppcs->intraBC_ctrls.max_block_size_hash;
                for (int size = 4; size <= max_sb_size; size <<= 1, src_idx = !src_idx) {
                    const uint8_t dst_idx = !src_idx;
                    svt_av1_generate_block_hash_value(&cpi_source,
                                                      size,
                                                      block_hash_values[src_idx],
                                                      block_hash_values[dst_idx],
                                                      is_block_same[src_idx],
                                                      is_block_same[dst_idx],
                                                      pcs);
                    if (size != 4 || pcs->ppcs->intraBC_ctrls.hash_4x4_blocks)
                        rtime_alloc_svt_av1_add_to_hash_map_by_row_with_precal_data(
                            &pcs->hash_table,
                            block_hash_values[dst_idx],
                            is_block_same[dst_idx][2],
                            pic_width,
                            pic_height,
                            size);
                }
                for (k = 0; k < 2; k++) {
                    for (j = 0; j < 2; j++) free(block_hash_values[k][j]);
                    for (j = 0; j < 3; j++) free(is_block_same[k][j]);
                }
            }

            svt_av1_init3smotion_compensation(&pcs->ss_cfg,
                                              pcs->ppcs->enhanced_picture_ptr->stride_y);
        }
        CdefControls *cdef_ctrls = &pcs->ppcs->cdef_ctrls;
        uint8_t       skip_perc  = pcs->ref_skip_percentage;
        if ((skip_perc > 75 && cdef_ctrls->use_skip_detector) ||
            (scs->vq_ctrls.sharpness_ctrls.cdef && pcs->ppcs->is_noise_level))
            pcs->ppcs->cdef_level = 0;
        else {
            if (cdef_ctrls->use_reference_cdef_fs) {
                if (pcs->slice_type != I_SLICE) {
                    uint8_t lowest_sg  = TOTAL_STRENGTHS - 1;
                    uint8_t highest_sg = 0;
                    // Determine luma pred filter
                    // Add filter from list0
                    EbReferenceObject *ref_obj_l0 =
                        (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
                    for (uint8_t fs = 0; fs < ref_obj_l0->ref_cdef_strengths_num; fs++) {
                        if (ref_obj_l0->ref_cdef_strengths[0][fs] < lowest_sg)
                            lowest_sg = ref_obj_l0->ref_cdef_strengths[0][fs];
                        if (ref_obj_l0->ref_cdef_strengths[0][fs] > highest_sg)
                            highest_sg = ref_obj_l0->ref_cdef_strengths[0][fs];
                    }
                    if (pcs->slice_type == B_SLICE) {
                        // Add filter from list1
                        EbReferenceObject *ref_obj_l1 =
                            (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
                        for (uint8_t fs = 0; fs < ref_obj_l1->ref_cdef_strengths_num; fs++) {
                            if (ref_obj_l1->ref_cdef_strengths[0][fs] < lowest_sg)
                                lowest_sg = ref_obj_l1->ref_cdef_strengths[0][fs];
                            if (ref_obj_l1->ref_cdef_strengths[0][fs] > highest_sg)
                                highest_sg = ref_obj_l1->ref_cdef_strengths[0][fs];
                        }
                    }
                    int8_t mid_filter             = MIN(63, MAX(0, (lowest_sg + highest_sg) / 2));
                    cdef_ctrls->pred_y_f          = mid_filter;
                    cdef_ctrls->pred_uv_f         = 0;
                    cdef_ctrls->first_pass_fs_num = 0;
                    cdef_ctrls->default_second_pass_fs_num = 0;
                    // Set cdef to off if pred is.
                    if ((cdef_ctrls->pred_y_f == 0) && (cdef_ctrls->pred_uv_f == 0))
                        pcs->ppcs->cdef_level = 0;
                }
            } else if (cdef_ctrls->search_best_ref_fs) {
                if (pcs->slice_type != I_SLICE) {
                    cdef_ctrls->first_pass_fs_num          = 1;
                    cdef_ctrls->default_second_pass_fs_num = 0;

                    // Add filter from list0, if not the same as the default
                    EbReferenceObject *ref_obj_l0 =
                        (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
                    if (ref_obj_l0->ref_cdef_strengths[0][0] !=
                        cdef_ctrls->default_first_pass_fs[0]) {
                        cdef_ctrls->default_first_pass_fs[1] = ref_obj_l0->ref_cdef_strengths[0][0];
                        (cdef_ctrls->first_pass_fs_num)++;
                    }

                    if (pcs->slice_type == B_SLICE) {
                        EbReferenceObject *ref_obj_l1 =
                            (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
                        // Add filter from list1, if different from default filter and list0 filter
                        if (ref_obj_l1->ref_cdef_strengths[0][0] !=
                                cdef_ctrls->default_first_pass_fs[0] &&
                            ref_obj_l1->ref_cdef_strengths[0][0] !=
                                cdef_ctrls
                                    ->default_first_pass_fs[cdef_ctrls->first_pass_fs_num - 1]) {
                            cdef_ctrls->default_first_pass_fs[cdef_ctrls->first_pass_fs_num] =
                                ref_obj_l1->ref_cdef_strengths[0][0];
                            (cdef_ctrls->first_pass_fs_num)++;

                            // Chroma
                            if (ref_obj_l0->ref_cdef_strengths[1][0] ==
                                    cdef_ctrls->default_first_pass_fs_uv[0] &&
                                ref_obj_l1->ref_cdef_strengths[1][0] ==
                                    cdef_ctrls->default_first_pass_fs_uv[0]) {
                                cdef_ctrls->default_first_pass_fs_uv[0] = -1;
                                cdef_ctrls->default_first_pass_fs_uv[1] = -1;
                            }
                        }
                        // if list0/list1 filters are the same, skip CDEF search, and use the filter selected by the ref frames
                        else if (cdef_ctrls->first_pass_fs_num == 2 &&
                                 ref_obj_l0->ref_cdef_strengths[0][0] ==
                                     ref_obj_l1->ref_cdef_strengths[0][0]) {
                            cdef_ctrls->use_reference_cdef_fs = 1;

                            cdef_ctrls->pred_y_f          = ref_obj_l0->ref_cdef_strengths[0][0];
                            cdef_ctrls->pred_uv_f         = MIN(63,
                                                        MAX(0,
                                                            (ref_obj_l0->ref_cdef_strengths[1][0] +
                                                             ref_obj_l1->ref_cdef_strengths[1][0]) /
                                                                2));
                            cdef_ctrls->first_pass_fs_num = 0;
                            cdef_ctrls->default_second_pass_fs_num = 0;
                        }
                    }
                    // Chroma
                    else if (ref_obj_l0->ref_cdef_strengths[1][0] ==
                             cdef_ctrls->default_first_pass_fs_uv[0]) {
                        cdef_ctrls->default_first_pass_fs_uv[0] = -1;
                        cdef_ctrls->default_first_pass_fs_uv[1] = -1;
                    }

                    // Set cdef to off if pred luma is.
                    if (cdef_ctrls->first_pass_fs_num == 1)
                        pcs->ppcs->cdef_level = 0;
                }
            }
        }

        if (scs->vq_ctrls.sharpness_ctrls.restoration && pcs->ppcs->is_noise_level) {
            pcs->ppcs->enable_restoration = 0;
        }

        // Post the results to the MD processes
        uint16_t tg_count = pcs->ppcs->tile_group_cols * pcs->ppcs->tile_group_rows;
        for (uint16_t tile_group_idx = 0; tile_group_idx < tg_count; tile_group_idx++) {
            svt_get_empty_object(context_ptr->mode_decision_configuration_output_fifo_ptr,
                                 &enc_dec_tasks_wrapper_ptr);

            EncDecTasks *enc_dec_tasks_ptr = (EncDecTasks *)enc_dec_tasks_wrapper_ptr->object_ptr;
            enc_dec_tasks_ptr->pcs_wrapper_ptr  = rate_control_results_ptr->pcs_wrapper_ptr;
            enc_dec_tasks_ptr->input_type       = rate_control_results_ptr->superres_recode
                      ? ENCDEC_TASKS_SUPERRES_INPUT
                      : ENCDEC_TASKS_MDC_INPUT;
            enc_dec_tasks_ptr->tile_group_index = tile_group_idx;

            // Post the Full Results Object
            svt_post_full_object(enc_dec_tasks_wrapper_ptr);

            if (rate_control_results_ptr->superres_recode) {
                // for superres input, only send one task
                break;
            }
        }

        // Release Rate Control Results
        svt_release_object(rate_control_results_wrapper_ptr);
    }

    return NULL;
}
