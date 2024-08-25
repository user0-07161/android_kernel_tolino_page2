/////////////////////////////////////////////////////////////////////////////////
// File Name: spi_api.c
//
// Description: Sample for API specification
//
// Author: SEIKO EPSON
//
// History: 2008/04/18 1st. design
//
// Copyright(c) SEIKO EPSON CORPORATION 2008, All rights reserved.
//
// $Id: spi_api.c,v 1.1.1.1 2008/08/28 07:12:47 bish2310 Exp $
/////////////////////////////////////////////////////////////////////////////////

#ifdef __KERNEL__//[
#else //][!__KERNEL__
#include <stdio.h>
#include <stdlib.h>
#endif //]__KERNEL__

#include "s1v3s3xx_spi_api.h" 
#include "s1v3s3xx_isc_msgs.h"

#define MAX_RECEIVED_DATA_LEN	20

static volatile unsigned char	aucReceivedData[MAX_RECEIVED_DATA_LEN+2];
static volatile unsigned short	usMessageErrorCode;
static volatile unsigned short	usBlockedMessageID;
static volatile unsigned short	usSequenceStatus;
static volatile unsigned short gusReceivedDataLen;

/////////////////////////////////////////////////////////////////////////////////
//	SPI_Initialize
//
//	description:
//		Initialize SPI I/F
//
//	argument:
//		None
//
//	return:
//		None
/////////////////////////////////////////////////////////////////////////////////
void SPI_Initialize(void)
{
#ifdef __KERNEL__//[
	printk("%s() should be inplemeted in linux driver .\n",__func__);
#else //][!__KERNEL__
	/////////////////////////////////////
	// Initialise SPI register settings
	/////////////////////////////////////
	SpiDisable();

	// Disalbe interrupt configuration
	SpiAllIntDisable();
	// Configure SPI controller
	SpiConfig1(SPI_TNS_8BIT | SPI_CLK_MODE3 | SPI_CLK_DIV64 | SPI_MASTER);
	SpiConfig2(0);
	// Set transfer wait
	SpiWait(0);
	// Mask receive data
	SpiRcvMask(SPI_RCV_MASK8);

	SpiEnable();
#endif //]__KERNEL__
}

/////////////////////////////////////////////////////////////////////////////////
//	SPI_SendReceiveByte
//
//	description:
//		Send and receive 1 byte data by SPI
//
//	argument:
//		ucSendData - send 1 byte data
//
//	return:
//		received 1 byte data
/////////////////////////////////////////////////////////////////////////////////
static volatile int giSPI_SyncCnt = 0;
unsigned char SPI_SendReceiveByte(
	unsigned char	ucSendData)
{
	unsigned char	ucReceivedData = 0x0;
	int errno;
#ifdef __KERNEL__//[
	struct spi_transfer	t = {
			.rx_buf = &ucReceivedData,
			.len	= 1,
	};
	struct spi_message	m;
	unsigned char bSendDummy=0;

	if (ucSendData) {
		t.tx_buf = &ucSendData;
	}
	else {
		t.tx_buf = &bSendDummy;
	}

	if(!gs1v3s3xx_ddata) {
		printk(KERN_ERR"%s() s1v3s3xx device not ready !\n",__func__);
		return 0;
	}
	ASSERT(gs1v3s3xx_ddata->spi_dev);

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	//DBG_MSG("W[%02x]",ucSendData);
	errno = spi_sync(gs1v3s3xx_ddata->spi_dev, &m);
	if(errno<0) {
		printk(KERN_ERR"s1v3s3xx spi sync failed (%s)\n",strerror(errno));
		return 0;
	}
	//DBG_MSG("R[%02x]",ucReceivedData);
	giSPI_SyncCnt++;
#else //][!__KERNEL__

	// transfer and received byte

	// SPI Transfer
	// status check Trans empty?
	while ((SpiStatus() & SPI_STAT_TDEF) != 0x10) {};
	*SPI_TXD_ADDR = ucSendData;

	// SPI Receiver
	// status check received full?
	while ((SpiStatus() & SPI_STAT_RDFF) != 0x4){};
	ucReceivedData = (unsigned char)*SPI_RXD_ADDR;
#endif //]__KERNEL__

	return ucReceivedData;
}

