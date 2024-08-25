
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/platform_data/s1v3s3xx.h>
#include <linux/input.h>

#ifdef CONFIG_MACH_MX6SL_NTX //[
#include <mach/iomux-mx6sl.h>
#endif //] CONFIG_MACH_MX6SL_NTX

#define GDEBUG 1
#include <linux/gallen_dbg.h>

#define S1V3S3XX_MSGRDY_INT		1

#define S1V3S3XX_AUDIOGAIN_MAX (-48)
#define S1V3S3XX_AUDIOGAIN_MIN (18)

#include "../../../arch/arm/mach-mx6/ntx_hwconfig.h"
#include "../../../arch/arm/mach-mx6/ntx_firmware.h"


struct s1v3s3xx_seqplay_file_event {
	int iDelay_ms;
	int iFile_no;
};

#define REQ_RET_SUCCESS						0
#define REQ_RET_ERR_NOMEM					-1
#define REQ_RET_ERR_UNKOWN_REQ		-2
#define REQ_RET_ERR_SENDMSG				-3
#define REQ_RET_ERR_RECVMSG				-4
#define REQ_RET_ERR_RECVMSGID			-5
#define REQ_RET_ERR_RECVMSGTIMEOUT	-6

#define FLASH_ERASE_TYPE_SECTOR 0
#define FLASH_ERASE_TYPE_CHIP 1


// sequencer play status 
#define SPLAY_STATUS_INIT		1
#define SPLAY_STATUS_INIT_AMP		2
#define SPLAY_STATUS_INIT_VOL		3
#define SPLAY_STATUS_PLAYING		10
#define SPLAY_STATUS_PAUSE			11
#define SPLAY_STATUS_STOP				0


#define SPLAY_FILES_TOTAL		64
struct s1v3s3xx_driver_data {
	// platform 
	struct spi_device *spi_dev;
	struct s1v3s3xx_platform_data *pdata;

#ifdef CONFIG_MACH_MX6SL_NTX //[
	iomux_v3_cfg_t tPadctrl_CS,tPadctrl_CS_Old;
	iomux_v3_cfg_t tPadctrl_RST,tPadctrl_RST_Old;
	iomux_v3_cfg_t tPadctrl_INT,tPadctrl_INT_Old;
	iomux_v3_cfg_t tPadctrl_STB,tPadctrl_STB_Old;
	iomux_v3_cfg_t tPadctrl_AMP,tPadctrl_AMP_Old;
	iomux_v3_cfg_t tPadctrl_BOOST,tPadctrl_BOOST_Old;
#endif //]CONFIG_MACH_MX6SL_NTX

	// platform hardware .
	unsigned gpio_TTS_INT;
	unsigned gpio_TTS_RST;
	unsigned gpio_TTS_STB;
	unsigned gpio_TTS_CS;
	int irq_TTS_INT;
	int iIsISR_ready;

	unsigned gpio_AMP;
	unsigned gpio_BOOST;
	int iDelay_ms_AMP_to_BOOST;
	// 
	unsigned char hw_id_int,hw_id_frac;
	unsigned char fw_version_int,fw_version_frac;
	unsigned long fw_feature;

	// test request . 
	unsigned short wCheckSumEnable;
	unsigned short wMsgReadyEnable;
	unsigned long dwKeyCode;

	// parameters for audio config . 
	int iAudioGainDB;
	unsigned long dwAudioSampleRateHz;
	int iIsMute;// AUDIO_MUTE_REQ parameter .


	unsigned short wLastSendIscID;
	unsigned short wLastRecvIscID;
	int iLastReqError ; //>=0 is success , <0 is fail . 
	unsigned short wErrorCode;

	// parameters for sequencer play .
	int iSPlayCount;
	int iSPlayFiles;
	struct s1v3s3xx_seqplay_file_event SPlayEvtA[SPLAY_FILES_TOTAL];
	int iIsPause;
	int iIsEnableStatusIND;


	// 
	int iSPlayStatus; // sequencer play status . 
	int iIsInRequestProc; 


	int iFlashEraseType; // 1:chip , 0:sector .

	unsigned short wMsgDataBytes;
	unsigned char *pbMsgData; // extra sending data . 

};

static struct s1v3s3xx_driver_data *gs1v3s3xx_ddata;
extern void ntx_report_key(int isDown,__u16 wKeyCode);
extern void ntx_report_event(unsigned int type, unsigned int code, int value);
static irqreturn_t s1v3s3xx_irq_handler(int irq, void *dev);


static const char *strerror(int err)
{
#define ERRNOSTR(_e) case _e: return # _e
	switch (err) {
	case 0: return "OK";
	ERRNOSTR(ENOMEM);
	ERRNOSTR(ENODEV);
	ERRNOSTR(ENXIO);
	ERRNOSTR(EINVAL);
	ERRNOSTR(EAGAIN);
	ERRNOSTR(EFBIG);
	ERRNOSTR(EPIPE);
	ERRNOSTR(EMSGSIZE);
	ERRNOSTR(ENOSPC);
	ERRNOSTR(EINPROGRESS);
	ERRNOSTR(ENOSR);
	ERRNOSTR(EOVERFLOW);
	ERRNOSTR(EPROTO);
	ERRNOSTR(EILSEQ);
	ERRNOSTR(ETIMEDOUT);
	ERRNOSTR(EOPNOTSUPP);
	ERRNOSTR(EPFNOSUPPORT);
	ERRNOSTR(EAFNOSUPPORT);
	ERRNOSTR(EADDRINUSE);
	ERRNOSTR(EADDRNOTAVAIL);
	ERRNOSTR(ENOBUFS);
	ERRNOSTR(EISCONN);
	ERRNOSTR(ENOTCONN);
	ERRNOSTR(ESHUTDOWN);
	ERRNOSTR(ENOENT);
	ERRNOSTR(ECONNRESET);
	ERRNOSTR(ETIME);
	ERRNOSTR(ECOMM);
	ERRNOSTR(EREMOTEIO);
	ERRNOSTR(EXDEV);
	ERRNOSTR(EPERM);
	default: return "unknown";
	}

#undef ERRNOSTR
}



#include "s1v3s3xx_isc_msgs.c"
#include "s1v3s3xx_spi_api.c"




int RespMessageDecode(struct s1v3s3xx_driver_data *s1v3s3xx_ddata)
{
	int iReceivedMsgLen = (int)GetMessageReceivedLen();
	int iRet = 0;

	unsigned short wMsgId;
	unsigned short wMsgLen;


	if(iReceivedMsgLen<=0) {
		DBG_MSG("%s(): iReceivedMsgLen=%d\n",__func__,iReceivedMsgLen);
		return -1; 
	}

	if(iReceivedMsgLen>MAX_RECEIVED_DATA_LEN) {
		ERR_MSG("%s() : [warning] received msg len=%d > buffer size !!\n",
			__func__,iReceivedMsgLen);
		return -2;
	}

	wMsgLen = aucReceivedData[1];
	wMsgLen  = (wMsgId << 8) | aucReceivedData[0];

	wMsgId = aucReceivedData[3];
	wMsgId  = (wMsgId << 8) | aucReceivedData[2];

	DBG_MSG("%s(): MsgRecvLen=%d,MsgId=0x%04x,MsgLen=0x%04x\n",__func__,iReceivedMsgLen,wMsgId,wMsgLen);

	if(iReceivedMsgLen<(int)wMsgLen) {
		ERR_MSG("%s() : [warning] received msg len=%d < specified len(%d) !!\n",
			__func__,iReceivedMsgLen,(int)wMsgLen);
	}

	switch(wMsgId) {
	case ID_ISC_VERSION_RESP:
		s1v3s3xx_ddata->hw_id_int = aucReceivedData[4];
		s1v3s3xx_ddata->hw_id_frac = aucReceivedData[5];
		s1v3s3xx_ddata->fw_version_int = aucReceivedData[6];
		s1v3s3xx_ddata->fw_version_frac = aucReceivedData[7];
		s1v3s3xx_ddata->fw_feature = aucReceivedData[11]<<24|aucReceivedData[10]<<16|aucReceivedData[9]<<8|aucReceivedData[8];
		break;
	case ID_ISC_ERROR_IND://6
	case ID_ISC_PMAN_STANDBY_ENTRY_RESP://6
	case ID_ISC_AUDIO_CONFIG_RESP:
	case ID_ISC_AUDIO_VOLUME_RESP://6
	case ID_ISC_AUDIO_MUTE_RESP://6
	case ID_ISC_SEQUENCER_CONFIG_RESP:
	case ID_ISC_SEQUENCER_START_RESP:
	case ID_ISC_SEQUENCER_PAUSE_RESP:
	case ID_ISC_SEQUENCER_STOP_RESP://6
	case ID_ISC_SEQUENCER_ERROR_IND://6
	case ID_ISC_TEST_RESP:
		s1v3s3xx_ddata->wErrorCode = usMessageErrorCode;//aucReceivedData[5]<<8|aucReceivedData[4];
		DBG_MSG("resp_id=0x%04x,ErrorCode=0x%04x\n",wMsgId,s1v3s3xx_ddata->wErrorCode);
		break;
	case ID_ISC_MSG_BLOCKED_RESP:
		s1v3s3xx_ddata->wErrorCode = usMessageErrorCode;//aucReceivedData[5]<<8|aucReceivedData[4];
		DBG_MSG("resp_id=0x%04x,ErrorCode=0x%04x\n",wMsgId,s1v3s3xx_ddata->wErrorCode);
		DBG_MSG("BlockedMessageID=0x%04x\n",usBlockedMessageID);
		break;
	case ID_ISC_SEQUENCER_STATUS_IND://6
		s1v3s3xx_ddata->wErrorCode = usSequenceStatus;//aucReceivedData[5]<<8|aucReceivedData[4];
		DBG_MSG("resp_id=0x%04x,SeqStatusCode=0x%04x\n",wMsgId,s1v3s3xx_ddata->wErrorCode);
		break;
	default :
		break;
	}
	s1v3s3xx_ddata->wLastRecvIscID = wMsgId;

	if(0==usMessageErrorCode) {
		switch(wMsgId) {
		case ID_ISC_SEQUENCER_CONFIG_RESP:
			s1v3s3xx_ddata->iSPlayStatus = SPLAY_STATUS_INIT;
			break;
		case ID_ISC_SEQUENCER_START_RESP:
			s1v3s3xx_ddata->iSPlayStatus = SPLAY_STATUS_PLAYING;
			if(gpio_is_valid(s1v3s3xx_ddata->gpio_BOOST)) {
				gpio_set_value(s1v3s3xx_ddata->gpio_BOOST,1);
				msleep(s1v3s3xx_ddata->iDelay_ms_AMP_to_BOOST);
			}
			if(gpio_is_valid(s1v3s3xx_ddata->gpio_AMP)) {
				gpio_set_value(s1v3s3xx_ddata->gpio_AMP,1);
			}
			ntx_report_event(EV_MSC,MSC_RAW,MSC_RAW_PLAY_START);
			break;
		case ID_ISC_SEQUENCER_PAUSE_RESP:
			s1v3s3xx_ddata->iSPlayStatus = SPLAY_STATUS_PAUSE;
			ntx_report_event(EV_MSC,MSC_RAW,MSC_RAW_PLAY_STOP);
			break;
		case ID_ISC_SEQUENCER_STATUS_IND:
			if(0xffff==usSequenceStatus) {
				s1v3s3xx_ddata->iSPlayStatus = SPLAY_STATUS_STOP;
				if(gpio_is_valid(s1v3s3xx_ddata->gpio_AMP)) {
					gpio_set_value(s1v3s3xx_ddata->gpio_AMP,0);
					msleep(s1v3s3xx_ddata->iDelay_ms_AMP_to_BOOST);
				}
				if(gpio_is_valid(s1v3s3xx_ddata->gpio_BOOST)) {
					gpio_set_value(s1v3s3xx_ddata->gpio_BOOST,0);
				}
				ntx_report_event(EV_MSC,MSC_RAW,MSC_RAW_PLAY_STOP);
			}
			break;
		case ID_ISC_SEQUENCER_STOP_RESP:
			s1v3s3xx_ddata->iSPlayStatus = SPLAY_STATUS_STOP;
			break;
		default:
			break;
		}
	}
	else {
		ERR_MSG("Got error code=0x%04x\n",usMessageErrorCode);
	}

	return iRet;
}


