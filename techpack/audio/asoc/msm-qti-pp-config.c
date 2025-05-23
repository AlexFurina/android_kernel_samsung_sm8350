// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2012-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <dsp/q6adm-v2.h>
#include <dsp/q6asm-v2.h>
#include <dsp/q6afe-v2.h>
#include <dsp/q6audio-v2.h>
#include <dsp/q6common.h>

#include "msm-qti-pp-config.h"
#include "msm-pcm-routing-v2.h"

/* EQUALIZER */
/* Equal to Frontend after last of the MULTIMEDIA SESSIONS */
#define MAX_EQ_SESSIONS		(MSM_FRONTEND_DAI_MAX + 1)
#define CHMIX_CFG_CONST_PARAM_SIZE 4

enum {
	EQ_BAND1 = 0,
	EQ_BAND2,
	EQ_BAND3,
	EQ_BAND4,
	EQ_BAND5,
	EQ_BAND6,
	EQ_BAND7,
	EQ_BAND8,
	EQ_BAND9,
	EQ_BAND10,
	EQ_BAND11,
	EQ_BAND12,
	EQ_BAND_MAX,
};

/* Audio Sphere data structures */
struct msm_audio_pp_asphere_state_s {
	uint32_t enabled;
	uint32_t strength;
	uint32_t mode;
	uint32_t version;
	int  port_id[AFE_MAX_PORTS];
	int  copp_idx[AFE_MAX_PORTS];
	bool  initialized;
	uint32_t enabled_prev;
	uint32_t strength_prev;
};

static struct msm_audio_pp_asphere_state_s asphere_state;

struct msm_audio_eq_stream_config	eq_data[MAX_EQ_SESSIONS];

static int msm_route_hfp_vol_control;
static const DECLARE_TLV_DB_LINEAR(hfp_rx_vol_gain, 0,
				INT_RX_VOL_MAX_STEPS);

static int msm_route_icc_vol_control;
static const DECLARE_TLV_DB_LINEAR(icc_rx_vol_gain, 0,
				INT_RX_VOL_MAX_STEPS);

static int msm_route_pri_auxpcm_lb_vol_ctrl;
static const DECLARE_TLV_DB_LINEAR(pri_auxpcm_lb_vol_gain, 0,
				INT_RX_VOL_MAX_STEPS);

static int msm_route_sec_auxpcm_lb_vol_ctrl;
static const DECLARE_TLV_DB_LINEAR(sec_auxpcm_lb_vol_gain, 0,
				INT_RX_VOL_MAX_STEPS);

static int msm_multichannel_ec_primary_mic_ch;
static int msm_ffecns_effect;

static void msm_qti_pp_send_eq_values_(int eq_idx)
{
	int result;
	struct msm_pcm_routing_fdai_data fe_dai;
	struct audio_client *ac = NULL;

	msm_pcm_routing_get_fedai_info(eq_idx, SESSION_TYPE_RX, &fe_dai);
	ac = q6asm_get_audio_client(fe_dai.strm_id);

	if (ac == NULL) {
		pr_err("%s: Could not get audio client for session: %d\n",
		      __func__, fe_dai.strm_id);
		goto done;
	}

	result = q6asm_equalizer(ac, &eq_data[eq_idx]);

	if (result < 0)
		pr_err("%s: Call to ASM equalizer failed, returned = %d\n",
		      __func__, result);
done:
	return;
}

static int msm_qti_pp_get_eq_enable_mixer(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	int eq_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->reg;

	if ((eq_idx < 0) || (eq_idx >= MAX_EQ_SESSIONS))
		return -EINVAL;

	ucontrol->value.integer.value[0] = eq_data[eq_idx].enable;

	pr_debug("%s: EQ #%d enable %d\n", __func__,
		eq_idx, eq_data[eq_idx].enable);
	return 0;
}

static int msm_qti_pp_put_eq_enable_mixer(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	int eq_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->reg;
	int value = ucontrol->value.integer.value[0];

	if ((eq_idx < 0) || (eq_idx >= MAX_EQ_SESSIONS))
		return -EINVAL;
	pr_debug("%s: EQ #%d enable %d\n", __func__,
		eq_idx, value);
	eq_data[eq_idx].enable = value;
	msm_pcm_routing_acquire_lock();
	msm_qti_pp_send_eq_values_(eq_idx);
	msm_pcm_routing_release_lock();
	return 0;
}

static int msm_qti_pp_get_eq_band_count_audio_mixer(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int eq_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->reg;

	if ((eq_idx < 0) || (eq_idx >= MAX_EQ_SESSIONS))
		return -EINVAL;
	ucontrol->value.integer.value[0] = eq_data[eq_idx].num_bands;

	pr_debug("%s: EQ #%d bands %d\n", __func__,
		eq_idx, eq_data[eq_idx].num_bands);
	return eq_data[eq_idx].num_bands;
}

static int msm_qti_pp_put_eq_band_count_audio_mixer(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int eq_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->reg;
	int value = ucontrol->value.integer.value[0];

	if ((eq_idx < 0) || (eq_idx >= MAX_EQ_SESSIONS))
		return -EINVAL;

	pr_debug("%s: EQ #%d bands %d\n", __func__,
		eq_idx, value);
	eq_data[eq_idx].num_bands = value;
	return 0;
}

