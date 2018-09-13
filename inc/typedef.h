/*
 * typedef.h
 *
 *  Created on: 2018年5月28日
 *      Author: enjie
 */

#ifndef INC_TYPEDEF_H_
#define INC_TYPEDEF_H_
//#include "loragw_spi.h"
#define DEBUG_SPI  0
#if DEBUG_SPI == 1
    #define DEBUG_MSG(str)                fprintf(stderr, str)
    #define DEBUG_PRINTF(fmt, args...)    fprintf(stderr,"%s:%d: "fmt, __FUNCTION__, __LINE__, args)
    #define CHECK_NULL(a)                if(a==NULL){fprintf(stderr,"%s:%d: ERROR: NULL POINTER AS ARGUMENT\n", __FUNCTION__, __LINE__);return LGW_SPI_ERROR;}
#else
    #define DEBUG_ERROR(str)				fprintf(stderr, str)
 	#define DEBUG_MSG(str)
    #define DEBUG_PRINTF(fmt, args...)
    #define CHECK_NULL(a)                if(a==NULL){return LGW_SPI_ERROR;}
#endif

#define SUCCESS 0

//#define MSG(args...) printf(args) /* message that is destined to the user */


#endif /* INC_TYPEDEF_H_ */