#define INVALID_WVAL	0xffff
#define INVALID_DWVAL	0xffffffff
int s1v3s3xx_request(struct s1v3s3xx_driver_data *s1v3s3xx_ddata,unsigned short wRequestID)
{
	int iRet = 0;
	int iError;
	unsigned char usReceivedMessageID;
	unsigned short wResponseID = 0;
	unsigned char *pbRequestMsg = 0;
	int iHasResponse = 1;
	unsigned short wTemp;
	int i;
	unsigned char *pb;

	DBG_MSG("%s():REQ=0x%04x\n",__func__,wRequestID);
	s1v3s3xx_ddata->iIsInRequestProc = 1;


	if(s1v3s3xx_ddata->pbMsgData) {
		kfree(s1v3s3xx_ddata->pbMsgData);
		s1v3s3xx_ddata->pbMsgData=0;
		s1v3s3xx_ddata->wMsgDataBytes=0;
	}
	s1v3s3xx_ddata->iLastReqError = REQ_RET_SUCCESS;
	s1v3s3xx_ddata->wLastSendIscID = wRequestID;
	s1v3s3xx_ddata->wLastRecvIscID = ID_ISC_INVALID;

	switch (wRequestID) {
	case ID_ISC_VERSION_REQ:
		wResponseID = ID_ISC_VERSION_RESP;
		pbRequestMsg = aucIscVersionReq;
		break;
	case ID_ISC_RESET_REQ:
		wResponseID = ID_ISC_RESET_RESP;
		pbRequestMsg = aucIscResetReq;
		break;
	case ID_ISC_PMAN_STANDBY_ENTRY_REQ:
		wResponseID = ID_ISC_PMAN_STANDBY_ENTRY_RESP;
		pbRequestMsg = aucIscPmanStandbyEntryReq;
		break;
	case ID_ISC_FLASHACCESS_MODE_IND:
		wResponseID = ID_ISC_INVALID;
		pbRequestMsg = aucIscFlashAccessModeReq;
		iHasResponse = 0;
		break;

	case ID_ISC_INVALID:
	case ID_ISC_MSG_BLOCKED_RESP:
	case ID_ISC_PMAN_STANDBY_EXIT_IND:
	case ID_ISC_ERROR_IND:
	case ID_ISC_AUDIO_PAUSE_IND:
	case ID_ISC_SEQUENCER_ERROR_IND:
	case ID_ISC_SEQUENCER_STATUS_IND:
		wResponseID = ID_ISC_INVALID;
		pbRequestMsg = 0;
		break;
	case ID_ISC_TEST_REQ:
		{
			wResponseID = ID_ISC_TEST_RESP;
			pbRequestMsg = aucIscTestReq;

			if(INVALID_WVAL!=s1v3s3xx_ddata->wCheckSumEnable) {
				if(s1v3s3xx_ddata->wCheckSumEnable) {
					aucIscTestReq[6]=1;
				}
				else {
					aucIscTestReq[6]=0;
				}
				aucIscTestReq[7]=0;
			}
			if(INVALID_WVAL!=s1v3s3xx_ddata->wMsgReadyEnable) {
				if(s1v3s3xx_ddata->wMsgReadyEnable) {
					aucIscTestReq[8]=1;
				}
				else {
					aucIscTestReq[8]=0;
				}
				aucIscTestReq[9]=0;
			}
			if(INVALID_DWVAL!=s1v3s3xx_ddata->dwKeyCode) {
				aucIscTestReq[10]=s1v3s3xx_ddata->dwKeyCode&0xff;
				aucIscTestReq[11]=s1v3s3xx_ddata->dwKeyCode>>8&0xff;
				aucIscTestReq[12]=s1v3s3xx_ddata->dwKeyCode>>16&0xff;
				aucIscTestReq[13]=s1v3s3xx_ddata->dwKeyCode>>24&0xff;
				DBG_MSG("keycode=0x%08x{0x%02x,0x%02x,0x%02x,0x%02x}\n",
						s1v3s3xx_ddata->dwKeyCode,aucIscTestReq[10],aucIscTestReq[11],
						aucIscTestReq[12],aucIscTestReq[13]);
			}
		}
		break;

	case ID_ISC_AUDIO_CONFIG_REQ:
		wResponseID = ID_ISC_AUDIO_CONFIG_RESP;
		pbRequestMsg = aucIscAudioConfigReq;

		if( (s1v3s3xx_ddata->iAudioGainDB<S1V3S3XX_AUDIOGAIN_MAX) ||
			 	(s1v3s3xx_ddata->iAudioGainDB>S1V3S3XX_AUDIOGAIN_MIN) ) 
		{
			aucIscAudioConfigReq[7] = 0; // mute ;
		}
		else {
			aucIscAudioConfigReq[7] = (unsigned char)(s1v3s3xx_ddata->iAudioGainDB+49); // audio gain;
		}

		if(16000==s1v3s3xx_ddata->dwAudioSampleRateHz) {
			aucIscAudioConfigReq[9] = 0x03; // audio sample rain 16KHz;
		}
		else {
			aucIscAudioConfigReq[9] = 0x09; // don't care ;
		}

		DBG_MSG("audio gain=0x%02x,sample rate=0x%02x\n",
				aucIscAudioConfigReq[7],aucIscAudioConfigReq[9]);

		break;

	case ID_ISC_AUDIO_VOLUME_REQ:
		wResponseID = ID_ISC_AUDIO_VOLUME_RESP;
		pbRequestMsg = aucIscAudioVolumeReq;
		if( (s1v3s3xx_ddata->iAudioGainDB<S1V3S3XX_AUDIOGAIN_MAX) ||
			 	(s1v3s3xx_ddata->iAudioGainDB>S1V3S3XX_AUDIOGAIN_MIN) )
		{	
			aucIscAudioVolumeReq[6] = 0;
		}
		else {
			aucIscAudioVolumeReq[6] = (unsigned char)(s1v3s3xx_ddata->iAudioGainDB+49);
		}
		aucIscAudioVolumeReq[7] = 0;
		DBG_MSG("audio gain=0x%02x\n",aucIscAudioVolumeReq[6]);
		break;

	case ID_ISC_AUDIO_MUTE_REQ:
		wResponseID = ID_ISC_AUDIO_MUTE_RESP;
		pbRequestMsg = aucIscAudioMuteReq;
		if(s1v3s3xx_ddata->iIsMute) {
			aucIscAudioMuteReq[6] = 1;
		}
		else {
			aucIscAudioMuteReq[6] = 0;
		}
		aucIscAudioMuteReq[7] = 0;
		DBG_MSG("audio mute=0x%02x\n",aucIscAudioMuteReq[6]);
		break;

	case ID_ISC_SEQUENCER_START_REQ:
		wResponseID = ID_ISC_SEQUENCER_START_RESP;
		pbRequestMsg = aucIscSequencerStartReq;
		if(s1v3s3xx_ddata->iIsEnableStatusIND) {
			aucIscSequencerStartReq[6] = 1;
		}
		else {
			aucIscSequencerStartReq[6] = 0;
		}
		aucIscSequencerStartReq[7] = 0;
		DBG_MSG("EnableStatusIND=0x%02x\n",aucIscSequencerStartReq[6]);
		break;

	case ID_ISC_SEQUENCER_PAUSE_REQ:
		wResponseID = ID_ISC_SEQUENCER_PAUSE_RESP;
		pbRequestMsg = aucIscSequencerPauseReq;
		if(s1v3s3xx_ddata->iIsPause) {
			aucIscSequencerPauseReq[6] = 1;
		}
		else {
			aucIscSequencerPauseReq[6] = 0;
		}
		aucIscSequencerPauseReq[7] = 0;
		DBG_MSG("Pause=0x%02x\n",aucIscSequencerPauseReq[6]);
		break;

	case ID_ISC_SEQUENCER_STOP_REQ:
		wResponseID = ID_ISC_SEQUENCER_STOP_RESP;
		pbRequestMsg = aucIscSequencerStopReq;
		break;

	case ID_ISC_SEQUENCER_CONFIG_REQ:
		wResponseID = ID_ISC_SEQUENCER_CONFIG_RESP;

		wTemp = (unsigned short)(LEN_HEAD_ISC_SEQUENCER_CONFIG_REQ+(s1v3s3xx_ddata->iSPlayFiles*8));
		aucIscSequencerConfigReq[2] = wTemp & 0xff;
		aucIscSequencerConfigReq[3] = (wTemp>>8) & 0xff;

		wTemp = (unsigned short)s1v3s3xx_ddata->iSPlayCount;
		aucIscSequencerConfigReq[6] = wTemp & 0xff;
		aucIscSequencerConfigReq[7] = (wTemp>>8) & 0xff;

		wTemp = (unsigned short)s1v3s3xx_ddata->iSPlayFiles;
		aucIscSequencerConfigReq[8] = wTemp & 0xff;
		aucIscSequencerConfigReq[9] = (wTemp>>8) & 0xff;

		DBG_MSG("msglen={0x%02x,0x%02x},playcount={0x%02x,0x%02x},files={0x%02x,0x%02x}\n",
				aucIscSequencerConfigReq[2],aucIscSequencerConfigReq[3],
				aucIscSequencerConfigReq[6],aucIscSequencerConfigReq[7],
				aucIscSequencerConfigReq[8],aucIscSequencerConfigReq[9]);
		ASSERT(!s1v3s3xx_ddata->pbMsgData);// 

		s1v3s3xx_ddata->wMsgDataBytes = s1v3s3xx_ddata->iSPlayFiles*8;
		s1v3s3xx_ddata->pbMsgData = kmalloc(HEADER_LEN+LEN_HEAD_ISC_SEQUENCER_CONFIG_REQ+s1v3s3xx_ddata->wMsgDataBytes,GFP_KERNEL);

		if(s1v3s3xx_ddata->pbMsgData) {
			struct s1v3s3xx_seqplay_file_event *ptSPlayFEVT=s1v3s3xx_ddata->SPlayEvtA;

			pbRequestMsg = s1v3s3xx_ddata->pbMsgData;

			memcpy(s1v3s3xx_ddata->pbMsgData,aucIscSequencerConfigReq,HEADER_LEN+LEN_HEAD_ISC_SEQUENCER_CONFIG_REQ);
			pb = s1v3s3xx_ddata->pbMsgData+HEADER_LEN+LEN_HEAD_ISC_SEQUENCER_CONFIG_REQ;
			for(i=0;i<s1v3s3xx_ddata->iSPlayFiles;i++) {
				ptSPlayFEVT = &s1v3s3xx_ddata->SPlayEvtA[i];

				// delay ms . 
				wTemp = (unsigned short)ptSPlayFEVT->iDelay_ms;
				pb[0] = wTemp & 0xff;
				pb[1] = (wTemp>>8) & 0xff;

				// descramble .
				pb[2] = 0;
				pb[3] = 0;

				// file type EOV
				pb[4] = 3;
				pb[5] = 0;

				// file no . 
				wTemp = (unsigned short)ptSPlayFEVT->iFile_no;
				pb[6] = wTemp & 0xff;
				pb[7] = (wTemp>>8) & 0xff;
				DBG_MSG("sp f[%d] msg={0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,}\n",
						i,pb[0],pb[1],pb[2],pb[3],pb[4],pb[5],pb[6],pb[7]);

				pb+=8;
			}

		}
		else {
			ERR_MSG("%s() memory not enough !\n",__func__);
			s1v3s3xx_ddata->iLastReqError = REQ_RET_ERR_NOMEM;
			iRet = -1;
			goto exit;
		}

		break;

	default :
		ERR_MSG("%s() unsupported request id(0x%04x)!\n",__func__,wRequestID);
		s1v3s3xx_ddata->iLastReqError = REQ_RET_ERR_UNKOWN_REQ;
		iRet = -1;
		goto exit;
	}

	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);
	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);// dummy times for > 100 ns . 

	if(pbRequestMsg) {
		iError = SPI_SendMessage(pbRequestMsg, &usReceivedMessageID);
		if(iError < SPIERR_SUCCESS) {
			ERR_MSG("%s() SendMessage failed (%d)!\n",__func__,iError);
			iRet = -2;
			s1v3s3xx_ddata->iLastReqError = REQ_RET_ERR_SENDMSG;
			goto exit;
		}
		//DBG_MSG("RcvMsgId=0x%04x\n",usReceivedMessageID); // always get 0xffff .
	}

	if(iHasResponse) {
		iError = SPI_ReceiveMessage(&usReceivedMessageID);
		if (iError < SPIERR_SUCCESS ) {
			ERR_MSG("%s() ReceiveMessage failed (%d)!\n",__func__,iError);
			s1v3s3xx_ddata->iLastReqError = REQ_RET_ERR_RECVMSG;
			iRet = -3;
			goto exit;
		}
		else if ( SPIERR_TIMEOUT==iError ) {
			s1v3s3xx_ddata->iLastReqError = REQ_RET_ERR_RECVMSGTIMEOUT;
			iRet = -4;
			goto exit;
		}

		if(ID_ISC_INVALID==wResponseID) {
		}
		else if (usReceivedMessageID != wResponseID) {
			DumpReceivedMessages();
			s1v3s3xx_ddata->iLastReqError = REQ_RET_ERR_RECVMSGID;
			iRet = -5;
			goto exit;
		}
		//DBG_MSG("gusReceivedDataLen=%d\n",(int)gusReceivedDataLen);

		RespMessageDecode(s1v3s3xx_ddata);
	}