static int msm_qti_pp_put_dtmf_module_enable
		(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	u16 fe_id = 0;
	struct msm_pcm_routing_fdai_data fe_dai;
	struct audio_client *ac = NULL;
	struct param_hdr_v3 param_hdr;
	int ret = 0;
	u32 flag = (bool)ucontrol->value.integer.value[0];

	fe_id = ((struct soc_multi_mixer_control *)
			kcontrol->private_value)->shift;
	if (fe_id >= MSM_FRONTEND_DAI_MAX) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return -EINVAL;
	}

	msm_pcm_routing_get_fedai_info(fe_id, SESSION_TYPE_RX, &fe_dai);
	ac = q6asm_get_audio_client(fe_dai.strm_id);

	if (ac == NULL) {
		pr_err("%s ac is null.\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	param_hdr.module_id = AUDPROC_MODULE_ID_DTMF_DETECTION;
	param_hdr.instance_id = INSTANCE_ID_0;
	param_hdr.param_id = AUDPROC_PARAM_ID_ENABLE;
	param_hdr.param_size = 4;

	ret = q6asm_pack_and_set_pp_param_in_band(ac,
			param_hdr, (u8 *)&flag);

done:
	return ret;
}

static int msm_qti_pp_get_eq_band_audio_mixer(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	int eq_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->reg;
	int band_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->shift;

	if ((eq_idx < 0) || (eq_idx >= MAX_EQ_SESSIONS) ||
	    (band_idx < EQ_BAND1) || (band_idx >= EQ_BAND_MAX))
		return -EINVAL;

	ucontrol->value.integer.value[0] =
			eq_data[eq_idx].eq_bands[band_idx].band_idx;
	ucontrol->value.integer.value[1] =
			eq_data[eq_idx].eq_bands[band_idx].filter_type;
	ucontrol->value.integer.value[2] =
			eq_data[eq_idx].eq_bands[band_idx].center_freq_hz;
	ucontrol->value.integer.value[3] =
			eq_data[eq_idx].eq_bands[band_idx].filter_gain;
	ucontrol->value.integer.value[4] =
			eq_data[eq_idx].eq_bands[band_idx].q_factor;

	pr_debug("%s: band_idx = %d\n", __func__,
			eq_data[eq_idx].eq_bands[band_idx].band_idx);
	pr_debug("%s: filter_type = %d\n", __func__,
			eq_data[eq_idx].eq_bands[band_idx].filter_type);
	pr_debug("%s: center_freq_hz = %d\n", __func__,
			eq_data[eq_idx].eq_bands[band_idx].center_freq_hz);
	pr_debug("%s: filter_gain = %d\n", __func__,
			eq_data[eq_idx].eq_bands[band_idx].filter_gain);
	pr_debug("%s: q_factor = %d\n", __func__,
			eq_data[eq_idx].eq_bands[band_idx].q_factor);
	return 0;
}

static int msm_qti_pp_put_eq_band_audio_mixer(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	int eq_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->reg;
	int band_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->shift;

	if ((eq_idx < 0) || (eq_idx >= MAX_EQ_SESSIONS) ||
	    (band_idx < EQ_BAND1) || (band_idx >= EQ_BAND_MAX))
		return -EINVAL;

	eq_data[eq_idx].eq_bands[band_idx].band_idx =
					ucontrol->value.integer.value[0];
	eq_data[eq_idx].eq_bands[band_idx].filter_type =
					ucontrol->value.integer.value[1];
	eq_data[eq_idx].eq_bands[band_idx].center_freq_hz =
					ucontrol->value.integer.value[2];
	eq_data[eq_idx].eq_bands[band_idx].filter_gain =
					ucontrol->value.integer.value[3];
	eq_data[eq_idx].eq_bands[band_idx].q_factor =
					ucontrol->value.integer.value[4];
	return 0;
}

#ifdef CONFIG_QTI_PP
void msm_qti_pp_send_eq_values(int fedai_id)
{
	if (eq_data[fedai_id].enable)
		msm_qti_pp_send_eq_values_(fedai_id);
}

/* CUSTOM MIXING */
int msm_qti_pp_send_stereo_to_custom_stereo_cmd(int port_id, int copp_idx,
						unsigned int session_id,
						uint16_t op_FL_ip_FL_weight,
						uint16_t op_FL_ip_FR_weight,
						uint16_t op_FR_ip_FL_weight,
						uint16_t op_FR_ip_FR_weight)
{
	char *params_value;
	int *update_params_value32, rc = 0;
	int16_t *update_params_value16 = 0;
	uint32_t params_length = CUSTOM_STEREO_PAYLOAD_SIZE * sizeof(uint32_t);
	uint32_t avail_length = params_length;

	pr_debug("%s: port_id - %d, session id - %d\n", __func__, port_id,
		 session_id);
	params_value = kzalloc(params_length, GFP_KERNEL);
	if (!params_value) {
		pr_err("%s, params memory alloc failed\n", __func__);
		return -ENOMEM;
	}
	update_params_value32 = (int *)params_value;
	if (avail_length < 2 * sizeof(uint32_t))
		goto skip_send_cmd;

	/*
	 * This module is internal to ADSP and cannot be configured with
	 * an instance id
	 */
	*update_params_value32++ = MTMX_MODULE_ID_DEFAULT_CHMIXER;
	*update_params_value32++ = DEFAULT_CHMIXER_PARAM_ID_COEFF;
	avail_length = avail_length - (2 * sizeof(uint32_t));

	update_params_value16 = (int16_t *)update_params_value32;
	if (avail_length < 10 * sizeof(uint16_t))
		goto skip_send_cmd;
	*update_params_value16++ = CUSTOM_STEREO_CMD_PARAM_SIZE;
	/*for alignment only*/
	*update_params_value16++ = 0;
	/*index is 32-bit param in little endian*/
	*update_params_value16++ = CUSTOM_STEREO_INDEX_PARAM;
	*update_params_value16++ = 0;
	/*for stereo mixing num out ch*/
	*update_params_value16++ = CUSTOM_STEREO_NUM_OUT_CH;
	/*for stereo mixing num in ch*/
	*update_params_value16++ = CUSTOM_STEREO_NUM_IN_CH;

	/* Out ch map FL/FR*/
	*update_params_value16++ = PCM_CHANNEL_FL;
	*update_params_value16++ = PCM_CHANNEL_FR;

	/* In ch map FL/FR*/
	*update_params_value16++ = PCM_CHANNEL_FL;
	*update_params_value16++ = PCM_CHANNEL_FR;
	avail_length = avail_length - (10 * sizeof(uint16_t));
	/* weighting coefficients as name suggests,
	 * mixing will be done according to these coefficients
	 */
	if (avail_length < 4 * sizeof(uint16_t))
		goto skip_send_cmd;
	*update_params_value16++ = op_FL_ip_FL_weight;
	*update_params_value16++ = op_FL_ip_FR_weight;
	*update_params_value16++ = op_FR_ip_FL_weight;
	*update_params_value16++ = op_FR_ip_FR_weight;
	avail_length = avail_length - (4 * sizeof(uint16_t));
	if (params_length) {
		rc = adm_set_stereo_to_custom_stereo(port_id,
						     copp_idx,
						     session_id,
						     params_value,
						     params_length);
		if (rc) {
			pr_err("%s: send params failed rc=%d\n", __func__, rc);
			kfree(params_value);
			return -EINVAL;
		}
	}
	kfree(params_value);
	return 0;
skip_send_cmd:
		pr_err("%s: insufficient memory, send cmd failed\n",
			__func__);
		kfree(params_value);
		return -ENOMEM;
}

static int msm_qti_pp_arrange_mch_map(int16_t *update_params_value16,
			 int channel_count)
{
	int i;
	int16_t ch_map[PCM_FORMAT_MAX_CHANNELS_9] = {
			PCM_CHANNEL_FL, PCM_CHANNEL_FR, PCM_CHANNEL_FC,
			PCM_CHANNEL_LS, PCM_CHANNEL_RS, PCM_CHANNEL_LFE,
			PCM_CHANNEL_LB, PCM_CHANNEL_RB, PCM_CHANNEL_CS };

	if (channel_count < 1 ||
	    channel_count > PCM_FORMAT_MAX_CHANNELS_9) {
		pr_err("%s: invalid ch_cnt %d\n",
			__func__, channel_count);
		return -EINVAL;
	}

	switch (channel_count) {
	/* Add special cases here */
	case 1:
		*update_params_value16++ = PCM_CHANNEL_FC;
		break;
	case 4:
		*update_params_value16++ = PCM_CHANNEL_FL;
		*update_params_value16++ = PCM_CHANNEL_FR;
		*update_params_value16++ = PCM_CHANNEL_LS;
		*update_params_value16++ = PCM_CHANNEL_RS;
		break;

	/* Add standard cases here */
	default:
		for (i = 0; i < channel_count; i++)
			*update_params_value16++ = ch_map[i];
		break;
	}

	return 0;
}

static uint32_t msm_qti_pp_get_chmix_param_size(int ip_ch_cnt, int op_ch_cnt)
{
	uint32_t param_size;
	/* Assign constant part of param length initially -
	 * Index, Num out channels, Num in channels.
	 */
	param_size = CHMIX_CFG_CONST_PARAM_SIZE * sizeof(uint16_t);

	/* Calculate variable part of param length using ip and op channels */

	/* channel map for input and output channels */
	param_size += op_ch_cnt * sizeof(uint16_t);
	param_size += ip_ch_cnt * sizeof(uint16_t);

	/* weightage coeff for each op ch corresponding to each ip ch */
	param_size += (ip_ch_cnt * op_ch_cnt) * sizeof(uint16_t);

	/* Params length should be multiple of 4 bytes i.e 32bit aligned*/
	param_size = (param_size + 3) & 0xFFFFFFFC;

	return param_size;
}

/*
 * msm_qti_pp_send_chmix_cfg_cmd:
 *	Send the custom channel mixer configuration command.
 *
 * @port_id: Backend port id
 * @copp_idx: ADM copp index
 * @session_id: id for the session requesting channel mixer
 * @ip_channel_cnt: Input channel count
 * @op_channel_cnt: Output channel count
 * @ch_wght_coeff: Channel weight co-efficients for mixing
 * @session_type: Indicates TX or RX session
 * @stream_type: Indicates Audio or Listen stream type
 */
int msm_qti_pp_send_chmix_cfg_cmd(int port_id, int copp_idx,
				unsigned int session_id, int ip_channel_cnt,
				int op_channel_cnt, int *ch_wght_coeff,
				int session_type, int stream_type)
{
	char *params_value;
	int rc = 0, i, direction;
	u8 *param_ptr;
	int16_t *update_params_value16 = 0;
	uint32_t param_size = msm_qti_pp_get_chmix_param_size(ip_channel_cnt,
				op_channel_cnt);
	struct param_hdr_v1 *param_hdr;

	/* constant payload data size represents module_id, param_id,
	 * param size, reserved field.
	 */
	uint32_t params_length = param_size + sizeof(*param_hdr);

	pr_debug("%s: port_id - %d, session id - %d\n", __func__, port_id,
		 session_id);

	params_value = kzalloc(params_length, GFP_KERNEL);
	if (!params_value)
		return -ENOMEM;

	param_ptr = params_value;

	param_hdr = (struct param_hdr_v1 *) param_ptr;
	param_hdr->module_id = MTMX_MODULE_ID_DEFAULT_CHMIXER;
	param_hdr->param_id = DEFAULT_CHMIXER_PARAM_ID_COEFF;
	param_hdr->param_size = param_size;

	param_ptr += sizeof(*param_hdr);

	update_params_value16 = (int16_t *) param_ptr;
	/*for alignment only*/
	*update_params_value16++ = 0;
	/*index is 32-bit param in little endian*/
	*update_params_value16++ = CUSTOM_STEREO_INDEX_PARAM;
	*update_params_value16++ = 0;
	/*number of out ch*/
	*update_params_value16++ = op_channel_cnt;
	/*number of in ch*/
	*update_params_value16++ = ip_channel_cnt;

	/* Out ch map FL/FR*/
	msm_qti_pp_arrange_mch_map(update_params_value16, op_channel_cnt);
	update_params_value16 += op_channel_cnt;

	/* In ch map FL/FR*/
	msm_qti_pp_arrange_mch_map(update_params_value16, ip_channel_cnt);
	update_params_value16 += ip_channel_cnt;

	/* weighting coefficients as name suggests,
	 * mixing will be done according to these coefficients.
	 */
	for (i = 0; i < ip_channel_cnt * op_channel_cnt; i++)
		*update_params_value16++ =
					ch_wght_coeff[i] ? Q14_GAIN_UNITY : 0;
	if (params_length) {
		direction = (session_type == SESSION_TYPE_RX) ?
			ADM_MATRIX_ID_AUDIO_RX : ADM_MATRIX_ID_AUDIO_TX;
		rc = adm_set_custom_chmix_cfg(port_id,
					      copp_idx,
					      session_id,
					      params_value,
					      params_length,
					      direction,
					      stream_type);
		if (rc) {
			pr_err("%s: send params failed rc=%d\n", __func__, rc);
			kfree(params_value);
			return -EINVAL;
		}
	}
	kfree(params_value);
	return 0;
}
EXPORT_SYMBOL(msm_qti_pp_send_chmix_cfg_cmd);
#endif /* CONFIG_QTI_PP */

/* RMS */
static int msm_qti_pp_get_rms_value_control(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	int rc = 0;
	int be_idx = 0, copp_idx;
	char *param_value;
	int *update_param_value;
	uint32_t param_size = (RMS_PAYLOAD_LEN + 1) * sizeof(uint32_t);
	struct msm_pcm_routing_bdai_data msm_bedai;
	struct param_hdr_v3 param_hdr;

	param_value = kzalloc(param_size, GFP_KERNEL);
	if (!param_value)
		return -ENOMEM;
	msm_pcm_routing_acquire_lock();
	for (be_idx = 0; be_idx < MSM_BACKEND_DAI_MAX; be_idx++) {
		msm_pcm_routing_get_bedai_info(be_idx, &msm_bedai);
		if (msm_bedai.port_id == SLIMBUS_0_TX)
			break;
	}
	if ((be_idx >= MSM_BACKEND_DAI_MAX) || !msm_bedai.active) {
		pr_debug("%s, back not active to query rms be_idx:%d\n",
			__func__, be_idx);
		rc = -EINVAL;
		goto get_rms_value_err;
	}
	copp_idx = adm_get_default_copp_idx(SLIMBUS_0_TX);
	if ((copp_idx < 0) || (copp_idx > MAX_COPPS_PER_PORT)) {
		pr_err("%s, no active copp to query rms copp_idx:%d\n",
			__func__, copp_idx);
		rc = -EINVAL;
		goto get_rms_value_err;
	}
	memset(&param_hdr, 0, sizeof(param_hdr));
	param_hdr.module_id = RMS_MODULEID_APPI_PASSTHRU;
	param_hdr.instance_id = INSTANCE_ID_0;
	param_hdr.param_id = RMS_PARAM_FIRST_SAMPLE;
	param_hdr.param_size = param_size;
	rc = adm_get_pp_params(SLIMBUS_0_TX, copp_idx, ADM_CLIENT_ID_DEFAULT,
			       NULL, &param_hdr, param_value);
	if (rc) {
		pr_err("%s: get parameters failed rc=%d\n", __func__, rc);
		rc = -EINVAL;
		goto get_rms_value_err;
	}
	update_param_value = (int *)param_value;
	ucontrol->value.integer.value[0] = update_param_value[0];

	pr_debug("%s: FROM DSP value[0] 0x%x\n",
		__func__, update_param_value[0]);
get_rms_value_err:
	msm_pcm_routing_release_lock();
	kfree(param_value);
	return rc;
}

static int msm_qti_pp_put_rms_value_control(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	/* not used */
	return 0;
}

/* VOLUME */
static int msm_route_dp_vol_control;
static int msm_route_fm_vol_control;
static int msm_afe_lb_vol_ctrl;
static int msm_afe_sec_mi2s_lb_vol_ctrl;
static int msm_afe_tert_mi2s_lb_vol_ctrl;
static int msm_afe_cdc_tx_3_lb_vol_ctrl;
static int msm_afe_usb_lb_vol_ctrl;
static int msm_afe_quat_mi2s_lb_vol_ctrl;
static int msm_afe_slimbus_7_lb_vol_ctrl;
static int msm_afe_slimbus_8_lb_vol_ctrl;
static int msm_asm_bit_width;
static const DECLARE_TLV_DB_LINEAR(fm_rx_vol_gain, 0, INT_RX_VOL_MAX_STEPS);
static const DECLARE_TLV_DB_LINEAR(afe_lb_vol_gain, 0, INT_RX_VOL_MAX_STEPS);


static int msm_qti_pp_get_dp_vol_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_route_dp_vol_control;
	return 0;
}

