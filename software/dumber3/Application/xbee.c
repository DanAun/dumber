/*
 * xbee.c
 *
 *  Created on: Sep 12, 2022
 *      Author: dimercur
 */


#include "xbee.h"
#include "semphr.h"
#include <stdlib.h>
#include <string.h>
#include "stm32l0xx_ll_usart.h"

extern UART_HandleTypeDef hlpuart1;
extern DMA_HandleTypeDef hdma_lpuart1_tx;
extern DMA_HandleTypeDef hdma_lpuart1_rx;

/***** API2 escaped char *****/
#define XBEE_API_ESCAPE_CHAR	0x7D
#define XBEE_API_START_OF_FRAME 0x7E
#define XBEE_API_XON			0x11
#define XBEE_API_XOFF			0x13

#define XBEE_ENDING_CHAR		'\r'

/***** TX part *****/
void XBEE_TxHandlerThread(void* params);
int XBEE_SendData(char* data);

/* Stuff for Xbee TX task */
StaticTask_t xTaskXbeeTXHandler;
StackType_t xStackXbeeTXHandler[ STACK_SIZE ];
TaskHandle_t xHandleXbeeTXHandler = NULL;

uint8_t txBuffer[XBEE_TX_BUFFER_MAX_LENGTH]={0};
uint16_t txIndex;
uint16_t txRemainingData;
uint16_t txDataToSend;

/***** RX part *****/
void XBEE_RxThread(void* params);

SemaphoreHandle_t xHandleSemaphoreRX = NULL;
StaticSemaphore_t xSemaphoreRx;

/* Stuff for Xbee RX task */
StaticTask_t xTaskXbeeRX;
StackType_t xStackXbeeRX[ STACK_SIZE ];
TaskHandle_t xHandleXbeeRX = NULL;

uint8_t rxBuffer[XBEE_RX_BUFFER_MAX_LENGTH]={0};
//uint8_t rxWaitForACK =0;
uint8_t rxPhase;
uint16_t rxCmdLength;
uint16_t rxDataToReceive;
uint16_t rxIndex;

#define XBEE_RX_PHASE_SOF		0
#define XBEE_RX_PHASE_HEADER 	1
#define XBEE_RX_PHASE_BODY		2

/****** TX part ******/
SemaphoreHandle_t xHandleSemaphoreTX = NULL;
//SemaphoreHandle_t xHandleSemaphoreTX_ACK = NULL;
StaticSemaphore_t xSemaphoreTX;
//StaticSemaphore_t xSemaphoreTX_ACK;

void XBEE_Init(void) {
	xHandleSemaphoreTX = xSemaphoreCreateBinaryStatic( &xSemaphoreTX );
	//xHandleSemaphoreTX_ACK = xSemaphoreCreateBinaryStatic( &xSemaphoreTX_ACK );
	xSemaphoreGive(xHandleSemaphoreTX);
	//xSemaphoreTake(xHandleSemaphoreTX_ACK);

	xHandleSemaphoreRX = xSemaphoreCreateBinaryStatic( &xSemaphoreRx );

	/* Create the task without using any dynamic memory allocation. */
	xHandleXbeeRX = xTaskCreateStatic(
			XBEE_RxThread,       /* Function that implements the task. */
			"XBEE Rx",          /* Text name for the task. */
			STACK_SIZE,      /* Number of indexes in the xStack array. */
			NULL,    /* Parameter passed into the task. */
			PriorityXbeeRX,/* Priority at which the task is created. */
			xStackXbeeRX,          /* Array to use as the task's stack. */
			&xTaskXbeeRX);  /* Variable to hold the task's data structure. */
	vTaskResume(xHandleXbeeRX);

	/* Create the task without using any dynamic memory allocation. */
	xHandleXbeeTXHandler = xTaskCreateStatic(
			XBEE_TxHandlerThread,       /* Function that implements the task. */
			"XBEE Tx",          /* Text name for the task. */
			STACK_SIZE,      /* Number of indexes in the xStack array. */
			NULL,    /* Parameter passed into the task. */
			PriorityXbeeTX,/* Priority at which the task is created. */
			xStackXbeeTXHandler,          /* Array to use as the task's stack. */
			&xTaskXbeeTXHandler);  /* Variable to hold the task's data structure. */
	vTaskResume(xHandleXbeeTXHandler);

	/* Enable Xbee */
	HAL_GPIO_WritePin(XBEE_RESET_GPIO_Port, XBEE_RESET_Pin, GPIO_PIN_SET);
}

/**** Support functions ****/

/**** TX Part *****/

void XBEE_TxHandlerThread(void* params) {
	MESSAGE_Typedef msg;

	while (1) {
		msg = MESSAGE_ReadMailbox(XBEE_Mailbox);

		if (msg.id == MSG_ID_XBEE_ANS) {
			XBEE_SendData((char*)msg.data); // block function during send

			free(msg.data);
		}
	}
}

/**
 * Send data. Create a transmission frame, add escape char to data and send it over UART
 *
 * \param data raw data to send
 * \return status of decoding: XBEE_OK if decoding is successful,
 *                             XBEE_TX_ERROR in case of sending error,
 *                             XBEE_TX_TIMEOUT in case semaphore takes too long
 */