exit:
	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,1);
	s1v3s3xx_ddata->iIsInRequestProc = 0;
	return iRet;
}

static void s1v3s3xx_hw_reset(struct s1v3s3xx_driver_data *s1v3s3xx_ddata)
{
	DBG_MSG("%s():hardware reset \n",__func__);
	gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_RST,0);
	msleep(10);
	gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_RST,1);
	msleep(10);
	s1v3s3xx_ddata->iSPlayStatus = SPLAY_STATUS_STOP;
}


#if 1 //[
static inline int s1v3s3xx_spi_rw(struct s1v3s3xx_driver_data *s1v3s3xx_ddata,const void *tbuf,void *rbuf, size_t len)
{
	struct spi_transfer	t = {
			.len		= len,
			// .tx_buf = 0,
			// .rx_buf = 0,
			// .speed_hz = 2000000 ,
		};
	struct spi_message	m;

	if(tbuf) {
		t.tx_buf = tbuf;
	}

	if(rbuf) {
		t.rx_buf = rbuf;
	}

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return spi_sync(s1v3s3xx_ddata->spi_dev, &m);
	
}
static inline int
_spi_read(struct s1v3s3xx_driver_data *s1v3s3xx_ddata, void *buf, size_t len)
{
	return s1v3s3xx_spi_rw(s1v3s3xx_ddata,0,buf,len);
}
static inline int
_spi_write(struct s1v3s3xx_driver_data *s1v3s3xx_ddata, const void *buf, size_t len)
{
	return s1v3s3xx_spi_rw(s1v3s3xx_ddata,buf,0,len);
}
#else //][!
static inline int
_spi_read(struct s1v3s3xx_driver_data *s1v3s3xx_ddata, void *buf, size_t len)
{
	return spi_read(s1v3s3xx_ddata->spi_dev,buf,len);
}

static inline int
_spi_write(struct s1v3s3xx_driver_data *s1v3s3xx_ddata, const void *buf, size_t len)
{
	return spi_write(s1v3s3xx_ddata->spi_dev,buf,len);
}
#endif //]