/////////////////////////////////////////////////////////////////////////////////
//	SPI_SendMessage_simple
//
//	description:
//		Send message
//
//	argument:
//		pucSendMessage 		- pointer to send message data
//		iSendMessageLength	- length of send message data
//
//	return:
//		error code
/////////////////////////////////////////////////////////////////////////////////
int SPI_SendMessage_simple(
	unsigned char	*pucSendMessage,
	int				iSendMessageLength)
{
	int				i;

	if (pucSendMessage == NULL) {
		return SPIERR_NULL_PTR;
	}

	for (i = 0; i < iSendMessageLength; i++) {
		SPI_SendReceiveByte(*pucSendMessage++);
	}

	return SPIERR_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////
//	SPI_SendMessage
//
//	description:
//		Send message
//
//	argument:
//		pucSendMessage			- pointer to send message data
//		pusReceivedMessageID	- Received message ID
//
//	return:
//		error code
/////////////////////////////////////////////////////////////////////////////////
int SPI_SendMessage(
	unsigned char	*pucSendMessage,
	unsigned short	*pusReceivedMessageID)
{
	unsigned char	ucReceivedData;				// temporary received data
	unsigned short	usSendLength;
	int				iReceivedCounts = 0;		// byte counts of received message (except prefix "0x00 0xAA")
	unsigned short	usReceiveLength = 0;		// length of received message
#ifdef CHECKSUM
	unsigned char	ucCheckSum = 0;				// volue of check sum
	int				iSentCounts = 0;			// byte counts of sent message (including prefix "0x00 0xAA")
#endif


	if (pucSendMessage == NULL || pusReceivedMessageID == NULL) {
		return SPIERR_NULL_PTR;
	}

	*pusReceivedMessageID = 0xFFFF;

	usSendLength = pucSendMessage[3];
	usSendLength = (usSendLength << 8) | pucSendMessage[2];
#ifdef CHECKSUM
	usSendLength += HEADER_LEN + 1;
#else
	usSendLength += HEADER_LEN;
#endif

	giSPI_SyncCnt = 0;
	DBG_MSG("%s(),send len=%d\n",__func__,usSendLength);
	while (usSendLength > 0 || usReceiveLength > 0) {
		if (usSendLength == 0) {
			ucReceivedData = SPI_SendReceiveByte(0);
#ifdef CHECKSUM
		} else if (usSendLength == 1) {
			ucReceivedData = SPI_SendReceiveByte(ucCheckSum);
			usSendLength--;
#endif
		} else {
#ifdef CHECKSUM
			if (iSentCounts >= HEADER_LEN) {
				ucCheckSum += *pucSendMessage;
			}
			iSentCounts++;
#endif
			ucReceivedData = SPI_SendReceiveByte(*pucSendMessage++);
			usSendLength--;
		}

		// check message prefix(0xAA)
		if (usReceiveLength == 0 && ucReceivedData == 0xAA) {
			usReceiveLength = 2; // set the length of message length field
		}
		else if (usReceiveLength > 0) {
			if (iReceivedCounts < MAX_RECEIVED_DATA_LEN) {
				aucReceivedData[iReceivedCounts] = ucReceivedData;
			}
			if (iReceivedCounts == 1) {
				usReceiveLength = ucReceivedData;
				usReceiveLength = (usReceiveLength << 8) | aucReceivedData[iReceivedCounts-1];
				usReceiveLength -= 2; // set the length except message length field
			}
			iReceivedCounts++;
			usReceiveLength--;
		}
	}

	if (iReceivedCounts > 0) {
		unsigned short	usId;
		usId  = aucReceivedData[3];
		usId  = (usId << 8) | aucReceivedData[2];
		*pusReceivedMessageID = usId;
	}

	DBG_MSG("%s(), recv len=%d,sync count=%d\n",__func__,(int)usReceiveLength,giSPI_SyncCnt);
	return SPIERR_SUCCESS;
}


/////////////////////////////////////////////////////////////////////////////////
//	SPI_ReceiveMessage_simple
//
//	description:
//		receive message
//
//	argument:
//		pusReceivedMessageID - Received message ID
//
//	return:
//		error code
/////////////////////////////////////////////////////////////////////////////////
int SPI_ReceiveMessage_simple(
	unsigned short	*pusReceivedMessageID)
{
	int				i;
	unsigned char	aucHeader[2];
	unsigned char	usTmp;
	unsigned short	usLen = 0;
	unsigned short	usId = 0x0;

	if (pusReceivedMessageID == NULL) {
		return SPIERR_NULL_PTR;
	}

	*pusReceivedMessageID = 0xFFFF;

	aucHeader[0] = SPI_SendReceiveByte(0);
	aucHeader[1] = SPI_SendReceiveByte(0);

	while (1) {
		if (aucHeader[0] == 0x00 &&
			aucHeader[1] == 0xaa) {
			usTmp = SPI_SendReceiveByte(0);
			usLen = SPI_SendReceiveByte(0);
			usLen = (usLen << 8) + usTmp;
			usTmp = SPI_SendReceiveByte(0);
			usId  = SPI_SendReceiveByte(0);
			usId  = (usId << 8) + usTmp;
			for (i = 4; i < usLen; i++) {
				SPI_SendReceiveByte(0);
			}
			break;
		}
		aucHeader[0] = aucHeader[1];
		aucHeader[1] = SPI_SendReceiveByte(0);
	}

	*pusReceivedMessageID = usId;

	return SPIERR_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////
//	SPI_ReceiveMessage
//
//	description:
//		Receive message
//
//	argument:
//		pusReceivedMessageID - Received message ID
//		iReceivedMessageLength - Length of receive message
//								  0 : Use length in message
//	return:
//		Error code
/////////////////////////////////////////////////////////////////////////////////
// table used to establish transmit
const unsigned char aucIscVersionResp[LEN_ISC_VERSION_RESP] = {
	0x14, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01, 0x00, 
	0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 
};

int SPI_ReceiveMessage(
	unsigned short	*pusReceivedMessageID)
{
	unsigned short	i;
	unsigned char	aucHeader[2];
	unsigned char	usTmp;
	unsigned short	usLen = 0;
	unsigned short	usId = 0x0;
	int				iReceivedCounts = 0;
	int				iTimeOut = 0;

	gusReceivedDataLen = 0;

	if (pusReceivedMessageID == NULL) {
		return SPIERR_NULL_PTR;
	}

	*pusReceivedMessageID = 0xFFFF;
	usMessageErrorCode = 0;
	usBlockedMessageID = 0;

	aucHeader[0] = SPI_SendReceiveByte(0);
	aucHeader[1] = SPI_SendReceiveByte(0);

	giSPI_SyncCnt = 0;
	//DBG_MSG("%s(),header={0x%02x,0x%02x}\n",__func__,aucHeader[0],aucHeader[1]);
	while (1) {
		if (aucHeader[0] == 0x00 &&
			aucHeader[1] == 0xaa) {
			usTmp = aucReceivedData[iReceivedCounts++] = SPI_SendReceiveByte(0);
			usLen = aucReceivedData[iReceivedCounts++] = SPI_SendReceiveByte(0);
			usLen = (usLen << 8) + usTmp;
			usTmp = aucReceivedData[iReceivedCounts++] = SPI_SendReceiveByte(0);
			usId  = aucReceivedData[iReceivedCounts++] = SPI_SendReceiveByte(0);
			usId  = (usId << 8) + usTmp;
			for (i = 4; i < usLen; i++) {
				if (iReceivedCounts < MAX_RECEIVED_DATA_LEN) {
					aucReceivedData[iReceivedCounts++] = SPI_SendReceiveByte(0);
				} else {
					SPI_SendReceiveByte(0);
				}
			}
			break;
		}
		aucHeader[0] = aucHeader[1];
		aucHeader[1] = SPI_SendReceiveByte(0);
		//DBG_MSG("{0x%02x},",aucHeader[1]);
		if (iTimeOut >=  SPI_MSGRDY_TIMEOUT) {
			gusReceivedDataLen = (unsigned short)iReceivedCounts;
			DBG_MSG("%s() timeout !! sync count=%d\n",__func__,giSPI_SyncCnt);
			return SPIERR_TIMEOUT;
		}
		iTimeOut++;
	}
	*pusReceivedMessageID = usId;

	gusReceivedDataLen = (unsigned short)iReceivedCounts;
	DBG_MSG("%s() sync count=%d,ReceivedCounts=%d,%d\n",__func__,
			giSPI_SyncCnt,iReceivedCounts,gusReceivedDataLen);
	// check RESP or IND message.
	switch (usId) {
	case ID_ISC_RESET_RESP://4
	case ID_ISC_AUDIO_PAUSE_IND://4
	case ID_ISC_PMAN_STANDBY_EXIT_IND://4
	case ID_ISC_UART_CONFIG_RESP://4
	case ID_ISC_UART_RCVRDY_IND://4
	case ID_ISC_AUDIODEC_READY_IND://0x11->17
		break;
	case ID_ISC_TEST_RESP://6
	case ID_ISC_ERROR_IND://6
	case ID_ISC_AUDIO_CONFIG_RESP://6
	case ID_ISC_AUDIO_VOLUME_RESP://6
	case ID_ISC_AUDIO_MUTE_RESP://6
	case ID_ISC_PMAN_STANDBY_ENTRY_RESP://6
	case ID_ISC_AUDIODEC_CONFIG_RESP://6
	case ID_ISC_AUDIODEC_DECODE_RESP://6
	case ID_ISC_AUDIODEC_PAUSE_RESP://6
	case ID_ISC_AUDIODEC_ERROR_IND://6
	case ID_ISC_SEQUENCER_CONFIG_RESP://6
	case ID_ISC_SEQUENCER_START_RESP://6
	case ID_ISC_SEQUENCER_STOP_RESP://6
	case ID_ISC_SEQUENCER_PAUSE_RESP://6
	case ID_ISC_SEQUENCER_ERROR_IND://6
	case ID_ISC_AUDIODEC_STOP_RESP://20
		usMessageErrorCode = aucReceivedData[5];
		usMessageErrorCode = (usMessageErrorCode << 8) + aucReceivedData[4];
		break;
	case ID_ISC_SEQUENCER_STATUS_IND://6
		usSequenceStatus = aucReceivedData[5];
		usSequenceStatus = (usSequenceStatus << 8) + aucReceivedData[4];
		break;
	case ID_ISC_VERSION_RESP://20
		for (i = 0; i < LEN_ISC_VERSION_RESP; i++) {
			if (aucReceivedData [i] != aucIscVersionResp[i]) {
				return SPIERR_ISC_VERSION_RESP;
				break;
			}
		}
		break;
	case ID_ISC_MSG_BLOCKED_RESP://8
		usBlockedMessageID = aucReceivedData[5];
		usBlockedMessageID = (usBlockedMessageID << 8) + aucReceivedData[4];
		usMessageErrorCode = aucReceivedData[7];
		usMessageErrorCode = (usMessageErrorCode << 8) + aucReceivedData[6];
		break;
	default:
		return SPIERR_RESERVED_MESSAGE_ID;
		break;
	}

	if (usMessageErrorCode != 0) {
		return SPIERR_GET_ERROR_CODE;
	}

	return SPIERR_SUCCESS;
}

///////////////////////////////////////////////////////////////////////
//	GPIO_ControlChipSelect
//
//	description:
//		NSCSS control for Device CS(Chip Select) High/Low control
//
//	argument:
//		iValue		Signal value High:1  Low:0
///////////////////////////////////////////////////////////////////////
void GPIO_ControlChipSelect(int iValue)
{
#ifdef __KERNEL__//[
#else //][!__KERNEL__

	if (iValue) {
		SetCsHigh();
	} else {
		SetCsLow();
	}
#endif //]__KERNEL__
}


///////////////////////////////////////////////////////////////////////
//	GPIO_ControlStandby
//
//	description:
//		STAND-BY control for Device STBY(Stand-by) High/Low control
//
//	argument:
//		iValue		Signal value High:1  Low:0
///////////////////////////////////////////////////////////////////////
void GPIO_ControlStandby(int iValue)
{
#ifdef __KERNEL__//[
#else //][!__KERNEL__
	
	if (iValue) {
		SetStanbyHigh();
	} else {
		SetStanbyLow();
	}
#endif //]__KERNEL__
}

///////////////////////////////////////////////////////////////////////
//	GPIO_ControlMute
//
//	description:
//		MUTE control for Device MUTE control
//
//	argument:
//		iValue		Signal value Mute  enable:1  disable:0
///////////////////////////////////////////////////////////////////////
void GPIO_ControlMute(int iValue)
{
#ifdef __KERNEL__//[
#else //][!__KERNEL__
	
	if (iValue) {
		SetMuteHigh();
	} else {
		SetMuteLow();
	}
#endif //]__KERNEL__
}

///////////////////////////////////////////////////////////////////////
// get blocked errorcode field in ISC_*_RESP or ISC_*_IND
///////////////////////////////////////////////////////////////////////
unsigned short GetMessageErrorCode(void)
{
	return usMessageErrorCode;
}

///////////////////////////////////////////////////////////////////////
// get blocked message id field in ISC_MSG_BLOCKED_RESP
///////////////////////////////////////////////////////////////////////
unsigned short GetBlockedMessageID(void)
{
	return usBlockedMessageID;
}

///////////////////////////////////////////////////////////////////////
// get sequnece status field in ISC_SEQUENCER_STATUS_IND
///////////////////////////////////////////////////////////////////////
unsigned short GetSequenceStatus(void)
{
	return usSequenceStatus;
}

unsigned short GetMessageReceivedLen(void)
{
	return gusReceivedDataLen;
}

void DumpReceivedMessages(void)
{
	int iMsgLen = (int)GetMessageReceivedLen();
	int i;


	if(iMsgLen<=0) {
		printk("no messages received !\n");
		return;
	}

	printk("Received Messages (%d bytes) = {\n",iMsgLen);
	for(i=0;i<iMsgLen;i++) {
			if(i>=MAX_RECEIVED_DATA_LEN) {
			printk("Warning : Received length >=%d\n",MAX_RECEIVED_DATA_LEN);
			break;
		}
		printk("0x%02x,",aucReceivedData[i]);
	}
	printk("\n}\n");
	printk("message len=0x%02x%02x",aucReceivedData[0],aucReceivedData[1]);
	printk("message id=0x%02x%02x",aucReceivedData[2],aucReceivedData[3]);

}