static int msm_qti_pp_set_dp_vol_mixer(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	afe_loopback_gain(AFE_PORT_ID_TX_CODEC_DMA_TX_3, ucontrol->value.integer.value[0]);

	msm_route_dp_vol_control = ucontrol->value.integer.value[0];

	return 0;
}

static int msm_qti_pp_get_fm_vol_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_route_fm_vol_control;
	return 0;
}

static int msm_qti_pp_set_fm_vol_mixer(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	afe_loopback_gain(INT_FM_TX, ucontrol->value.integer.value[0]);

	msm_route_fm_vol_control = ucontrol->value.integer.value[0];

	return 0;
}

static int msm_asm_bit_width_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s get ASM bitwidth = %d\n",
		__func__, msm_asm_bit_width);

	ucontrol->value.integer.value[0] = msm_asm_bit_width;

	return 0;
}

static int msm_asm_bit_width_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 16:
		msm_asm_bit_width = 16;
		break;
	case 24:
		msm_asm_bit_width = 24;
		break;
	case 32:
		msm_asm_bit_width = 32;
		break;
	default:
		msm_asm_bit_width = 0;
		break;
	}

	return 0;
}

static int msm_qti_pp_get_pri_mi2s_lb_vol_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_afe_lb_vol_ctrl;
	return 0;
}

static int msm_qti_pp_set_pri_mi2s_lb_vol_mixer(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	afe_loopback_gain(AFE_PORT_ID_PRIMARY_MI2S_TX,
			  ucontrol->value.integer.value[0]);

	msm_afe_lb_vol_ctrl = ucontrol->value.integer.value[0];

	return 0;
}

static int msm_qti_pp_get_sec_mi2s_lb_vol_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_afe_sec_mi2s_lb_vol_ctrl;
	return 0;
}

static int msm_qti_pp_set_sec_mi2s_lb_vol_mixer(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	afe_loopback_gain(AFE_PORT_ID_SECONDARY_MI2S_TX,
			  ucontrol->value.integer.value[0]);
	msm_afe_sec_mi2s_lb_vol_ctrl = ucontrol->value.integer.value[0];

	return 0;
}

static int msm_qti_pp_get_tert_mi2s_lb_vol_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_afe_tert_mi2s_lb_vol_ctrl;
	return 0;
}

static int msm_qti_pp_set_tert_mi2s_lb_vol_mixer(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	afe_loopback_gain(AFE_PORT_ID_TERTIARY_MI2S_TX,
			  ucontrol->value.integer.value[0]);
	msm_afe_tert_mi2s_lb_vol_ctrl = ucontrol->value.integer.value[0];
	return 0;
}

static int msm_qti_pp_get_usb_tx_lb_vol_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_afe_usb_lb_vol_ctrl;
	return 0;
}

static int msm_qti_pp_set_usb_tx_lb_vol_mixer(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	afe_loopback_gain(AFE_PORT_ID_USB_TX,
			  ucontrol->value.integer.value[0]);
	msm_afe_usb_lb_vol_ctrl = ucontrol->value.integer.value[0];
	return 0;
}

static int msm_qti_pp_get_cdc_tx_3_lb_vol_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_afe_cdc_tx_3_lb_vol_ctrl;
	return 0;
}

static int msm_qti_pp_set_cdc_tx_3_lb_vol_mixer(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	afe_loopback_gain(AFE_PORT_ID_TX_CODEC_DMA_TX_3,
			  ucontrol->value.integer.value[0]);
	msm_afe_cdc_tx_3_lb_vol_ctrl = ucontrol->value.integer.value[0];
	return 0;
}

static int msm_qti_pp_get_slimbus_7_lb_vol_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_afe_slimbus_7_lb_vol_ctrl;
	return 0;
}

static int msm_qti_pp_set_slimbus_7_lb_vol_mixer(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	int ret = afe_loopback_gain(SLIMBUS_7_TX,
				ucontrol->value.integer.value[0]);

	if (ret)
		pr_err("%s: failed to set LB vol for SLIMBUS_7_TX, err %d\n",
			__func__, ret);
	else
		msm_afe_slimbus_7_lb_vol_ctrl =
				ucontrol->value.integer.value[0];

	return ret;
}

static int msm_qti_pp_get_slimbus_8_lb_vol_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_afe_slimbus_8_lb_vol_ctrl;
	return 0;
}

static int msm_qti_pp_set_slimbus_8_lb_vol_mixer(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;

	ret = afe_loopback_gain(SLIMBUS_8_TX,
				ucontrol->value.integer.value[0]);

	if (ret)
		pr_err("%s: failed to set LB vol for SLIMBUS_8_TX", __func__);
	else
		msm_afe_slimbus_8_lb_vol_ctrl =
				ucontrol->value.integer.value[0];

	return ret;
}

static int msm_qti_pp_get_icc_vol_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_route_icc_vol_control;
	return 0;
}

static int msm_qti_pp_set_icc_vol_mixer(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	adm_set_mic_gain(AFE_PORT_ID_QUATERNARY_TDM_TX,
		adm_get_default_copp_idx(AFE_PORT_ID_QUATERNARY_TDM_TX),
		ucontrol->value.integer.value[0]);
	msm_route_icc_vol_control = ucontrol->value.integer.value[0];
	return 0;
}

static int msm_qti_pp_get_quat_mi2s_fm_vol_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_afe_quat_mi2s_lb_vol_ctrl;
	return 0;
}