static int s1v3s3xx_flash_get_ids(struct s1v3s3xx_driver_data *s1v3s3xx_ddata,unsigned char *O_pbMID,unsigned short *O_pwDID) 
{
	unsigned char bCmd;
	unsigned char bIDA[3];
	int iRet = 0;

	bCmd = 0x9f; // RDID . 
	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);
	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);// dummy times for > 100 ns . 

	_spi_write(s1v3s3xx_ddata,&bCmd,1);
	_spi_read(s1v3s3xx_ddata,bIDA,3);
	
	//printk("Manufacturer ID = 0x%02x\n",bIDA[0]);
	//printk("Device ID = 0x%02x%02x\n",bIDA[2],bIDA[1]);

	if(O_pbMID) {
		*O_pbMID = bIDA[0];
	}
	if(O_pwDID) {
		*O_pwDID = bIDA[2]<<8|bIDA[1];
	}

	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,1);
	return iRet;
}


static int s1v3s3xx_flash_wait_ready(struct s1v3s3xx_driver_data *s1v3s3xx_ddata,const char *pszTitle,unsigned long dwTimeout_ms)
{
	unsigned char bCmd=0x05;
	unsigned char bSR;
	int iTryCnt = 0;
	int iRet = 0;
	unsigned long ulStartTick = jiffies,ulTickNow;
	unsigned long ulTimeoutTick = ulStartTick+msecs_to_jiffies(dwTimeout_ms);

	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);
	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);// dummy times for > 100 ns . 


	//DBG_MSG("%s : wait %s ...",__func__,pszTitle);
	_spi_write(s1v3s3xx_ddata,&bCmd,1);

	do {
		ulTickNow=jiffies;
		if( time_after(ulTickNow,ulTimeoutTick) ) {
			WARNING_MSG("%s : wait %s %dms timeout, %d tried !!\n",
					__func__,pszTitle,(int)dwTimeout_ms,iTryCnt);
			iRet = -1;
			break;
		}
		udelay(100);
		++iTryCnt;
		_spi_read(s1v3s3xx_ddata,&bSR,1);
	} while (bSR&0x01);

	if( !time_after(ulTickNow,ulTimeoutTick) ) {
		//DBG_MSG("[ready %d]\n",iTryCnt);
		iRet = iTryCnt;
	}
	
	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,1);

	return iRet;
}

static int s1v3s3xx_flash_write_enable(struct s1v3s3xx_driver_data *s1v3s3xx_ddata)
{
	unsigned char bFlashWrEnCmd = 0x06;

	if(s1v3s3xx_flash_wait_ready(s1v3s3xx_ddata,"before flash write enabling",10000)<0) {
		return -1;
	}

	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);
	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);
	_spi_write(s1v3s3xx_ddata,&bFlashWrEnCmd,1);
	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,1);
	return 0;
}



static int s1v3s3xx_flash_chip_erase(struct s1v3s3xx_driver_data *s1v3s3xx_ddata)
{
	unsigned char bFlashEraseCmd = 0xc7;

	s1v3s3xx_flash_write_enable(s1v3s3xx_ddata);

	if(s1v3s3xx_flash_wait_ready(s1v3s3xx_ddata,"before flash chip erasing",10000)<0) {
		return -1;
	}
	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);
	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);
	_spi_write(s1v3s3xx_ddata,&bFlashEraseCmd,1);
	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,1);
	return 0;
}
static int s1v3s3xx_flash_sector_erase(struct s1v3s3xx_driver_data *s1v3s3xx_ddata,unsigned long ulEraseAddress)
{
	unsigned char bFlashEraseCmdA[4] = {0x20,};

	//DBG_MSG("erase address=0x%08x\n",ulEraseAddress);
	if( ulEraseAddress & 0xfff ) {
		// invalid address . 
		return -1;
	}

	if(s1v3s3xx_flash_write_enable(s1v3s3xx_ddata)<0) {
		return -2;
	}

	if(s1v3s3xx_flash_wait_ready(s1v3s3xx_ddata,"before flash sector erasing",10000)<0) {
		return -3;
	}

	bFlashEraseCmdA[1] = (unsigned char)(0x0000FF & ulEraseAddress >> 16);
	bFlashEraseCmdA[2] = (unsigned char)(0x0000FF & ulEraseAddress >> 8);
	bFlashEraseCmdA[3] = (unsigned char)(0x0000FF & ulEraseAddress);

	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);
	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);
	_spi_write(s1v3s3xx_ddata,bFlashEraseCmdA,4);
	gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,1);

	return 0;
}

static int s1v3s3xx_flash_write(struct s1v3s3xx_driver_data *s1v3s3xx_ddata,const char *filename)
{
  struct file *file;
  loff_t pos = 0;
  int result = 0;
  mm_segment_t old_fs = get_fs();
	loff_t fsz_chk;
	unsigned long dwBytesToWrite = 0;
	ssize_t tSz;
	int iRet = 0;
	unsigned char bFlashWriteCmdA[4] = {0x02,};
	unsigned char bFlashReadCmdA[5] = {0x0b,0,0,0,0};
	unsigned long ulAddress = 0;
	int iIsFlashAccessMode = 0;
	int iIsSPICS=0;
	int i = 0;

	const int iFlashChunkSize = 256;
	unsigned char bWrCacheA[iFlashChunkSize],bRdCacheA[iFlashChunkSize];
	int iFlashWrBytes;

  file = filp_open(filename, O_RDONLY, 0);
  
  if (IS_ERR(file)) {
  	printk ("[%s-%d] failed open %s,(%p)\n",__func__,__LINE__,filename,file);
		return -1;
  }

	//
	// get file size .
	//
	set_fs(KERNEL_DS);
	fsz_chk = file->f_op->llseek(file,0,SEEK_END);
	dwBytesToWrite = (unsigned long)fsz_chk;
	DBG_MSG("%s : \"%s\" , size=%d\n",__func__,filename,(int)dwBytesToWrite);

	file->f_op->llseek(file,0,SEEK_SET); // goto beginning of the file for reading .


	// switch s1v3s3xx to flash accesss mode . 
	s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_FLASHACCESS_MODE_IND);
	iIsFlashAccessMode=1;

	if(FLASH_ERASE_TYPE_CHIP==s1v3s3xx_ddata->iFlashEraseType) {
		s1v3s3xx_flash_chip_erase(s1v3s3xx_ddata);
		if(s1v3s3xx_flash_wait_ready(s1v3s3xx_ddata,"flash erase complete",60000)<0) {
			iRet=-2;goto exit;
		}
	}

#if 1
	while(dwBytesToWrite>0) { //[
		//
		// read file for flash writting . 
		//
		if((int)dwBytesToWrite>=iFlashChunkSize) {
			iFlashWrBytes = iFlashChunkSize;
		}
		else {
			iFlashWrBytes = (int)dwBytesToWrite;
		}

		tSz = file->f_op->read(file,bWrCacheA,iFlashWrBytes, &file->f_pos);
		if((int)tSz!=(int)iFlashWrBytes) {
			ERR_MSG("[%s-%d] failed read \"%s\",%d!=%d\n",__func__,__LINE__,
					filename,(int)tSz,(int)iFlashWrBytes);
			iRet = -3;
			goto exit;
		}

		if(FLASH_ERASE_TYPE_SECTOR==s1v3s3xx_ddata->iFlashEraseType) {
			if(s1v3s3xx_flash_sector_erase(s1v3s3xx_ddata,ulAddress)>=0) {
				printk("flash sector @0x%08x (4096 bytes) erase done\n",ulAddress);
				if(s1v3s3xx_flash_wait_ready(s1v3s3xx_ddata,"flash erase complete",60000)<0) {
					iRet=-2;goto exit;
				}
			}
		}

		s1v3s3xx_flash_write_enable(s1v3s3xx_ddata);
		if(s1v3s3xx_flash_wait_ready(s1v3s3xx_ddata,"before flash wr",60000)<0) {
			iRet=-2;goto exit;
		}

		// write flash .
		gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);iIsSPICS=1;
		gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);// dummy times for > 100 ns . 


		bFlashWriteCmdA[1] = (unsigned char)(0x0000FF & ulAddress >> 16);
		bFlashWriteCmdA[2] = (unsigned char)(0x0000FF & ulAddress >> 8);
		bFlashWriteCmdA[3] = (unsigned char)(0x0000FF & ulAddress);
		printk("flash WR@0x%08x,%d bytes\n",ulAddress,iFlashWrBytes);
		_spi_write(s1v3s3xx_ddata,bFlashWriteCmdA,sizeof(bFlashWriteCmdA));
		_spi_write(s1v3s3xx_ddata,bWrCacheA,iFlashWrBytes);

		gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,1);iIsSPICS=0;

		/////////////////////////////////////////////////
		// verify flash data .
		//
		if(s1v3s3xx_flash_wait_ready(s1v3s3xx_ddata,"before flash reading",60000)<0) {
			iRet=-2;goto exit;
		}

		gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);iIsSPICS=1;
		gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,0);// dummy times for > 100 ns . 

		bFlashReadCmdA[1] = (unsigned char)(0x0000FF & ulAddress >> 16);
		bFlashReadCmdA[2] = (unsigned char)(0x0000FF & ulAddress >> 8);
		bFlashReadCmdA[3] = (unsigned char)(0x0000FF & ulAddress);
		printk("flash RD@0x%08x,%d bytes for verify \n",ulAddress,iFlashWrBytes);
		_spi_write(s1v3s3xx_ddata,bFlashReadCmdA,sizeof(bFlashReadCmdA));
		_spi_read(s1v3s3xx_ddata,bRdCacheA,iFlashWrBytes);

		if(0!=memcmp(bRdCacheA,bWrCacheA,iFlashWrBytes)) {
			ERR_MSG("%s : flash verify failed @ 0x%08x\n",__func__,ulAddress);
			printk("0x%02x,0x%02x,0x%02x,0x%02x\n",
					bRdCacheA[0],bRdCacheA[1],bRdCacheA[2],bRdCacheA[3]);
			iRet=-5;goto exit;
		}

		gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,1);iIsSPICS=0;

		////////////////////////////////////////////////////////

		dwBytesToWrite -= iFlashWrBytes;
		ulAddress += iFlashWrBytes;

	} //] end of while loop .
