/////////////////////////////////////////////////////////////////////////////////
// File Name: spi_api.h
//
// Description: Header file for API specification
//
// Author: SEIKO EPSON
//
// History: 2008/04/18 1st. design
//
// Copyright(c) SEIKO EPSON CORPORATION 2008, All rights reserved.
//
// $Id: spi_api.h,v 1.1.1.1 2008/08/28 07:12:47 bish2310 Exp $
/////////////////////////////////////////////////////////////////////////////////

#ifndef	_SPI_API_H_
#define	_SPI_API_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __KERNEL__//[
#else //][!__KERNEL__
#include "reg.h" 


//////////////////////////////////////////////////////////////////////////////
//	Define
//////////////////////////////////////////////////////////////////////////////

// SPI I/F Definition

// SpiConfig1()
#define	SPI_TNS_8BIT		0x1c00
#define	SPI_TNS_16BIT		0x3c00
#define	SPI_TNS_32BIT		0x7c00

#define	SPI_CLK_MODE0		0x0000
#define	SPI_CLK_MODE1		0x0100
#define	SPI_CLK_MODE2		0x0200
#define	SPI_CLK_MODE3		0x0300

#define	SPI_CLK_DIV4		0x0000
#define	SPI_CLK_DIV8		0x0010
#define	SPI_CLK_DIV16		0x0020
#define	SPI_CLK_DIV32		0x0030
#define	SPI_CLK_DIV64		0x0040
#define	SPI_CLK_DIV128		0x0050
#define	SPI_CLK_DIV256		0x0060
#define	SPI_CLK_DIV512		0x0070

#define	SPI_MASTER			0x0002
#define	SPI_SLAVE			0x0000

// SpiRcvMask()
#define	SPI_RCV_MASK8		0x00001c02
#define	SPI_RCV_MASK16		0x00003c02
#define	SPI_RCV_MASK32		0x00000000

// SpiStatus()
#define	SPI_BSYF			0x40
#define	SPI_TDEF			0x10
#define	SPI_RDOF			0x08
#define	SPI_RDFF			0x04

//SpiIntSettings()
#define	SPI_TEIE			0x10
#define	SPI_ROIE			0x08
#define	SPI_RFIE			0x04
#define	SPI_MIRQ			0x02
#define	SPI_IRQE			0x01

//////////////////////////////////////////////////////////////////////////////
//	Macro function
//////////////////////////////////////////////////////////////////////////////
#define	SpiEnable()			(*SPI_CTL1_ADDR |= 1)
#define	SpiDisable()		(*SPI_CTL1_ADDR &= ~1)

#define	SpiIntEnable()		(*SPI_INT_ADDR |= 1)
#define	SpiIntDisable()		(*SPI_INT_ADDR &= ~1)
#define	SpiAllIntDisable()	(*SPI_INT_ADDR = 0x00000000)
#define SpiIntSettings(x)	(*SPI_INT_ADDR = (x))

#define SpiConfig1(x)		(*SPI_CTL1_ADDR = (x))
#define SpiConfig2(x)		(*SPI_CTL2_ADDR = (x))

#define	SpiWait(x)			(*SPI_WAIT_ADDR = (x))
#define SpiRcvMask(x)		(*SPI_RXMK_ADDR = (x))

#define	SpiStatus()			(*SPI_STAT_ADDR)
#define	SpiRcvData()		(*SPI_RXD_ADDR)
#define	SpiTnsData(x)		(*SPI_TXD_ADDR = (x))

#define SetStanbyHigh()		(*GPIO_P9_P9D_ADDR |= 0x02)
#define SetStanbyLow()		(*GPIO_P9_P9D_ADDR &= ~0x02)
#define SetCsHigh()			(*GPIO_P9_P9D_ADDR |= 0x04)
#define SetCsLow()			(*GPIO_P9_P9D_ADDR &= ~0x04)
#define SetMuteHigh()		(*GPIO_P9_P9D_ADDR |= 0x08)
#define SetMuteLow()		(*GPIO_P9_P9D_ADDR &= ~0x08)
#define SetHwResetHigh()	(*GPIO_P9_P9D_ADDR |= 0x10)
#define SetHwResetLow()		(*GPIO_P9_P9D_ADDR &= ~0x10)
#endif //]__KERNEL__


// SPI transfer
//#define	SPI_MSGRDY_TIMEOUT	10000
#define	SPI_MSGRDY_TIMEOUT	5000

// Error definition for API function
#define SPIERR_TIMEOUT				1
#define SPIERR_SUCCESS				0
#define SPIERR_NULL_PTR				-1
#define SPIERR_GET_ERROR_CODE		-2
#define SPIERR_RESERVED_MESSAGE_ID	-3
#define SPIERR_ISC_VERSION_RESP		-4

// Definition of checksum function
//#define CHECKSUM


//////////////////////////////////////////////////////////////////////////////
//	Prototype definition
//////////////////////////////////////////////////////////////////////////////
void SPI_Initialize(void);

unsigned char SPI_SendReceiveByte(
	unsigned char	ucSendData);

int SPI_SendMessage(
	unsigned char	*pucSendMessage,
	unsigned short	*pusReceivedMessageID);

int SPI_ReceiveMessage(
	unsigned short	*pusReceivedMessageID);

int SPI_SendMessage_simple(
	unsigned char	*pucSendMessage,
	int				iSendMessageLength);

int SPI_ReceiveMessage_simple(
	unsigned short	*pusReceivedMessageID);

void GPIO_ControlChipSelect(
	int				iValue);

void GPIO_ControlStandby(
	int				iValue);

void GPIO_ControlMute(
	int				iValue);

// function for test
unsigned short GetMessageErrorCode(void);
unsigned short GetBlockedMessageID(void);
unsigned short GetSequenceStatus(void);

#ifdef __cplusplus
}
#endif

#endif //!_SPI_API_H_