static int msm_qti_pp_set_quat_mi2s_fm_vol_mixer(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	afe_loopback_gain(AFE_PORT_ID_QUATERNARY_MI2S_TX,
			  ucontrol->value.integer.value[0]);

	msm_afe_quat_mi2s_lb_vol_ctrl = ucontrol->value.integer.value[0];

	return 0;
}

static int msm_qti_pp_get_hfp_vol_mixer(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_route_hfp_vol_control;
	return 0;
}

static int msm_qti_pp_set_hfp_vol_mixer(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	afe_loopback_gain(INT_BT_SCO_TX, ucontrol->value.integer.value[0]);

	msm_route_hfp_vol_control = ucontrol->value.integer.value[0];

	return 0;
}

static int msm_qti_pp_get_pri_auxpcm_lb_vol_mixer(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_route_pri_auxpcm_lb_vol_ctrl;
	pr_debug("%s: Volume = %ld\n", __func__,
		ucontrol->value.integer.value[0]);
	return 0;
}

static int msm_qti_pp_set_pri_auxpcm_lb_vol_mixer(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
	(struct soc_mixer_control *)kcontrol->private_value;

	afe_loopback_gain(mc->reg, ucontrol->value.integer.value[0]);

	msm_route_pri_auxpcm_lb_vol_ctrl = ucontrol->value.integer.value[0];

	return 0;
}

static int msm_qti_pp_get_sec_auxpcm_lb_vol_mixer(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_route_sec_auxpcm_lb_vol_ctrl;
	pr_debug("%s: Volume = %ld\n", __func__,
		ucontrol->value.integer.value[0]);
	return 0;
}

static int msm_qti_pp_set_sec_auxpcm_lb_vol_mixer(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
	(struct soc_mixer_control *)kcontrol->private_value;

	afe_loopback_gain(mc->reg, ucontrol->value.integer.value[0]);

	msm_route_sec_auxpcm_lb_vol_ctrl = ucontrol->value.integer.value[0];

	return 0;
}

static int msm_qti_pp_get_channel_map_mixer(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	char channel_map[PCM_FORMAT_MAX_NUM_CHANNEL_V8] = {0};
	int i;

	adm_get_multi_ch_map(channel_map, ADM_PATH_PLAYBACK);
	for (i = 0; i < PCM_FORMAT_MAX_NUM_CHANNEL_V8; i++)
		ucontrol->value.integer.value[i] =
			(unsigned int) channel_map[i];
	return 0;
}

static int msm_qti_pp_put_channel_map_mixer(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	char channel_map[PCM_FORMAT_MAX_NUM_CHANNEL_V8];
	int i;

	for (i = 0; i < PCM_FORMAT_MAX_NUM_CHANNEL_V8; i++)
		channel_map[i] = (char)(ucontrol->value.integer.value[i]);
	adm_set_multi_ch_map(channel_map, ADM_PATH_PLAYBACK);

	return 0;
}

static int msm_qti_pp_get_channel_map_capture(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	char channel_map[PCM_FORMAT_MAX_NUM_CHANNEL_V8] = {0};
	int i;

	adm_get_multi_ch_map(channel_map, ADM_PATH_LIVE_REC);
	for (i = 0; i < PCM_FORMAT_MAX_NUM_CHANNEL_V8; i++)
		ucontrol->value.integer.value[i] =
			(unsigned int) channel_map[i];
	return 0;
}

static int msm_qti_pp_put_channel_map_capture(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	char channel_map[PCM_FORMAT_MAX_NUM_CHANNEL_V8];
	int i;

	for (i = 0; i < PCM_FORMAT_MAX_NUM_CHANNEL_V8; i++)
		channel_map[i] = (char)(ucontrol->value.integer.value[i]);
	adm_set_multi_ch_map(channel_map, ADM_PATH_LIVE_REC);

	return 0;
}

/* Audio Sphere functions */

static void msm_qti_pp_asphere_init_state(void)
{
	int i;

	if (asphere_state.initialized)
		return;
	asphere_state.initialized = true;
	for (i = 0; i < AFE_MAX_PORTS; i++) {
		asphere_state.port_id[i] = -1;
		asphere_state.copp_idx[i] = -1;
	}
	asphere_state.enabled = 0;
	asphere_state.strength = 0;
	asphere_state.mode = 0;
	asphere_state.version = 0;
	asphere_state.enabled_prev = 0;
	asphere_state.strength_prev = 0;
}

static int msm_qti_pp_asphere_send_params(int port_id, int copp_idx, bool force)
{
	u8 *packed_params = NULL;
	u32 packed_params_size = 0;
	u32 param_size = 0;
	struct param_hdr_v3 param_hdr;
	bool set_enable = force ||
			(asphere_state.enabled != asphere_state.enabled_prev);
	bool set_strength = asphere_state.enabled == 1 && (set_enable ||
		(asphere_state.strength != asphere_state.strength_prev));
	int param_count = 0;
	int ret = 0;

	if (set_enable)
		param_count++;
	if (set_strength)
		param_count++;

	if (param_count == 0) {
		pr_debug("%s: Nothing to send, exiting\n", __func__);
		return 0;
	}

	pr_debug("%s: port_id %d, copp_id %d, forced %d, param_count %d\n",
		 __func__, port_id, copp_idx, force, param_count);
	pr_debug("%s: enable prev:%u cur:%u, strength prev:%u cur:%u\n",
		__func__, asphere_state.enabled_prev, asphere_state.enabled,
		asphere_state.strength_prev, asphere_state.strength);

	packed_params_size =
		param_count * (sizeof(struct param_hdr_v3) + sizeof(uint32_t));
	packed_params = kzalloc(packed_params_size, GFP_KERNEL);
	if (!packed_params)
		return -ENOMEM;

	memset(&param_hdr, 0, sizeof(param_hdr));
	packed_params_size = 0;
	param_hdr.module_id = AUDPROC_MODULE_ID_AUDIOSPHERE;
	param_hdr.instance_id = INSTANCE_ID_0;
	if (set_strength) {
		/* add strength command */
		param_hdr.param_id = AUDPROC_PARAM_ID_AUDIOSPHERE_STRENGTH;
		param_hdr.param_size = sizeof(asphere_state.strength);
		ret = q6common_pack_pp_params(packed_params +
						      packed_params_size,
					      &param_hdr,
					      (u8 *) &asphere_state.strength,
					      &param_size);
		if (ret) {
			pr_err("%s: Failed to pack params for audio sphere"
				" strength, error %d\n", __func__, ret);
			goto done;
		}
		packed_params_size += param_size;
	}
	if (set_enable) {
		/* add enable command */
		param_hdr.param_id = AUDPROC_PARAM_ID_AUDIOSPHERE_ENABLE;
		param_hdr.param_size = sizeof(asphere_state.enabled);
		q6common_pack_pp_params(packed_params + packed_params_size,
					&param_hdr,
					(u8 *) &asphere_state.enabled,
					&param_size);
		if (ret) {
			pr_err("%s: Failed to pack params for audio sphere"
				" enable, error %d\n", __func__, ret);
			goto done;
		}
		packed_params_size += param_size;
	}

	pr_debug("%s: packed data size: %d\n", __func__, packed_params_size);
	ret = adm_set_pp_params(port_id, copp_idx, NULL, packed_params,
				packed_params_size);
	if (ret)
		pr_err("%s: set param failed with err=%d\n", __func__, ret);

done:
	kfree(packed_params);
	return 0;
}

#if defined(CONFIG_QTI_PP) && defined(CONFIG_QTI_PP_AUDIOSPHERE)
int msm_qti_pp_asphere_init(int port_id, int copp_idx)
{
	int index = adm_validate_and_get_port_index(port_id);

	pr_debug("%s, port_id %d, copp_id %d\n", __func__, port_id, copp_idx);
	if (index < 0) {
		pr_err("%s: Invalid port idx %d port_id %#x\n", __func__, index,
			port_id);
		return -EINVAL;
	}
	msm_qti_pp_asphere_init_state();

	asphere_state.port_id[index] = port_id;
	asphere_state.copp_idx[index] = copp_idx;

	if (asphere_state.enabled)
		msm_qti_pp_asphere_send_params(port_id, copp_idx, true);

	return 0;
}

void msm_qti_pp_asphere_deinit(int port_id)
{
	int index = adm_validate_and_get_port_index(port_id);

	pr_debug("%s, port_id %d\n", __func__, port_id);
	if (index < 0) {
		pr_err("%s: Invalid port idx %d port_id %#x\n", __func__, index,
			port_id);
		return;
	}

	if (asphere_state.port_id[index] == port_id) {
		asphere_state.port_id[index] = -1;
		asphere_state.copp_idx[index] = -1;
	}
}
#endif

static int msm_qti_pp_asphere_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	if (!asphere_state.initialized)
		return -EAGAIN;
	ucontrol->value.integer.value[0] = asphere_state.enabled;
	ucontrol->value.integer.value[1] = asphere_state.strength;
	pr_debug("%s, enable %u, strength %u\n", __func__,
			asphere_state.enabled, asphere_state.strength);
	return 0;
}