#endif



exit:

	if(iIsSPICS) {
		gpio_set_value(s1v3s3xx_ddata->gpio_TTS_CS,1);
	}

	if (iIsFlashAccessMode) {
		// switch back s1v3s3xx to normal mode . 
		s1v3s3xx_hw_reset(s1v3s3xx_ddata);
		// s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_RESET_REQ);
	}

  if (!IS_ERR(file)) {
		filp_close(file, NULL);file=0;
		set_fs(old_fs);
	}
	return iRet;
}

///////////////////////////////////////////////////////////////
static ssize_t request_get(struct device *dev, struct device_attribute *attr,	char *buf)
{
	struct s1v3s3xx_driver_data *s1v3s3xx_ddata;
	s1v3s3xx_ddata = dev_get_drvdata(dev);

	sprintf(buf,"STATUS=%d\nID_ISC_SEND=0x%04x\nID_ISC_RECV=0x%04x\nERR_CODE=0x%04x\n",
			s1v3s3xx_ddata->iLastReqError,
			s1v3s3xx_ddata->wLastSendIscID,
			s1v3s3xx_ddata->wLastRecvIscID,
			s1v3s3xx_ddata->wErrorCode);

	return strlen(buf);
}


static void _splay_user_command_usage(void)
{
	printk("<play_count>,<file_no&delay_ms>,[file_no&delay_ms],...\n");
	printk(" <play_count> : -1 for infinite play or 1~65534 .\n");
	printk(" <file_no> : the file number of EOV .\n");
	printk(" <delay_ms> : the delay ms (2~2047) inserted before playback of sequence file \n");
}
static void _audio_config_user_command_usage(void)
{
	printk("<audio_gain_db>,<audio_sample_rate_hz>\n");
	printk(" <audio_gain_db> : -48~+18 , <48=mute .\n");
	printk(" <audio_sample_rate_hz> : 16000 is 16KHz , others don't care .\n");
}

static ssize_t request_set(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	const int iParamLen=1024;
	struct s1v3s3xx_driver_data *s1v3s3xx_ddata;
	s1v3s3xx_ddata = dev_get_drvdata(dev);
	char cREQA[64];
	char cParamA[iParamLen];
	int iIsREQ_Ready = 1;
	//const unsigned short wREQ_invalid=0xffff;
	unsigned short wREQ;
	void *pvREQ = 0;
	int i;
	char *pc;
	int iLen;
	int iIsChkMsgRDY = 1;


	pc = strchr(buf,':');
	if(pc) {
		// has parameters ,
		if(strlen(pc+2)>=iParamLen) {
			ERR_MSG("parameter length should be < %d\n",iParamLen);
			return strlen(buf);
		}
		sscanf(buf,"%s : %s\n",cREQA,cParamA);
	}
	else {
		sscanf(buf,"%s\n",cREQA);
		cParamA[0] = '\0';
	}

	DBG_MSG("%s() : req=\"%s\",param=\"%s\"\n",__func__,cREQA,cParamA);

	if(0==strcmp(cREQA,"VERSION_REQ")) {
		wREQ = ID_ISC_VERSION_REQ;
	}
	else if(0==strcmp(cREQA,"TEST_REQ")) {
		char cKeyCodeA[12];

		if('\0'==cParamA[0]) {
			return strlen(buf);
		}

		if(strlen(cParamA)>=12) {
		}
		else {

			s1v3s3xx_ddata->wCheckSumEnable = INVALID_WVAL;
			s1v3s3xx_ddata->wMsgReadyEnable = INVALID_WVAL;
			sscanf(cParamA,"%s",cKeyCodeA);
			DBG_MSG("keycode=\"%s\"\n",cKeyCodeA);
			if('0'==cKeyCodeA[0] && 'x'==cKeyCodeA[1]) {
				s1v3s3xx_ddata->dwKeyCode = simple_strtol(cKeyCodeA, NULL, 16);
			}
			else {
				s1v3s3xx_ddata->dwKeyCode = simple_strtol(cKeyCodeA, NULL, 10);
			}
			wREQ = ID_ISC_TEST_REQ;
		}
	}
	else if(0==strcmp(cREQA,"AUDIO_CONFIG_REQ")) {
		char *pcE,*pcS,*pcD;

		if('\0'==cParamA[0]) {
			_audio_config_user_command_usage();
			return strlen(buf);
		}

		wREQ = ID_ISC_AUDIO_CONFIG_REQ;
		pcS = &cParamA[0];
		pcE = strchr(pcS,',');
		if(pcE) {
			*pcE = '\0';
			pcD = pcE+1;
		}
		else {
			pcD = 0;
		}
		s1v3s3xx_ddata->iAudioGainDB = simple_strtol(pcS, NULL, 10);
		DBG_MSG("audio_gain_db=%d\n",s1v3s3xx_ddata->iAudioGainDB);
		if(pcD) { 
			s1v3s3xx_ddata->dwAudioSampleRateHz = simple_strtoul(pcD, NULL, 10);
		}
		DBG_MSG("audio_sample_rate_hz=%d\n",(int)s1v3s3xx_ddata->dwAudioSampleRateHz);
	}
#if 0
	else if(0==strcmp(cREQA,"AUDIO_VOLUME_REQ")) {
		wREQ = ID_ISC_AUDIO_VOLUME_REQ;
	}
#endif
	else if(0==strcmp(cREQA,"RESET_REQ")) {
		wREQ = ID_ISC_RESET_REQ;
	}
	else if(0==strcmp(cREQA,"PMAN_STANDBY_ENTRY_REQ")) {
		wREQ = ID_ISC_PMAN_STANDBY_ENTRY_REQ;
	}
#if 0
	else if(0==strcmp(cREQA,"AUDIODEC_CONFIG_REQ")) {
		wREQ = ID_ISC_AUDIODEC_CONFIG_REQ;
	}
	else if(0==strcmp(cREQA,"AUDIODEC_DECODE_REQ")) {
		wREQ = ID_ISC_AUDIODEC_DECODE_REQ;
	}
	else if(0==strcmp(cREQA,"AUDIODEC_PAUSE_REQ")) {
		wREQ = ID_ISC_AUDIODEC_PAUSE_REQ;
	}
	else if(0==strcmp(cREQA,"AUDIODEC_STOP_REQ")) {
		wREQ = ID_ISC_AUDIODEC_STOP_REQ;
	}
#endif 
	else if(0==strcmp(cREQA,"SEQUENCER_CONFIG_REQ")) {

		char *pcE,*pcS,*pcD;
		if('\0'==cParamA[0]) {
			_splay_user_command_usage();
			return strlen(buf);
		}

		wREQ = ID_ISC_SEQUENCER_CONFIG_REQ;
		// repeat count .
		pcS = &cParamA[0];
		pcE = strchr(pcS,',');
		if(pcE) {
			*pcE = '\0';
		}
		else {
			ERR_MSG("<play_count> and <file_no> must be assigned !\n");
			_splay_user_command_usage();
			return strlen(buf);
		}
		s1v3s3xx_ddata->iSPlayCount = simple_strtol(pcS, NULL, 10);
		DBG_MSG("play_count=%d\n",s1v3s3xx_ddata->iSPlayCount);

		pcS = pcE+1;
		
		// file_no&delay_ms
		for(pcE = pcS,i=0;pcE;pcS=pcE+1,i++) {

			if(i>=SPLAY_FILES_TOTAL) {
				WARNING_MSG("file count cannot >= %d\n",SPLAY_FILES_TOTAL);
				break;
			}

			pcE = strchr(pcS,',');
			if(pcE) {
				*pcE = '\0';
			}
			pcD = strchr(pcS,'&');
			if(pcD) {
				*pcD++ = '\0';
				s1v3s3xx_ddata->SPlayEvtA[i].iDelay_ms = simple_strtol(pcD, NULL, 10);
			}
			else {
				s1v3s3xx_ddata->SPlayEvtA[i].iDelay_ms = 100;
			}
			s1v3s3xx_ddata->SPlayEvtA[i].iFile_no = simple_strtol(pcS, NULL, 10);

			DBG_MSG("play_fno=%d,delayms=%d\n",
					s1v3s3xx_ddata->SPlayEvtA[i].iFile_no,
					s1v3s3xx_ddata->SPlayEvtA[i].iDelay_ms);
		}
		s1v3s3xx_ddata->iSPlayFiles=i;

		DBG_MSG("play_files=%d\n",s1v3s3xx_ddata->iSPlayFiles);
	}
	else if(0==strcmp(cREQA,"AUDIO_VOLUME_REQ")) {
		wREQ = ID_ISC_AUDIO_VOLUME_REQ;
		if('\0'!=cParamA[0]) {
			s1v3s3xx_ddata->iAudioGainDB = simple_strtol(cParamA, NULL, 10);
		}
	}
	else if(0==strcmp(cREQA,"AUDIO_MUTE_REQ")) {
		wREQ = ID_ISC_AUDIO_MUTE_REQ;
		if('\0'!=cParamA[0]) {
			s1v3s3xx_ddata->iIsMute = simple_strtol(cParamA, NULL, 10);
		}
	}
	else if(0==strcmp(cREQA,"SEQUENCER_START_REQ")) {
		wREQ = ID_ISC_SEQUENCER_START_REQ;
		if('\0'!=cParamA[0]) {
			s1v3s3xx_ddata->iIsEnableStatusIND = simple_strtol(cParamA, NULL, 10);
		}
	}
	else if(0==strcmp(cREQA,"SEQUENCER_PAUSE_REQ")) {
		wREQ = ID_ISC_SEQUENCER_PAUSE_REQ;
		if('\0'!=cParamA[0]) {
			s1v3s3xx_ddata->iIsPause = simple_strtol(cParamA, NULL, 10);
		}
	}
	else if(0==strcmp(cREQA,"SEQUENCER_STOP_REQ")) {
		wREQ = ID_ISC_SEQUENCER_STOP_REQ;
	}
	else if(0==strcmp(cREQA,"RECV_MSG_ANY")) {
		
		wREQ = ID_ISC_INVALID;
		if('\0'!=cParamA[0]) {
			iIsChkMsgRDY = simple_strtol(cParamA, NULL, 10);
		}
		else {
			iIsChkMsgRDY = 0;
		}
	}
	else {
		iIsREQ_Ready = 0;
		ERR_MSG("%s : unsupported REQ/IND\n",__func__);
	}

	if(iIsREQ_Ready) {
		if(iIsChkMsgRDY) {
			if(gpio_is_valid(s1v3s3xx_ddata->gpio_TTS_INT)) {
				if(gpio_get_value(s1v3s3xx_ddata->gpio_TTS_INT)) {
					if(ID_ISC_INVALID==wREQ) {
						// request is just getting IND/RESP , just do the main job .
					}
					else {
						// there is a message before requesting , we need to clean it . 
						DBG_MSG("Got MSGRDY ,clear IND !\n");
						if(s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_INVALID)>=0) {
							// if really got message , break this request !.
							return strlen(buf);
						}
					}
				}
				else {
					
					if(ID_ISC_INVALID==wREQ) {
						DBG_MSG("No MSGRDY signal !\n");
						// request is just getting IND/RESP we can just leave .
						return strlen(buf);
					}
				}
			}
			else {
				WARNING_MSG("%s(%d):TTS-INT gpio invalid !\n",__func__,__LINE__);
			}
		}
		s1v3s3xx_request(s1v3s3xx_ddata,wREQ);
	}

	return strlen(buf);
}
static DEVICE_ATTR (request, 0644, request_get,request_set);