int XBEE_SendData(char* data) {
	BaseType_t state;
	int status = XBEE_OK;

	// Prevents successive calls to overlap
	state = xSemaphoreTake(xHandleSemaphoreTX, pdMS_TO_TICKS(XBEE_TX_SEMAPHORE_WAIT)); // wait max 500 ms (to avoid interlocking)

	if (state != pdFALSE) { /* test semaphore take answer
	                           if answer is false, it means timeout appends
	                           We should probably reset something in "else" branch */

		strncpy((char*)txBuffer,data,XBEE_TX_BUFFER_MAX_LENGTH-1);
		txBuffer[XBEE_TX_BUFFER_MAX_LENGTH-1]=0;
		txRemainingData = strlen((char*)txBuffer);

		if (txRemainingData!=0) {
			txIndex =1;
			txRemainingData=txRemainingData-1;

			LL_USART_TransmitData8(hlpuart1.Instance, txBuffer[0]);
			LL_USART_EnableIT_TXE(hlpuart1.Instance); // enable TX Interrupt
		}
	} else status= XBEE_TX_TIMEOUT;

	return status;
}

void XBEE_TX_IRQHandler(void) {
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	if (txRemainingData==0) { // No more data, disable TXE bit
		LL_USART_DisableIT_TXE(hlpuart1.Instance);
		xSemaphoreGiveFromISR( xHandleSemaphoreTX, &xHigherPriorityTaskWoken );
	} else {
		LL_USART_TransmitData8(hlpuart1.Instance, txBuffer[txIndex]);
		txIndex++;
		txRemainingData--;
	}

	if (xHigherPriorityTaskWoken) {
		/* If xHigherPriorityTaskWoken is now set to pdTRUE then a context switch
					    should be performed to ensure the interrupt returns directly to the highest
					    priority task.  The macro used for this purpose is dependent on the port in
					    use and may be called portEND_SWITCHING_ISR(). */
		portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
	}
}

/***** Rx Part *****/

/**
 * Reception thread. Wait for incoming frame, process it and send message to application
 *
 * \param params not used
 */
void XBEE_RxThread(void* params) {
	//XBEE_INCOMING_FRAME *incomingFrame;
	char* incomingData;
	rxCmdLength=0;
	rxIndex=0;

	while (HAL_UART_Receive_IT(&hlpuart1, rxBuffer, 1)!= HAL_OK); // try starting reception of frame
	LL_USART_Disable(hlpuart1.Instance);
	LL_USART_DisableOverrunDetect(hlpuart1.Instance);
	LL_USART_Enable(hlpuart1.Instance);

	// endless task
	while (1) {
		if (xSemaphoreTake(xHandleSemaphoreRX, portMAX_DELAY)==pdTRUE) { // wait forever

			incomingData = (char*)malloc(rxCmdLength+1); // +1 for ending zero !
			strncpy (incomingData, (char*)rxBuffer, XBEE_RX_BUFFER_MAX_LENGTH);

			rxCmdLength=0; // reset counters for next command
			rxIndex=0;

			MESSAGE_SendMailbox(APPLICATION_Mailbox, MSG_ID_XBEE_CMD, (QueueHandle_t)0x0, (void*)incomingData);
		}
	}
}

void XBEE_RX_IRQHandler(void) {
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	uint8_t data;

	data = LL_USART_ReceiveData8(hlpuart1.Instance); // lecture de l'octet reçu

	if (data != XBEE_ENDING_CHAR) { // end of command not received
		rxBuffer[rxIndex] = data;
		rxIndex++;
		if (rxIndex>=XBEE_RX_BUFFER_MAX_LENGTH)
			rxIndex=0;

		rxCmdLength++;
		if (rxCmdLength>=XBEE_RX_BUFFER_MAX_LENGTH)
			rxCmdLength=0;
	} else { // end of command  received
		rxBuffer[rxIndex] = 0; // ending zero for C string
		xSemaphoreGiveFromISR( xHandleSemaphoreRX, &xHigherPriorityTaskWoken ); /* send event to receive task to process received task */
	}

	if (xHigherPriorityTaskWoken) {
		/* If xHigherPriorityTaskWoken is now set to pdTRUE then a context switch
				    should be performed to ensure the interrupt returns directly to the highest
				    priority task.  The macro used for this purpose is dependent on the port in
				    use and may be called portEND_SWITCHING_ISR(). */
		portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
	}
}

/**
 * DMA IRQ handler for reception. Receive a complete frame send send event to sending frame in case of acknowledge frame or to receive task otherwise
 *
 * \param UartHandle not used
 */
void LPUART1_IRQHandler(void) {

	if (LL_USART_IsActiveFlag_RXNE(hlpuart1.Instance)) {
		XBEE_RX_IRQHandler();
	} else if ((LL_USART_IsEnabledIT_TXE(hlpuart1.Instance)) && (LL_USART_IsActiveFlag_TXE(hlpuart1.Instance))) {
		XBEE_TX_IRQHandler();
	} else {
		if ((LL_USART_IsEnabledIT_TC(hlpuart1.Instance)) && (LL_USART_IsActiveFlag_TC(hlpuart1.Instance)))
			LL_USART_DisableIT_TC(hlpuart1.Instance);
		else if ((LL_USART_IsEnabledIT_IDLE(hlpuart1.Instance)) && (LL_USART_IsActiveFlag_IDLE(hlpuart1.Instance)))
			LL_USART_ClearFlag_IDLE(hlpuart1.Instance);
		else {
			LL_USART_ClearFlag_ORE(hlpuart1.Instance);
			LL_USART_ClearFlag_FE(hlpuart1.Instance);
			LL_USART_ClearFlag_PE(hlpuart1.Instance);
			LL_USART_ClearFlag_NE(hlpuart1.Instance);
			LL_USART_DisableIT_ERROR(hlpuart1.Instance);
			LL_USART_DisableIT_PE(hlpuart1.Instance);
		}
	}
}