static int msm_qti_pp_asphere_set(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int32_t enable = ucontrol->value.integer.value[0];
	int32_t strength = ucontrol->value.integer.value[1];
	int i;

	pr_debug("%s, enable %u, strength %u\n", __func__, enable, strength);

	msm_qti_pp_asphere_init_state();

	if (enable == 0 || enable == 1) {
		asphere_state.enabled_prev = asphere_state.enabled;
		asphere_state.enabled = enable;
	}

	if (strength >= 0 && strength <= 1000) {
		asphere_state.strength_prev = asphere_state.strength;
		asphere_state.strength = strength;
	}

	if (asphere_state.strength != asphere_state.strength_prev ||
		asphere_state.enabled != asphere_state.enabled_prev) {
		for (i = 0; i < AFE_MAX_PORTS; i++) {
			if (asphere_state.port_id[i] >= 0)
				msm_qti_pp_asphere_send_params(
					asphere_state.port_id[i],
					asphere_state.copp_idx[i],
					false);
		}
	}
	return 0;
}

int msm_adsp_init_mixer_ctl_pp_event_queue(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_kcontrol *kctl;
	const char *deviceNo = "NN";
	char *mixer_str = NULL;
	int ctl_len = 0, ret = 0;
	const char *mixer_ctl_name = DSP_STREAM_CALLBACK;
	struct dsp_stream_callback_prtd *kctl_prtd = NULL;

	if (!rtd) {
		pr_err("%s: rtd is NULL\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	ctl_len = strlen(mixer_ctl_name) + 1 + strlen(deviceNo) + 1;
	mixer_str = kzalloc(ctl_len, GFP_KERNEL);
	if (!mixer_str) {
		ret = -EINVAL;
		goto done;
	}

	snprintf(mixer_str, ctl_len, "%s %d", mixer_ctl_name,
		rtd->pcm->device);
	kctl = snd_soc_card_get_kcontrol(rtd->card, mixer_str);
	kfree(mixer_str);
	if (!kctl) {
		pr_err("%s: failed to get kctl.\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	if (kctl->private_data != NULL) {
		pr_err("%s: kctl_prtd is not NULL at initialization.\n",
			__func__);
		return -EINVAL;
	}

	kctl_prtd = kzalloc(sizeof(struct dsp_stream_callback_prtd),
			GFP_KERNEL);
	if (!kctl_prtd) {
		ret = -ENOMEM;
		goto done;
	}

	spin_lock_init(&kctl_prtd->prtd_spin_lock);
	INIT_LIST_HEAD(&kctl_prtd->event_queue);
	kctl_prtd->event_count = 0;
	kctl->private_data = kctl_prtd;

done:
	return ret;
}

int msm_adsp_clean_mixer_ctl_pp_event_queue(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_kcontrol *kctl;
	const char *deviceNo = "NN";
	char *mixer_str = NULL;
	int ctl_len = 0, ret = 0;
	struct dsp_stream_callback_list *node, *n;
	unsigned long spin_flags;
	const char *mixer_ctl_name = DSP_STREAM_CALLBACK;
	struct dsp_stream_callback_prtd *kctl_prtd = NULL;

	if (!rtd) {
		pr_err("%s: rtd is NULL\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	ctl_len = strlen(mixer_ctl_name) + 1 + strlen(deviceNo) + 1;
	mixer_str = kzalloc(ctl_len, GFP_KERNEL);
	if (!mixer_str) {
		ret = -EINVAL;
		goto done;
	}

	snprintf(mixer_str, ctl_len, "%s %d", mixer_ctl_name,
		rtd->pcm->device);
	kctl = snd_soc_card_get_kcontrol(rtd->card, mixer_str);
	kfree(mixer_str);
	if (!kctl) {
		pr_err("%s: failed to get kctl.\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	kctl_prtd = (struct dsp_stream_callback_prtd *)
			kctl->private_data;
	if (kctl_prtd != NULL) {
		spin_lock_irqsave(&kctl_prtd->prtd_spin_lock, spin_flags);
		/* clean the queue */
		list_for_each_entry_safe(node, n,
				&kctl_prtd->event_queue, list) {
			list_del(&node->list);
			kctl_prtd->event_count--;
			pr_debug("%s: %d remaining events after del.\n",
				__func__, kctl_prtd->event_count);
			kfree(node);
		}
		spin_unlock_irqrestore(&kctl_prtd->prtd_spin_lock, spin_flags);
	}

	kfree(kctl_prtd);
	kctl->private_data = NULL;

done:
	return ret;
}

int msm_adsp_init_mixer_ctl_adm_pp_event_queue(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_kcontrol *kctl = NULL;
	char *mixer_str = NULL;
	int ctl_len = 0, ret = 0;
	const char *mixer_ctl_name = DSP_ADM_CALLBACK;
	struct dsp_adm_callback_prtd *kctl_prtd = NULL;

	if (!rtd) {
		pr_err("%s: rtd is NULL\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	ctl_len = strlen(mixer_ctl_name) + 1;
	mixer_str = kzalloc(ctl_len, GFP_KERNEL);
	if (!mixer_str) {
		ret = -EINVAL;
		goto done;
	}

	snprintf(mixer_str, ctl_len, "%s", mixer_ctl_name);
	kctl = snd_soc_card_get_kcontrol(rtd->card, mixer_str);
	kfree(mixer_str);
	if (!kctl) {
		pr_err("%s: failed to get kctl.\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	if (kctl->private_data != NULL) {
		pr_err("%s: kctl_prtd is not NULL at initialization.\n",
			__func__);
		return -EINVAL;
	}

	kctl_prtd = kzalloc(sizeof(struct dsp_adm_callback_prtd),
			GFP_KERNEL);
	if (!kctl_prtd) {
		ret = -ENOMEM;
		goto done;
	}

	spin_lock_init(&kctl_prtd->prtd_spin_lock);
	INIT_LIST_HEAD(&kctl_prtd->event_queue);
	kctl_prtd->event_count = 0;
	kctl->private_data = kctl_prtd;

done:
	return ret;
}

int msm_adsp_clean_mixer_ctl_adm_pp_event_queue(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_kcontrol *kctl = NULL;
	char *mixer_str = NULL;
	int ctl_len = 0, ret = 0;
	struct dsp_adm_callback_list *node = NULL, *n = NULL;
	unsigned long spin_flags = 0;
	const char *mixer_ctl_name = DSP_ADM_CALLBACK;
	struct dsp_adm_callback_prtd *kctl_prtd = NULL;

	if (!rtd) {
		pr_err("%s: rtd is NULL\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	ctl_len = strlen(mixer_ctl_name) + 1;
	mixer_str = kzalloc(ctl_len, GFP_KERNEL);
	if (!mixer_str) {
		ret = -EINVAL;
		goto done;
	}

	snprintf(mixer_str, ctl_len, "%s", mixer_ctl_name);
	kctl = snd_soc_card_get_kcontrol(rtd->card, mixer_str);
	kfree(mixer_str);
	if (!kctl) {
		pr_err("%s: failed to get kctl.\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	kctl_prtd = (struct dsp_adm_callback_prtd *)
			kctl->private_data;
	if (kctl_prtd != NULL) {
		spin_lock_irqsave(&kctl_prtd->prtd_spin_lock, spin_flags);
		/* clean the queue */
		list_for_each_entry_safe(node, n,
				&kctl_prtd->event_queue, list) {
			list_del(&node->list);
			kctl_prtd->event_count--;
			pr_debug("%s: %d remaining events after del.\n",
				__func__, kctl_prtd->event_count);
			kfree(node);
		}
		spin_unlock_irqrestore(&kctl_prtd->prtd_spin_lock, spin_flags);
	}

	kfree(kctl_prtd);
	kctl->private_data = NULL;

done:
	return ret;
}

int msm_adsp_copp_inform_mixer_ctl(struct snd_soc_pcm_runtime *rtd,
			uint32_t *payload)
{
	/* adsp adm pp event notifier */
	struct snd_kcontrol *kctl = NULL;
	struct snd_ctl_elem_value control = {0};
	char *mixer_str = NULL;
	int ctl_len = 0, ret = 0;
	struct dsp_adm_callback_list *new_event = NULL;
	struct dsp_adm_callback_list *oldest_event = NULL;
	unsigned long spin_flags = 0;
	struct dsp_adm_callback_prtd *kctl_prtd = NULL;
	struct msm_adsp_event_data *event_data = NULL;
	const char *mixer_ctl_name = DSP_ADM_CALLBACK;
	struct snd_ctl_elem_info kctl_info = {0};

	if (!rtd || !payload) {
		pr_err("%s: %s is NULL\n", __func__,
			(!rtd) ? "rtd" : "payload");
		ret = -EINVAL;
		goto done;
	}

	if (rtd->card->snd_card == NULL) {
		pr_err("%s: snd_card is null.\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	ctl_len = strlen(mixer_ctl_name) + 1;
	mixer_str = kzalloc(ctl_len, GFP_ATOMIC);
	if (!mixer_str) {
		ret = -EINVAL;
		goto done;
	}

	snprintf(mixer_str, ctl_len, "%s", mixer_ctl_name);
	kctl = snd_soc_card_get_kcontrol(rtd->card, mixer_str);
	kfree(mixer_str);
	if (!kctl) {
		pr_err("%s: failed to get kctl.\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	event_data = (struct msm_adsp_event_data *)payload;
	kctl->info(kctl, &kctl_info);
	if (sizeof(struct msm_adsp_event_data)
		+ event_data->payload_len > kctl_info.count) {
		pr_err("%s: payload length exceeds limit of %u bytes.\n",
			__func__, kctl_info.count);
		ret = -EINVAL;
		goto done;
	}

	kctl_prtd = (struct dsp_adm_callback_prtd *)
			kctl->private_data;
	if (kctl_prtd == NULL) {
		/* queue is not initialized */
		ret = -EINVAL;
		pr_err("%s: event queue is not initialized.\n", __func__);
		goto done;
	}

	new_event = kzalloc(sizeof(struct dsp_adm_callback_list)
			+ event_data->payload_len + sizeof(uint32_t),
			GFP_ATOMIC);
	if (new_event == NULL) {
		ret = -ENOMEM;
		goto done;
	}
	memcpy((void *)&new_event->event, (void *)payload,
		   event_data->payload_len
		   + sizeof(struct msm_adsp_event_data));


	spin_lock_irqsave(&kctl_prtd->prtd_spin_lock, spin_flags);
	while (kctl_prtd->event_count >= DSP_ADM_CALLBACK_QUEUE_SIZE) {
		pr_info("%s: queue of size %d is full. delete oldest one.\n",
			__func__, DSP_ADM_CALLBACK_QUEUE_SIZE);
		oldest_event = list_first_entry(&kctl_prtd->event_queue,
				struct dsp_adm_callback_list, list);
		pr_info("%s: event deleted: type %d length %d\n",
			__func__, oldest_event->event.event_type,
			oldest_event->event.payload_len);
		list_del(&oldest_event->list);
		kctl_prtd->event_count--;
		kfree(oldest_event);
	}

	list_add_tail(&new_event->list, &kctl_prtd->event_queue);
	kctl_prtd->event_count++;
	spin_unlock_irqrestore(&kctl_prtd->prtd_spin_lock, spin_flags);

	control.id = kctl->id;

	snd_ctl_notify(rtd->card->snd_card,
			SNDRV_CTL_EVENT_MASK_INFO,
			&control.id);

done:
	return ret;
}

int msm_adsp_inform_mixer_ctl(struct snd_soc_pcm_runtime *rtd,
			uint32_t *payload)
{
	/* adsp pp event notifier */
	struct snd_kcontrol *kctl;
	struct snd_ctl_elem_value control;
	const char *deviceNo = "NN";
	char *mixer_str = NULL;
	int ctl_len = 0, ret = 0;
	struct dsp_stream_callback_list *new_event;
	struct dsp_stream_callback_list *oldest_event;
	unsigned long spin_flags = 0;
	struct dsp_stream_callback_prtd *kctl_prtd = NULL;
	struct msm_adsp_event_data *event_data = NULL;
	const char *mixer_ctl_name = DSP_STREAM_CALLBACK;
	struct snd_ctl_elem_info kctl_info;

	if (!rtd || !payload) {
		pr_err("%s: %s is NULL\n", __func__,
			(!rtd) ? "rtd" : "payload");
		ret = -EINVAL;
		goto done;
	}

	if (rtd->card->snd_card == NULL) {
		pr_err("%s: snd_card is null.\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	ctl_len = strlen(mixer_ctl_name) + 1 + strlen(deviceNo) + 1;
	mixer_str = kzalloc(ctl_len, GFP_ATOMIC);
	if (!mixer_str) {
		ret = -EINVAL;
		goto done;
	}

	snprintf(mixer_str, ctl_len, "%s %d", mixer_ctl_name,
		rtd->pcm->device);
	kctl = snd_soc_card_get_kcontrol(rtd->card, mixer_str);
	kfree(mixer_str);
	if (!kctl) {
		pr_err("%s: failed to get kctl.\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	event_data = (struct msm_adsp_event_data *)payload;
	kctl->info(kctl, &kctl_info);

	if (event_data->payload_len >
		kctl_info.count - sizeof(struct msm_adsp_event_data)) {
		pr_err("%s: payload length exceeds limit of %u bytes.\n",
			__func__, kctl_info.count);
		ret = -EINVAL;
		goto done;
	}

	kctl_prtd = (struct dsp_stream_callback_prtd *)
			kctl->private_data;
	if (kctl_prtd == NULL) {
		/* queue is not initialized */
		ret = -EINVAL;
		pr_err("%s: event queue is not initialized.\n", __func__);
		goto done;
	}

	new_event = kzalloc(sizeof(struct dsp_stream_callback_list)
			+ event_data->payload_len,
			GFP_ATOMIC);
	if (new_event == NULL) {
		ret = -ENOMEM;
		goto done;
	}
	memcpy((void *)&new_event->event, (void *)payload,
		   event_data->payload_len
		   + sizeof(struct msm_adsp_event_data));

	spin_lock_irqsave(&kctl_prtd->prtd_spin_lock, spin_flags);
	while (kctl_prtd->event_count >= DSP_STREAM_CALLBACK_QUEUE_SIZE) {
		pr_debug("%s: queue of size %d is full. delete oldest one.\n",
			__func__, DSP_STREAM_CALLBACK_QUEUE_SIZE);
		oldest_event = list_first_entry(&kctl_prtd->event_queue,
				struct dsp_stream_callback_list, list);
		pr_debug("%s: event deleted: type %d length %d\n",
			__func__, oldest_event->event.event_type,
			oldest_event->event.payload_len);
		list_del(&oldest_event->list);
		kctl_prtd->event_count--;
		kfree(oldest_event);
	}

	list_add_tail(&new_event->list, &kctl_prtd->event_queue);
	kctl_prtd->event_count++;
	spin_unlock_irqrestore(&kctl_prtd->prtd_spin_lock, spin_flags);

	control.id = kctl->id;
	snd_ctl_notify(rtd->card->snd_card,
			SNDRV_CTL_EVENT_MASK_INFO,
			&control.id);

done:
	return ret;
}

int msm_adsp_stream_cmd_info(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count =
		sizeof(((struct snd_ctl_elem_value *)0)->value.bytes.data);

	return 0;
}

int msm_adsp_stream_callback_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	uint32_t payload_size = 0;
	struct dsp_stream_callback_list *oldest_event;
	unsigned long spin_flags = 0;
	struct dsp_stream_callback_prtd *kctl_prtd = NULL;
	int ret = 0;

	kctl_prtd = (struct dsp_stream_callback_prtd *)
			kcontrol->private_data;
	if (kctl_prtd == NULL) {
		pr_debug("%s: ASM Stream PP event queue is not initialized.\n",
			__func__);
		ret = -EINVAL;
		goto done;
	}

	spin_lock_irqsave(&kctl_prtd->prtd_spin_lock, spin_flags);
	pr_debug("%s: %d events in queue.\n", __func__, kctl_prtd->event_count);
	if (list_empty(&kctl_prtd->event_queue)) {
		pr_err("%s: ASM Stream PP event queue is empty.\n", __func__);
		ret = -EINVAL;
		spin_unlock_irqrestore(&kctl_prtd->prtd_spin_lock, spin_flags);
		goto done;
	}

	oldest_event = list_first_entry(&kctl_prtd->event_queue,
			struct dsp_stream_callback_list, list);
	list_del(&oldest_event->list);
	kctl_prtd->event_count--;
	spin_unlock_irqrestore(&kctl_prtd->prtd_spin_lock, spin_flags);

	payload_size = oldest_event->event.payload_len;
	pr_debug("%s: event fetched: type %d length %d\n",
			__func__, oldest_event->event.event_type,
			oldest_event->event.payload_len);
	memcpy(ucontrol->value.bytes.data, &oldest_event->event,
		sizeof(struct msm_adsp_event_data) + payload_size);
	kfree(oldest_event);

done:
	return ret;
}

int msm_adsp_stream_callback_info(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count =
		sizeof(((struct snd_ctl_elem_value *)0)->value.bytes.data);

	return 0;
}

static int msm_multichannel_ec_primary_mic_ch_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	int copp_idx = 0;
	int port_id = AFE_PORT_ID_QUATERNARY_TDM_TX;

	msm_multichannel_ec_primary_mic_ch = ucontrol->value.integer.value[0];
	pr_debug("%s: msm_multichannel_ec_primary_mic_ch = %u\n",
		__func__, msm_multichannel_ec_primary_mic_ch);
	copp_idx = adm_get_default_copp_idx(port_id);
	if ((copp_idx < 0) || (copp_idx > MAX_COPPS_PER_PORT)) {
		pr_err("%s : no active copp to query multichannel ec copp_idx: %u\n",
			__func__, copp_idx);
		return -EINVAL;
	}
	adm_send_set_multichannel_ec_primary_mic_ch(port_id, copp_idx,
		msm_multichannel_ec_primary_mic_ch);

	return ret;
}

static int msm_multichannel_ec_primary_mic_ch_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_multichannel_ec_primary_mic_ch;
	pr_debug("%s: msm_multichannel_ec_primary_mic_ch = %lu\n",
		__func__, ucontrol->value.integer.value[0]);
	return 0;
}

static const struct  snd_kcontrol_new msm_multichannel_ec_controls[] = {
	SOC_SINGLE_EXT("Multichannel EC Primary Mic Ch", SND_SOC_NOPM, 0,
		0xFFFFFFFF, 0, msm_multichannel_ec_primary_mic_ch_get,
		msm_multichannel_ec_primary_mic_ch_put),
};

static char const *ffecns_effect_text[] = {"NO_EFFECT", "EC_ONLY", "NS_ONLY", "ECNS"};

static int msm_ffecns_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int ret = -EINVAL;

	if (ucontrol->value.integer.value[0] < 0 ||
		ucontrol->value.integer.value[0] >= ARRAY_SIZE(ffecns_effect_text)) {
		pr_err("%s: invalid ffecns effect value %ld\n",
			__func__, ucontrol->value.integer.value[0]);
		return -EINVAL;
	}

	msm_ffecns_effect = ucontrol->value.integer.value[0];

	pr_debug("%s: set %s for ffecns\n", __func__,
		ffecns_effect_text[msm_ffecns_effect]);

	ret = adm_set_ffecns_effect(msm_ffecns_effect);
	if (ret)
		pr_err("%s: failed to set %s for ffecns\n",
			__func__, ffecns_effect_text[msm_ffecns_effect]);

	return ret;
}

static int msm_ffecns_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_ffecns_effect;
	pr_debug("%s: ffecns effect = %ld\n",
		__func__, ucontrol->value.integer.value[0]);

	return 0;
}

static SOC_ENUM_SINGLE_EXT_DECL(ffecns_effect_enum, ffecns_effect_text);

static const struct snd_kcontrol_new ec_ffecns_controls[] = {
	SOC_ENUM_EXT("FFECNS Effect", ffecns_effect_enum,
		msm_ffecns_get, msm_ffecns_put),
};

static const struct snd_kcontrol_new int_dp_vol_mixer_controls[] = {
	SOC_SINGLE_EXT_TLV("DISPLAY_PORT Loopback Volume", SND_SOC_NOPM, 0,
	INT_RX_VOL_GAIN, 0, msm_qti_pp_get_dp_vol_mixer,
	msm_qti_pp_set_dp_vol_mixer, afe_lb_vol_gain),
};

static const struct snd_kcontrol_new int_fm_vol_mixer_controls[] = {
	SOC_SINGLE_EXT_TLV("Internal FM RX Volume", SND_SOC_NOPM, 0,
	INT_RX_VOL_GAIN, 0, msm_qti_pp_get_fm_vol_mixer,
	msm_qti_pp_set_fm_vol_mixer, fm_rx_vol_gain),
	SOC_SINGLE_EXT_TLV("Quat MI2S FM RX Volume", SND_SOC_NOPM, 0,
	INT_RX_VOL_GAIN, 0, msm_qti_pp_get_quat_mi2s_fm_vol_mixer,
	msm_qti_pp_set_quat_mi2s_fm_vol_mixer, fm_rx_vol_gain),
};

static const struct snd_kcontrol_new dsp_bit_width_controls[] = {
	SOC_SINGLE_EXT(DSP_BIT_WIDTH_MIXER_CTL, SND_SOC_NOPM, 0, 0x20,
	0, msm_asm_bit_width_get, msm_asm_bit_width_put),
};

static const struct snd_kcontrol_new pri_mi2s_lb_vol_mixer_controls[] = {
	SOC_SINGLE_EXT_TLV("PRI MI2S LOOPBACK Volume", SND_SOC_NOPM, 0,
	INT_RX_VOL_GAIN, 0, msm_qti_pp_get_pri_mi2s_lb_vol_mixer,
	msm_qti_pp_set_pri_mi2s_lb_vol_mixer, afe_lb_vol_gain),
};

static const struct snd_kcontrol_new sec_mi2s_lb_vol_mixer_controls[] = {
	SOC_SINGLE_EXT_TLV("SEC MI2S LOOPBACK Volume", SND_SOC_NOPM, 0,
	INT_RX_VOL_GAIN, 0, msm_qti_pp_get_sec_mi2s_lb_vol_mixer,
	msm_qti_pp_set_sec_mi2s_lb_vol_mixer, afe_lb_vol_gain),
};

static const struct snd_kcontrol_new tert_mi2s_lb_vol_mixer_controls[] = {
	SOC_SINGLE_EXT_TLV("Tert MI2S LOOPBACK Volume", SND_SOC_NOPM, 0,
	INT_RX_VOL_GAIN, 0, msm_qti_pp_get_tert_mi2s_lb_vol_mixer,
	msm_qti_pp_set_tert_mi2s_lb_vol_mixer, afe_lb_vol_gain),
};

static const struct snd_kcontrol_new tx_cdc_dma_tx_3_lb_vol_mixer_controls[] = {
	SOC_SINGLE_EXT_TLV("TX_CDC_DMA_TX_3 LOOPBACK Volume", SND_SOC_NOPM, 0,
	INT_RX_VOL_GAIN, 0, msm_qti_pp_get_cdc_tx_3_lb_vol_mixer,
	msm_qti_pp_set_cdc_tx_3_lb_vol_mixer, afe_lb_vol_gain),
};

static const struct snd_kcontrol_new usb_audio_tx_vol_mixer_controls[] = {
	SOC_SINGLE_EXT_TLV("USB_AUDIO_TX LOOPBACK Volume", SND_SOC_NOPM, 0,
	INT_RX_VOL_GAIN, 0, msm_qti_pp_get_usb_tx_lb_vol_mixer,
	msm_qti_pp_set_usb_tx_lb_vol_mixer, afe_lb_vol_gain),
};

static const struct snd_kcontrol_new slimbus_7_lb_vol_mixer_controls[] = {
	SOC_SINGLE_EXT_TLV("SLIMBUS_7 LOOPBACK Volume", SND_SOC_NOPM, 0,
				INT_RX_VOL_GAIN, 0,
				msm_qti_pp_get_slimbus_7_lb_vol_mixer,
				msm_qti_pp_set_slimbus_7_lb_vol_mixer,
				afe_lb_vol_gain),
};

static const struct snd_kcontrol_new slimbus_8_lb_vol_mixer_controls[] = {
	SOC_SINGLE_EXT_TLV("SLIMBUS_8 LOOPBACK Volume", SND_SOC_NOPM, 0,
	INT_RX_VOL_GAIN, 0, msm_qti_pp_get_slimbus_8_lb_vol_mixer,
	msm_qti_pp_set_slimbus_8_lb_vol_mixer, afe_lb_vol_gain),
};

static const struct snd_kcontrol_new int_hfp_vol_mixer_controls[] = {
	SOC_SINGLE_EXT_TLV("Internal HFP RX Volume", SND_SOC_NOPM, 0,
	INT_RX_VOL_GAIN, 0, msm_qti_pp_get_hfp_vol_mixer,
	msm_qti_pp_set_hfp_vol_mixer, hfp_rx_vol_gain),
};

static const struct snd_kcontrol_new int_icc_vol_mixer_controls[] = {
	SOC_SINGLE_EXT_TLV("Internal ICC Volume", SND_SOC_NOPM, 0,
	INT_RX_VOL_GAIN, 0, msm_qti_pp_get_icc_vol_mixer,
	msm_qti_pp_set_icc_vol_mixer, icc_rx_vol_gain),
};

static const struct snd_kcontrol_new pri_auxpcm_lb_vol_mixer_controls[] = {
	SOC_SINGLE_EXT_TLV("PRI AUXPCM LOOPBACK Volume",
	AFE_PORT_ID_PRIMARY_PCM_TX, 0, INT_RX_VOL_GAIN, 0,
	msm_qti_pp_get_pri_auxpcm_lb_vol_mixer,
	msm_qti_pp_set_pri_auxpcm_lb_vol_mixer,
	pri_auxpcm_lb_vol_gain),
};

static const struct snd_kcontrol_new sec_auxpcm_lb_vol_mixer_controls[] = {
	SOC_SINGLE_EXT_TLV("SEC AUXPCM LOOPBACK Volume",
	AFE_PORT_ID_SECONDARY_PCM_TX, 0, INT_RX_VOL_GAIN, 0,
	msm_qti_pp_get_sec_auxpcm_lb_vol_mixer,
	msm_qti_pp_set_sec_auxpcm_lb_vol_mixer,
	sec_auxpcm_lb_vol_gain),
};

static const struct snd_kcontrol_new multi_ch_channel_map_mixer_controls[] = {
	SOC_SINGLE_MULTI_EXT("Playback Device Channel Map", SND_SOC_NOPM,
	0, PCM_MAX_CHMAP_ID, 0, PCM_FORMAT_MAX_NUM_CHANNEL_V8,
	msm_qti_pp_get_channel_map_mixer,
	msm_qti_pp_put_channel_map_mixer),
};

static const struct snd_kcontrol_new multi_ch_channel_map_capture_controls[] = {
	SOC_SINGLE_MULTI_EXT("Capture Device Channel Map", SND_SOC_NOPM,
	0, PCM_MAX_CHMAP_ID, 0, PCM_FORMAT_MAX_NUM_CHANNEL_V8,
	msm_qti_pp_get_channel_map_capture,
	msm_qti_pp_put_channel_map_capture),
};


static const struct snd_kcontrol_new get_rms_controls[] = {
	SOC_SINGLE_EXT("Get RMS", SND_SOC_NOPM, 0, 0xFFFFFFFF,
	0, msm_qti_pp_get_rms_value_control, msm_qti_pp_put_rms_value_control),
};

static const struct snd_kcontrol_new eq_enable_mixer_controls[] = {
	SOC_SINGLE_EXT("MultiMedia1 EQ Enable", SND_SOC_NOPM,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 1, 0, msm_qti_pp_get_eq_enable_mixer,
	msm_qti_pp_put_eq_enable_mixer),
	SOC_SINGLE_EXT("MultiMedia2 EQ Enable", SND_SOC_NOPM,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 1, 0, msm_qti_pp_get_eq_enable_mixer,
	msm_qti_pp_put_eq_enable_mixer),
	SOC_SINGLE_EXT("MultiMedia3 EQ Enable", SND_SOC_NOPM,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 1, 0, msm_qti_pp_get_eq_enable_mixer,
	msm_qti_pp_put_eq_enable_mixer),
};

static const struct snd_kcontrol_new eq_band_mixer_controls[] = {
	SOC_SINGLE_EXT("MultiMedia1 EQ Band Count", SND_SOC_NOPM,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 11, 0,
	msm_qti_pp_get_eq_band_count_audio_mixer,
	msm_qti_pp_put_eq_band_count_audio_mixer),
	SOC_SINGLE_EXT("MultiMedia2 EQ Band Count", SND_SOC_NOPM,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 11, 0,
	msm_qti_pp_get_eq_band_count_audio_mixer,
	msm_qti_pp_put_eq_band_count_audio_mixer),
	SOC_SINGLE_EXT("MultiMedia3 EQ Band Count", SND_SOC_NOPM,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 11, 0,
	msm_qti_pp_get_eq_band_count_audio_mixer,
	msm_qti_pp_put_eq_band_count_audio_mixer),
};

static const struct snd_kcontrol_new eq_coeff_mixer_controls[] = {
	SOC_SINGLE_MULTI_EXT("MultiMedia1 EQ Band1", EQ_BAND1,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia1 EQ Band2", EQ_BAND2,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia1 EQ Band3", EQ_BAND3,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia1 EQ Band4", EQ_BAND4,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia1 EQ Band5", EQ_BAND5,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia1 EQ Band6", EQ_BAND6,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia1 EQ Band7", EQ_BAND7,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia1 EQ Band8", EQ_BAND8,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia1 EQ Band9", EQ_BAND9,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia1 EQ Band10", EQ_BAND10,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia1 EQ Band11", EQ_BAND11,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia1 EQ Band12", EQ_BAND12,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia2 EQ Band1", EQ_BAND1,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia2 EQ Band2", EQ_BAND2,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia2 EQ Band3", EQ_BAND3,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia2 EQ Band4", EQ_BAND4,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia2 EQ Band5", EQ_BAND5,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia2 EQ Band6", EQ_BAND6,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia2 EQ Band7", EQ_BAND7,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia2 EQ Band8", EQ_BAND8,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia2 EQ Band9", EQ_BAND9,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia2 EQ Band10", EQ_BAND10,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia2 EQ Band11", EQ_BAND11,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia2 EQ Band12", EQ_BAND12,
	MSM_FRONTEND_DAI_MULTIMEDIA2, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia3 EQ Band1", EQ_BAND1,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia3 EQ Band2", EQ_BAND2,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia3 EQ Band3", EQ_BAND3,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia3 EQ Band4", EQ_BAND4,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia3 EQ Band5", EQ_BAND5,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia3 EQ Band6", EQ_BAND6,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia3 EQ Band7", EQ_BAND7,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia3 EQ Band8", EQ_BAND8,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia3 EQ Band9", EQ_BAND9,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia3 EQ Band10", EQ_BAND10,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia3 EQ Band11", EQ_BAND11,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("MultiMedia3 EQ Band12", EQ_BAND12,
	MSM_FRONTEND_DAI_MULTIMEDIA3, 255, 0, 5,
	msm_qti_pp_get_eq_band_audio_mixer, msm_qti_pp_put_eq_band_audio_mixer),
};

static const struct snd_kcontrol_new asphere_mixer_controls[] = {
	SOC_SINGLE_MULTI_EXT("MSM ASphere Set Param", SND_SOC_NOPM, 0,
	0xFFFFFFFF, 0, 2, msm_qti_pp_asphere_get, msm_qti_pp_asphere_set),
};

static const struct snd_kcontrol_new dtmf_detect_enable_mixer_controls[] = {
	SOC_SINGLE_EXT("MultiMedia1 DTMF Detect Enable", SND_SOC_NOPM,
	MSM_FRONTEND_DAI_MULTIMEDIA1, 1, 0, NULL,
	msm_qti_pp_put_dtmf_module_enable),
	SOC_SINGLE_EXT("MultiMedia6 DTMF Detect Enable", SND_SOC_NOPM,
	MSM_FRONTEND_DAI_MULTIMEDIA6, 1, 0, NULL,
	msm_qti_pp_put_dtmf_module_enable),
	SOC_SINGLE_EXT("MultiMedia21 DTMF Detect Enable", SND_SOC_NOPM,
	MSM_FRONTEND_DAI_MULTIMEDIA21, 1, 0, NULL,
	msm_qti_pp_put_dtmf_module_enable),
};

#ifdef CONFIG_QTI_PP
void msm_qti_pp_add_controls(struct snd_soc_component *component)
{
	snd_soc_add_component_controls(component, int_dp_vol_mixer_controls,
			ARRAY_SIZE(int_dp_vol_mixer_controls));

	snd_soc_add_component_controls(component, int_fm_vol_mixer_controls,
			ARRAY_SIZE(int_fm_vol_mixer_controls));

	snd_soc_add_component_controls(component,
			pri_mi2s_lb_vol_mixer_controls,
			ARRAY_SIZE(pri_mi2s_lb_vol_mixer_controls));

	snd_soc_add_component_controls(component,
			sec_mi2s_lb_vol_mixer_controls,
			ARRAY_SIZE(sec_mi2s_lb_vol_mixer_controls));

	snd_soc_add_component_controls(component,
			tert_mi2s_lb_vol_mixer_controls,
			ARRAY_SIZE(tert_mi2s_lb_vol_mixer_controls));

	snd_soc_add_component_controls(component,
			tx_cdc_dma_tx_3_lb_vol_mixer_controls,
			ARRAY_SIZE(tx_cdc_dma_tx_3_lb_vol_mixer_controls));

	snd_soc_add_component_controls(component,
			usb_audio_tx_vol_mixer_controls,
			ARRAY_SIZE(usb_audio_tx_vol_mixer_controls));

	snd_soc_add_component_controls(component,
			slimbus_7_lb_vol_mixer_controls,
			ARRAY_SIZE(slimbus_7_lb_vol_mixer_controls));

	snd_soc_add_component_controls(component,
			slimbus_8_lb_vol_mixer_controls,
			ARRAY_SIZE(slimbus_8_lb_vol_mixer_controls));

	snd_soc_add_component_controls(component, int_hfp_vol_mixer_controls,
			ARRAY_SIZE(int_hfp_vol_mixer_controls));

	snd_soc_add_component_controls(component, int_icc_vol_mixer_controls,
			ARRAY_SIZE(int_icc_vol_mixer_controls));

	snd_soc_add_component_controls(component,
			pri_auxpcm_lb_vol_mixer_controls,
			ARRAY_SIZE(pri_auxpcm_lb_vol_mixer_controls));

	snd_soc_add_component_controls(component,
				sec_auxpcm_lb_vol_mixer_controls,
			ARRAY_SIZE(sec_auxpcm_lb_vol_mixer_controls));

	snd_soc_add_component_controls(component,
				multi_ch_channel_map_mixer_controls,
			ARRAY_SIZE(multi_ch_channel_map_mixer_controls));

	snd_soc_add_component_controls(component, get_rms_controls,
			ARRAY_SIZE(get_rms_controls));

	snd_soc_add_component_controls(component,
				multi_ch_channel_map_capture_controls,
			ARRAY_SIZE(multi_ch_channel_map_capture_controls));


	snd_soc_add_component_controls(component, eq_enable_mixer_controls,
			ARRAY_SIZE(eq_enable_mixer_controls));

	snd_soc_add_component_controls(component, eq_band_mixer_controls,
			ARRAY_SIZE(eq_band_mixer_controls));

	snd_soc_add_component_controls(component, eq_coeff_mixer_controls,
			ARRAY_SIZE(eq_coeff_mixer_controls));

	snd_soc_add_component_controls(component, asphere_mixer_controls,
			ARRAY_SIZE(asphere_mixer_controls));

	snd_soc_add_component_controls(component, msm_multichannel_ec_controls,
			ARRAY_SIZE(msm_multichannel_ec_controls));

	snd_soc_add_component_controls(component, dsp_bit_width_controls,
			ARRAY_SIZE(dsp_bit_width_controls));

	snd_soc_add_component_controls(component, ec_ffecns_controls,
			ARRAY_SIZE(ec_ffecns_controls));

	snd_soc_add_component_controls(component,
				dtmf_detect_enable_mixer_controls,
			ARRAY_SIZE(dtmf_detect_enable_mixer_controls));
}
#endif /* CONFIG_QTI_PP */