///////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////
static ssize_t version_get(struct device *dev, struct device_attribute *attr,	char *buf)
{
	struct s1v3s3xx_driver_data *s1v3s3xx_ddata;
	s1v3s3xx_ddata = dev_get_drvdata(dev);

	sprintf(buf,"hw_id=0x%02x\nhw_id_frac=0x%02x\nfw_features=0x%08x\n",
			s1v3s3xx_ddata->hw_id_int,s1v3s3xx_ddata->hw_id_frac,s1v3s3xx_ddata->fw_feature);

	return strlen(buf);
}

static ssize_t version_set(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{

	struct s1v3s3xx_driver_data *s1v3s3xx_ddata;
	s1v3s3xx_ddata = dev_get_drvdata(dev);

	s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_VERSION_REQ);

	return strlen(buf);
}
static DEVICE_ATTR (version, 0644, version_get,version_set);

///////////////////////////////////////////////////////////////////////

static ssize_t splay_status_get(struct device *dev, struct device_attribute *attr,	char *buf)
{
	struct s1v3s3xx_driver_data *s1v3s3xx_ddata;
	s1v3s3xx_ddata = dev_get_drvdata(dev);

	if(gpio_is_valid(s1v3s3xx_ddata->gpio_TTS_INT)) {
		if(gpio_get_value(s1v3s3xx_ddata->gpio_TTS_INT)) {
				// there is a message before requesting , we need to clean it . 
			DBG_MSG("Got MSGRDY ,taking IND !\n");
			if(s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_INVALID)>=0) {
				// no messages .
			}
		}
		else {
			DBG_MSG("No MSGRDY signal !\n");
		}
	}
	else {
		WARNING_MSG("%s(%d):TTS-INT gpio invalid !\n",__func__,__LINE__);
		if(s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_INVALID)>=0) {
			// no messages .
		}
	}
	
	switch (s1v3s3xx_ddata->iSPlayStatus)
	{
		case SPLAY_STATUS_STOP:
			sprintf(buf,"STOP\n");
			break;
		case SPLAY_STATUS_PAUSE:
			sprintf(buf,"PAUSE\n");
			break;
		default:
		case SPLAY_STATUS_PLAYING:
			sprintf(buf,"PLAYING\n");
			break;
	}
	return strlen(buf);
}

static ssize_t splay_status_set(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{

	struct s1v3s3xx_driver_data *s1v3s3xx_ddata;
	s1v3s3xx_ddata = dev_get_drvdata(dev);

	return strlen(buf);
}
static DEVICE_ATTR (splay_status, 0644, splay_status_get,splay_status_set);



///////////////////////////////////////////////////////////////
static ssize_t flash_get(struct device *dev, struct device_attribute *attr,	char *buf)
{
	struct s1v3s3xx_driver_data *s1v3s3xx_ddata;
	s1v3s3xx_ddata = dev_get_drvdata(dev);


	return strlen(buf);
}

static ssize_t flash_set(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{

	struct s1v3s3xx_driver_data *s1v3s3xx_ddata;
	s1v3s3xx_ddata = dev_get_drvdata(dev);
	char fname[256];

	sscanf(buf,"%s\n",fname);
	
	s1v3s3xx_flash_write(s1v3s3xx_ddata,fname);

	return strlen(buf);
}
static DEVICE_ATTR (flash, 0644, flash_get,flash_set);
//////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////
static ssize_t flash_test_get(struct device *dev, struct device_attribute *attr,	char *buf)
{
	struct s1v3s3xx_driver_data *s1v3s3xx_ddata;
	s1v3s3xx_ddata = dev_get_drvdata(dev);


	return strlen(buf);
}
static ssize_t flash_test_set(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{

	struct s1v3s3xx_driver_data *s1v3s3xx_ddata;
	s1v3s3xx_ddata = dev_get_drvdata(dev);
	char cmd[256];
#define FLASH_TEST_CMD_NA			0
#define FLASH_TEST_CMD_RDID		1
#define FLASH_TEST_CMD_MISC		2
	int iCmd = FLASH_TEST_CMD_NA;
	int iIsFlashAccessMode = 0;

	sscanf(buf,"%s\n",cmd);
	printk("flash test cmd=\"%s\"\n",cmd);
	if(0==strcmp(cmd,"RDID")) {
		iCmd = FLASH_TEST_CMD_RDID;
	}
	else if(0==strcmp(cmd,"MISC")) {
		iCmd = FLASH_TEST_CMD_MISC;
	}

	if(iCmd!=FLASH_TEST_CMD_NA) {
		s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_FLASHACCESS_MODE_IND);
		iIsFlashAccessMode=1;
	}

	switch (iCmd) 
	{
	case FLASH_TEST_CMD_RDID:
		{
			unsigned char bManuID;
			unsigned short wDevID;

			s1v3s3xx_flash_get_ids(s1v3s3xx_ddata,&bManuID,&wDevID);

			printk("Manufacturer ID = 0x%02x\n",bManuID);
			printk("Device ID = 0x%04x\n",wDevID);

		}
		break;
	}

	if (iIsFlashAccessMode) {
		// switch back s1v3s3xx to normal mode . 
		s1v3s3xx_hw_reset(s1v3s3xx_ddata);
		// s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_RESET_REQ);
	}


	return strlen(buf);
}
static DEVICE_ATTR (flash_test, 0644, flash_test_get,flash_test_set);
/////////////////////////////////////////////////////////////////

static ssize_t reset_get(struct device *dev, struct device_attribute *attr,	char *buf)
{
	struct s1v3s3xx_driver_data *s1v3s3xx_ddata;
	s1v3s3xx_ddata = dev_get_drvdata(dev);

	return strlen(buf);
}

static ssize_t reset_set(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	unsigned short	usReceivedMessageID;
	int iError;

	struct s1v3s3xx_driver_data *s1v3s3xx_ddata;
	s1v3s3xx_ddata = dev_get_drvdata(dev);

	DBG_MSG("%s() \n",__func__);

	if( 0==strcmp("1",buf) || 0==strcmp("h",buf)) {
		// hardware reset .
		s1v3s3xx_hw_reset(s1v3s3xx_ddata);
	}
	else if( 0==strcmp("0",buf) || 0==strcmp("s",buf)) {
		// software reset .
		s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_RESET_REQ);
		msleep(10);
	}
	else if( 0==strcmp("2",buf) || 0==strcmp("hs",buf)) {
		// hard reset .
		s1v3s3xx_hw_reset(s1v3s3xx_ddata);
		DBG_MSG("%s():software reset \n",__func__);
		s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_RESET_REQ);
		msleep(10);
	}
	else {
	}

exit:
	return strlen(buf);
}
static DEVICE_ATTR (reset, 0644, reset_get,reset_set);

