/*
 * esp32_rmt_remotes.h
 *
 */

#pragma once

#include "stdint.h"
#ifndef ESP32_RMT_REMOTES_H
#define ESP32_RMT_REMOTES_H

#if defined(CONFIG_TWATCH_V1) || defined(CONFIG_TWATCH_V3)
  #define TX_PIN_SSSS GPIO_NUM_13
#elif CONFIG_TWATCH_V2
  #define TX_PIN_SSSS GPIO_NUM_2
#endif

/* Available remote protocols */
#define SEND_NEC			1
#define RECEIVE_NEC			0
#define SEND_SAMSUNG		1
#define RECEIVE_SAMSUNG		0
#define SEND_RC5			1
#define RECEIVE_RC5			0
#define SEND_SONY           1
#define RECEIVE_SONY        0
#define SEND_RAW            1

#ifdef SEND_NEC
void rmtlib_nec_send(unsigned long data);
#endif

#ifdef RECEIVE_NEC
void rmtlib_nec_receive();
#endif

#ifdef SEND_SAMSUNG
void rmtlib_samsung_send(unsigned long data);
#endif

#ifdef RECEIVE_SAMSUNG
void rmtlib_samsung_receive();
#endif

#ifdef SEND_RC5
void rmtlib_rc5_send(unsigned long data);
#endif

#ifdef RECEIVE_RC5
void rmtlib_rc5_receive();
#endif

#ifdef SEND_SONY
void rmtlib_sony_send(unsigned long data);
#endif

#ifdef SEND_RAW
void rmtlib_raw_send(unsigned int frequency, int data[], int data_len);
#endif


#endif /* ESP32_RMT_REMOTES_H */
