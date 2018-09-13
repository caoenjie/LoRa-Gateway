#include <stdio.h>
#include <stdint.h>
#include <stdbool.h> 
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <errno.h>
#include "platform.h"
//#include "LPC11xx.h"                                                    /* LPC11xx����Ĵ���            */
#include "loragw_spi.h"



void *spi_target = NULL; 	/*! generic pointer to the SPI device */
uint8_t spi_mux_mode = 0; 		/*! current SPI mux mode used */
uint8_t spi_mux_target = 0x0;


int SX1276Connect()
{
	return lgw_spi_open(&spi_target);
}

int SX1276Disconnect()
{
	return lgw_spi_close(spi_target);
}

void SX1276WriteBuffer( uint8_t address, uint8_t *data, uint16_t size )
{
	lgw_spi_wb(spi_target, spi_mux_mode, spi_mux_target, address, data, size);
}

void SX1276ReadBuffer( uint8_t address, uint8_t *data, uint16_t size )
{
	lgw_spi_rb(spi_target, spi_mux_mode, spi_mux_target, address, data, size);

}


void SX1276Write( uint8_t address, uint8_t data )
{
	lgw_spi_w(spi_target, spi_mux_mode, spi_mux_target, address, data);
}

void SX1276Read( uint8_t address, uint8_t *data )
{
	lgw_spi_r(spi_target, spi_mux_mode, spi_mux_target, address, data);
}



void SX1276WriteFifo( uint8_t *buffer, uint16_t size )
{
	lgw_spi_wb(spi_target, spi_mux_mode, spi_mux_target, 0x0, buffer, size);
}

void SX1276ReadFifo( uint8_t *buffer, uint16_t size )
{
	lgw_spi_rb(spi_target, spi_mux_mode, spi_mux_target, 0x0, buffer, size);
}

int SX1276ReadDio0(void)
{
	int fd ;
	fd = open("/sys/class/gpio/gpio39/value",O_RDONLY);
	if(fd<0)
	{
//		printf("open gpio39 failed\n");
		perror("open gpio39:");
		return 0;
	}
	char buf[1];
	read(fd,buf,1);
	if(buf[0] == '1')
	{
//		printf("read dio0  = 1\n");
		close(fd);
		return 1;
	}
	else{
		close(fd);
		return 0;

	}


}