static irqreturn_t s1v3s3xx_irq_handler(int irq, void *dev)
{
	struct s1v3s3xx_driver_data *s1v3s3xx_ddata = (struct s1v3s3xx_driver_data *)dev;

	if(s1v3s3xx_ddata->iIsInRequestProc) {
		return IRQ_HANDLED;
	}

	DBG_MSG("%s(),INT=%d\n",__func__,gpio_get_value(s1v3s3xx_ddata->gpio_TTS_INT));

	if(s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_INVALID)>=0) {
		//ntx_report_key(1,KEY_PLAYER);
		//ntx_report_key(0,KEY_PLAYER);
	}

	return IRQ_HANDLED;
}

static int s1v3s3xx_suspend(struct spi_device *spi, pm_message_t mesg)
{
	struct s1v3s3xx_driver_data *s1v3s3xx_ddata = dev_get_drvdata(&spi->dev);

	if(SPLAY_STATUS_STOP!=s1v3s3xx_ddata->iSPlayStatus) {
		ERR_MSG("%s : s1v3s3xx in playing, please stop it or try this latter \n",__func__);
		return -1;
	}

	if(gpio_is_valid(s1v3s3xx_ddata->gpio_TTS_INT)) {
		while(gpio_get_value(s1v3s3xx_ddata->gpio_TTS_INT)) {
			DBG_MSG("%s : s1v3s3xx ind/resp is pending , go check them ...\n",__func__);
			if(s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_INVALID)>=0) {
				// error occuring ...
			}
		}
	}

	if(gpio_is_valid(s1v3s3xx_ddata->gpio_TTS_STB)) {
		//gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_STB,0);
		//udelay(50);
		if ( s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_PMAN_STANDBY_ENTRY_REQ)<0 )
		{ 
			ERR_MSG("%s(), S1V3S3XX enter standby failed !\n",__func__);
			//return -1;
		}
		gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_STB,1);
		//udelay(800);
	}

	return 0;
}

static int s1v3s3xx_resume(struct spi_device *spi)
{
	struct s1v3s3xx_driver_data *s1v3s3xx_ddata = dev_get_drvdata(&spi->dev);

	if(gpio_is_valid(s1v3s3xx_ddata->gpio_TTS_STB)) {
		gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_STB,0);
		//msleep(120);
	}
	

	return 0;
}
static int s1v3s3xx_remove(struct spi_device *spi)
{
	int ret=0;
	struct s1v3s3xx_driver_data *s1v3s3xx_ddata = dev_get_drvdata(&spi->dev);

#ifdef CONFIG_MACH_MX6SL_NTX //[
	mxc_iomux_v3_setup_pad(s1v3s3xx_ddata->tPadctrl_CS_Old);
	gpio_free (s1v3s3xx_ddata->gpio_TTS_CS);

	mxc_iomux_v3_setup_pad(s1v3s3xx_ddata->tPadctrl_RST_Old);
	gpio_free (s1v3s3xx_ddata->gpio_TTS_RST);
	if(gpio_is_valid(s1v3s3xx_ddata->gpio_TTS_INT)) {
		mxc_iomux_v3_setup_pad(s1v3s3xx_ddata->tPadctrl_INT_Old);
		gpio_free (s1v3s3xx_ddata->gpio_TTS_INT);
		//free_irq(s1v3s3xx_ddata->irq_TTS_INT,s1v3s3xx_ddata);
	}
	if(gpio_is_valid(s1v3s3xx_ddata->gpio_TTS_STB)) {
		mxc_iomux_v3_setup_pad(s1v3s3xx_ddata->tPadctrl_STB_Old);
		gpio_free (s1v3s3xx_ddata->gpio_TTS_STB);
	}
	if(gpio_is_valid(s1v3s3xx_ddata->gpio_AMP)) {
		mxc_iomux_v3_setup_pad(s1v3s3xx_ddata->tPadctrl_AMP_Old);
		gpio_free (s1v3s3xx_ddata->gpio_AMP);
	}
	if(gpio_is_valid(s1v3s3xx_ddata->gpio_BOOST)) {
		mxc_iomux_v3_setup_pad(s1v3s3xx_ddata->tPadctrl_BOOST_Old);
		gpio_free (s1v3s3xx_ddata->gpio_BOOST);
	}
#endif //] CONFIG_MACH_MX6SL_NTX
	if(s1v3s3xx_ddata->pbMsgData) {
		kfree(s1v3s3xx_ddata->pbMsgData);
		s1v3s3xx_ddata->pbMsgData=0;
		s1v3s3xx_ddata->wMsgDataBytes=0;
	}

	if(s1v3s3xx_ddata->iIsISR_ready) {
		//printk("%s():free irq(%d)\n",__func__,s1v3s3xx_ddata->irq_TTS_INT);
		free_irq(s1v3s3xx_ddata->irq_TTS_INT,0);
		s1v3s3xx_ddata->iIsISR_ready = 0;
	}


	device_remove_file(&spi->dev,&dev_attr_version);
	device_remove_file(&spi->dev,&dev_attr_reset);
	device_remove_file(&spi->dev,&dev_attr_flash);
	device_remove_file(&spi->dev,&dev_attr_flash_test);
	device_remove_file(&spi->dev,&dev_attr_request);
	device_remove_file(&spi->dev,&dev_attr_splay_status);

	if(s1v3s3xx_ddata) {
		devm_kfree(&spi->dev,s1v3s3xx_ddata);
	}

	return ret ;
}


static int s1v3s3xx_probe(struct spi_device *spi)
{
	int ret = 0;
	struct s1v3s3xx_platform_data *pdata = spi->dev.platform_data;
	struct s1v3s3xx_driver_data *s1v3s3xx_ddata;
	int rval;

	printk("%s()\n",__func__);

	s1v3s3xx_ddata = devm_kzalloc(&spi->dev, 
			sizeof(struct s1v3s3xx_driver_data),GFP_KERNEL);
	if (!s1v3s3xx_ddata) {
		return -ENOMEM;
	}
	s1v3s3xx_ddata->spi_dev = spi;
	s1v3s3xx_ddata->pdata = pdata;

	s1v3s3xx_ddata->gpio_TTS_INT = pdata->gpio_TTS_INT;
	s1v3s3xx_ddata->gpio_TTS_RST = pdata->gpio_TTS_RST;
	s1v3s3xx_ddata->gpio_TTS_STB = pdata->gpio_TTS_STB;
	s1v3s3xx_ddata->gpio_TTS_CS = pdata->gpio_TTS_CS;

	s1v3s3xx_ddata->gpio_AMP = pdata->gpio_AMP;
	s1v3s3xx_ddata->gpio_BOOST = pdata->gpio_BOOST;
	s1v3s3xx_ddata->iDelay_ms_AMP_to_BOOST = 500;

	s1v3s3xx_ddata->iSPlayStatus = SPLAY_STATUS_STOP;
	
	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_3;
	rval = spi_setup(spi);
	if (rval < 0) {
		dev_err (&spi->dev, "spi setup error!");
		ret=-EIO;goto err_exit;
	}
	dev_set_drvdata(&spi->dev, s1v3s3xx_ddata);

	
#ifdef CONFIG_MACH_MX6SL_NTX //[
	if ( IMX_GPIO_NR(2,22) == s1v3s3xx_ddata->gpio_TTS_CS) {
		DBG_MSG("TTS_CS=GPIO_2_22 (%d)\n",s1v3s3xx_ddata->gpio_TTS_CS);
		s1v3s3xx_ddata->tPadctrl_CS_Old = s1v3s3xx_ddata->tPadctrl_CS = MX6SL_PAD_LCD_DAT2__GPIO_2_22_SION;
		mxc_iomux_v3_get_pad(&s1v3s3xx_ddata->tPadctrl_CS_Old);
		mxc_iomux_v3_setup_pad(s1v3s3xx_ddata->tPadctrl_CS);
	}
	else
	if ( IMX_GPIO_NR(4,15) == s1v3s3xx_ddata->gpio_TTS_CS) {
		DBG_MSG("TTS_CS=GPIO_4_15 (%d)\n",s1v3s3xx_ddata->gpio_TTS_CS);
		s1v3s3xx_ddata->tPadctrl_CS_Old = s1v3s3xx_ddata->tPadctrl_CS = MX6SL_PAD_ECSPI2_SS0__GPIO_4_15;
		mxc_iomux_v3_get_pad(&s1v3s3xx_ddata->tPadctrl_CS_Old);
		mxc_iomux_v3_setup_pad(s1v3s3xx_ddata->tPadctrl_CS);
	}
	else {
		dev_err(&spi->dev, "%s : CS of TTS must be assigned !!.\n",__func__);
		ret=-EIO;goto err_exit;
	}
	if ( IMX_GPIO_NR(3,29) == s1v3s3xx_ddata->gpio_TTS_RST) {
		DBG_MSG("TTS_RST=GPIO_3_29 (%d)\n",s1v3s3xx_ddata->gpio_TTS_RST);
		s1v3s3xx_ddata->tPadctrl_RST_Old = s1v3s3xx_ddata->tPadctrl_RST = MX6SL_PAD_KEY_ROW2__GPIO_3_29_OUTPUT;
		mxc_iomux_v3_get_pad(&s1v3s3xx_ddata->tPadctrl_RST_Old);
		mxc_iomux_v3_setup_pad(s1v3s3xx_ddata->tPadctrl_RST);
	}
	else {
		dev_err(&spi->dev, "%s : RST of TTS must be assigned !!.\n",__func__);
		ret=-EIO;goto err_exit;
	}


	if ( IMX_GPIO_NR(4,3) == s1v3s3xx_ddata->gpio_TTS_INT) {
		DBG_MSG("TTS_INT=GPIO_4_3 (%d)\n",s1v3s3xx_ddata->gpio_TTS_INT);
		s1v3s3xx_ddata->tPadctrl_INT_Old = s1v3s3xx_ddata->tPadctrl_INT = MX6SL_PAD_KEY_ROW5__GPIO_4_3_PDINT;
		mxc_iomux_v3_get_pad(&s1v3s3xx_ddata->tPadctrl_INT_Old);
		mxc_iomux_v3_setup_pad(s1v3s3xx_ddata->tPadctrl_INT);

#if 0
		printk("\n\nTESTING\n\n");
		while(1){		
			mdelay(100);
			gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_INT,0);
			mdelay(100);
			gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_INT,1);
		}
#endif
	}
	else {
		s1v3s3xx_ddata->gpio_TTS_INT = -1;
		dev_warn(&spi->dev, "%s : INT of TTS not assigned !!.\n",__func__);
	}


	if ( IMX_GPIO_NR(3,31) == s1v3s3xx_ddata->gpio_TTS_STB) {
		DBG_MSG("TTS_STB=GPIO_3_31 (%d)\n",s1v3s3xx_ddata->gpio_TTS_STB);
		s1v3s3xx_ddata->tPadctrl_STB_Old = s1v3s3xx_ddata->tPadctrl_STB = MX6SL_PAD_KEY_ROW3__GPIO_3_31;
		mxc_iomux_v3_get_pad(&s1v3s3xx_ddata->tPadctrl_STB_Old);
		mxc_iomux_v3_setup_pad(s1v3s3xx_ddata->tPadctrl_STB);
	}
	else {
		s1v3s3xx_ddata->gpio_TTS_STB = -1;
		dev_warn(&spi->dev, "%s : STB of TTS not assigned !!.\n",__func__);
	}

	if ( IMX_GPIO_NR(3,30) == s1v3s3xx_ddata->gpio_AMP) {
		DBG_MSG("AMP=GPIO_3_30 (%d)\n",s1v3s3xx_ddata->gpio_AMP);
		s1v3s3xx_ddata->tPadctrl_AMP_Old = s1v3s3xx_ddata->tPadctrl_AMP = MX6SL_PAD_KEY_COL3__GPIO_3_30_SION_OUT;
		mxc_iomux_v3_get_pad(&s1v3s3xx_ddata->tPadctrl_AMP_Old);
		mxc_iomux_v3_setup_pad(s1v3s3xx_ddata->tPadctrl_AMP);
	}
	else {
		s1v3s3xx_ddata->gpio_AMP = -1;
	}
	if ( IMX_GPIO_NR(3,24) == s1v3s3xx_ddata->gpio_BOOST) {
		DBG_MSG("BOOST=GPIO_3_24 (%d)\n",s1v3s3xx_ddata->gpio_BOOST);
		s1v3s3xx_ddata->tPadctrl_BOOST_Old = s1v3s3xx_ddata->tPadctrl_BOOST = MX6SL_PAD_KEY_COL0__GPIO_3_24_SION_OUT;
		mxc_iomux_v3_get_pad(&s1v3s3xx_ddata->tPadctrl_BOOST_Old);
		mxc_iomux_v3_setup_pad(s1v3s3xx_ddata->tPadctrl_BOOST);
	}
	else {
		s1v3s3xx_ddata->gpio_BOOST = -1;
	}
	
#endif //] CONFIG_MACH_MX6SL_NTX

	gpio_request (s1v3s3xx_ddata->gpio_TTS_CS, "S1V3S3XX_CS");
	gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_CS,1);

	gpio_request (s1v3s3xx_ddata->gpio_TTS_RST, "S1V3S3XX_RST");
	gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_RST,0);


	if(gpio_is_valid(s1v3s3xx_ddata->gpio_TTS_INT)) {
		gpio_request (s1v3s3xx_ddata->gpio_TTS_INT, "S1V3S3XX_INT");
		gpio_direction_input(s1v3s3xx_ddata->gpio_TTS_INT);
	}

	if(gpio_is_valid(s1v3s3xx_ddata->gpio_TTS_STB)) {
		gpio_request (s1v3s3xx_ddata->gpio_TTS_STB, "S1V3S3XX_STB");
		gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_STB,0);
	}

	if(gpio_is_valid(s1v3s3xx_ddata->gpio_AMP)) {
		gpio_request (s1v3s3xx_ddata->gpio_AMP, "S1V3S3XX_AMP");
		gpio_direction_output(s1v3s3xx_ddata->gpio_AMP,0);
	}
	if(gpio_is_valid(s1v3s3xx_ddata->gpio_BOOST)) {
		gpio_request (s1v3s3xx_ddata->gpio_BOOST, "S1V3S3XX_BOOST");
		gpio_direction_output(s1v3s3xx_ddata->gpio_BOOST,0);
	}

	s1v3s3xx_ddata->iAudioGainDB = -20;
	s1v3s3xx_ddata->dwAudioSampleRateHz = 16000;


	// sequencer play .
	s1v3s3xx_ddata->iSPlayCount = 0;
	s1v3s3xx_ddata->iSPlayFiles = 0;
	s1v3s3xx_ddata->pbMsgData = 0;
	s1v3s3xx_ddata->wMsgDataBytes = 0;

	gs1v3s3xx_ddata = s1v3s3xx_ddata;


	gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_RST,1);
	udelay(100);

	{ // workarround solution for 1st SCLK issue . 
		unsigned char bDummy;
		gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_CS,0);
		gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_CS,0);
		_spi_read(s1v3s3xx_ddata,&bDummy,1);
		gpio_direction_output(s1v3s3xx_ddata->gpio_TTS_CS,1);
	}
#ifdef S1V3S3XX_MSGRDY_INT //[
	if(gpio_is_valid(s1v3s3xx_ddata->gpio_TTS_INT)) {
		s1v3s3xx_ddata->irq_TTS_INT = gpio_to_irq(s1v3s3xx_ddata->gpio_TTS_INT);
		if(request_threaded_irq(s1v3s3xx_ddata->irq_TTS_INT, 0,s1v3s3xx_irq_handler,
		     IRQF_TRIGGER_RISING | IRQF_ONESHOT, S1V3S3XX_NAME, s1v3s3xx_ddata)) 
		{
			ERR_MSG("%s() : register TTS interrupt failed\n",__FUNCTION__);
		}
		else {
			s1v3s3xx_ddata->iIsISR_ready = 1;
		}
	}
#endif //]S1V3S3XX_MSGRDY_INT

	rval = device_create_file(&spi->dev, &dev_attr_version);
	if (rval < 0) {
		dev_err(&spi->dev, "%s : sysfs version creating failed.\n",__func__);
	}
	rval = device_create_file(&spi->dev, &dev_attr_reset);
	if (rval < 0) {
		dev_err(&spi->dev, "%s : sysfs reset creating failed.\n",__func__);
	}
	rval = device_create_file(&spi->dev, &dev_attr_flash);
	if (rval < 0) {
		dev_err(&spi->dev, "%s : sysfs flash creating failed.\n",__func__);
	}
	rval = device_create_file(&spi->dev, &dev_attr_flash_test);
	if (rval < 0) {
		dev_err(&spi->dev, "%s : sysfs flash_test creating failed.\n",__func__);
	}
	rval = device_create_file(&spi->dev, &dev_attr_request);
	if (rval < 0) {
		dev_err(&spi->dev, "%s : sysfs request creating failed.\n",__func__);
	}
	rval = device_create_file(&spi->dev, &dev_attr_splay_status);
	if (rval < 0) {
		dev_err(&spi->dev, "%s : sysfs splay_status creating failed.\n",__func__);
	}

#if 1
	//s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_RESET_REQ);
	if(s1v3s3xx_request(s1v3s3xx_ddata,ID_ISC_VERSION_REQ)>=0) {
		printk("s1v3s3xx hw_id=0x%02x,hw_id_frac=0x%02x,fw_features=0x%08x\n",
			s1v3s3xx_ddata->hw_id_int,s1v3s3xx_ddata->hw_id_frac,s1v3s3xx_ddata->fw_feature);
	}
	else {
		printk(KERN_ERR"%s() :get version failed !\n",__FUNCTION__);
		//ret = -EIO;goto err_exit;
	}
#endif

	return 0;

err_exit:

	s1v3s3xx_remove(spi);

	return ret;
}

static struct spi_driver s1v3s3xx_spi_driver = {
	.driver = {
		   .name = S1V3S3XX_NAME,
		   },
	.probe = s1v3s3xx_probe,
	.remove = s1v3s3xx_remove,
	.suspend = s1v3s3xx_suspend,
	.resume = s1v3s3xx_resume,
};

static int __init s1v3s3xx_init(void)
{
	return spi_register_driver(&s1v3s3xx_spi_driver);
}

static void __exit s1v3s3xx_exit(void)
{
	spi_unregister_driver(&s1v3s3xx_spi_driver);
}
module_init(s1v3s3xx_init);
module_exit(s1v3s3xx_exit);

MODULE_DESCRIPTION("Netronix S1V3S3XX driver");
MODULE_LICENSE("GPL v2");


