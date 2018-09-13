
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <error.h>
#include <netinet/in.h>
#include <netdb.h>          /* gai_strerror */
#include <errno.h>          /* error messages */
#include <pthread.h>

#include "loragw_spi.h"
#include "platform.h"
//#include "radio.h"
#include "sx1276-LoRa.h"
#include "sx1276-Hal.h"
#include "sx1276.h"
#include "typedef.h"
#include "base64.h"
#include "trace.h"
#include "jitqueue.h"
#include "timersync.h"
#include "parson.h"
#include "loragw_hal.h"

static const int CHANNEL = 0;

uint8_t currentMode = 0x81;

uint8_t ucRxbuf[255] = {0};
uint8_t ucTxbuf[255] = {0};
//char message[256];
char b64[256];

#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define STRINGIFY(x)    #x
#define STR(x)          STRINGIFY(x)
#define PROTOCOL_VERSION  2

#define PKT_PUSH_DATA	 0
#define PKT_PUSH_ACK 	 1
#define PKT_PULL_DATA	 2
#define PKT_PULL_RESP	 3
#define PKT_PULL_ACK 	 4
#define PKT_TX_ACK   	 5


#define TX_BUFF_SIZE  2048
#define STATUS_SIZE	  1024
#define DEFAULT_STAT        30          /* default time interval for statistics */
#define FETCH_SLEEP_MS      10          /* nb of ms waited when a fetch return no packets */
#define PUSH_TIMEOUT_MS     100
#define PULL_TIMEOUT_MS     200
#define DEFAULT_KEEPALIVE   5           /* default time interval for downstream keep-alive packet */

#define MIN_LORA_PREAMB 6 /* minimum Lora preamble length for this application */
#define STD_LORA_PREAMB 8
#define MIN_FSK_PREAMB  3 /* minimum FSK preamble length for this application */
#define STD_FSK_PREAMB  5

#define DEFAULT_BEACON_FREQ_HZ      869525000
#define DEFAULT_BEACON_FREQ_NB      1
#define DEFAULT_BEACON_FREQ_STEP    0
#define DEFAULT_BEACON_DATARATE     10
#define DEFAULT_BEACON_BW_HZ        500000
#define DEFAULT_BEACON_POWER        14
#define DEFAULT_BEACON_INFODESC     0

/* loragw_hal.c*/
#define TX_METADATA_NB      16
#define MIN_LORA_PREAMBLE   6
#define STD_LORA_PREAMBLE   8
#define SET_PPM_ON(bw,dr)   (((bw == BW_125KHZ) && ((dr == DR_LORA_SF11) || (dr == DR_LORA_SF12))) || ((bw == BW_250KHZ) && (dr == DR_LORA_SF12)))


uint8_t receivedbytes;


struct sockaddr_in si_other;
int s, slen=sizeof(si_other);
struct ifreq ifr;


static pthread_mutex_t mx_stat_rep = PTHREAD_MUTEX_INITIALIZER; /* control access to the status report */
static bool report_ready = false; /* true when there is a new report to send to the server */
static char status_report[STATUS_SIZE]; /* status report as a JSON object */

/* beacon parameters */
static uint32_t beacon_period = 0; /* set beaconing period, must be a sub-multiple of 86400, the nb of sec in a day */
static uint32_t beacon_freq_hz = DEFAULT_BEACON_FREQ_HZ; /* set beacon TX frequency, in Hz */
static uint8_t beacon_freq_nb = DEFAULT_BEACON_FREQ_NB; /* set number of beaconing channels beacon */
static uint32_t beacon_freq_step = DEFAULT_BEACON_FREQ_STEP; /* set frequency step between beacon channels, in Hz */
static uint8_t beacon_datarate = DEFAULT_BEACON_DATARATE; /* set beacon datarate (SF) */
static uint32_t beacon_bw_hz = DEFAULT_BEACON_BW_HZ; /* set beacon bandwidth, in Hz */
static int8_t beacon_power = DEFAULT_BEACON_POWER; /* set beacon TX power, in dBm */
static uint8_t beacon_infodesc = DEFAULT_BEACON_INFODESC; /* set beacon information descriptor */

/* measurements to establish statistics */
static pthread_mutex_t mx_meas_up = PTHREAD_MUTEX_INITIALIZER; /* control access to the upstream measurements */
static uint32_t meas_nb_rx_rcv = 0; /* count packets received */
static uint32_t meas_nb_rx_ok = 0; /* count packets received with PAYLOAD CRC OK */
static uint32_t meas_nb_rx_bad = 0; /* count packets received with PAYLOAD CRC ERROR */
static uint32_t meas_nb_rx_nocrc = 0; /* count packets received with NO PAYLOAD CRC */
static uint32_t meas_up_pkt_fwd = 0; /* number of radio packet forwarded to the server */
static uint32_t meas_up_network_byte = 0; /* sum of UDP bytes sent for upstream traffic */
static uint32_t meas_up_payload_byte = 0; /* sum of radio payload bytes sent for upstream traffic */
static uint32_t meas_up_dgram_sent = 0; /* number of datagrams sent for upstream traffic */
static uint32_t meas_up_ack_rcv = 0; /* number of datagrams acknowledged for upstream traffic */

static pthread_mutex_t mx_meas_dw = PTHREAD_MUTEX_INITIALIZER; /* control access to the downstream measurements */
static uint32_t meas_dw_pull_sent = 0; /* number of PULL requests sent for downstream traffic */
static uint32_t meas_dw_ack_rcv = 0; /* number of PULL requests acknowledged for downstream traffic */
static uint32_t meas_dw_dgram_rcv = 0; /* count PULL response packets received for downstream traffic */
static uint32_t meas_dw_network_byte = 0; /* sum of UDP bytes sent for upstream traffic */
static uint32_t meas_dw_payload_byte = 0; /* sum of radio payload bytes sent for upstream traffic */
static uint32_t meas_nb_tx_ok = 0; /* count packets emitted successfully */
static uint32_t meas_nb_tx_fail = 0; /* count packets were TX failed for other reasons */
static uint32_t meas_nb_tx_requested = 0; /* count TX request from server (downlinks) */
static uint32_t meas_nb_tx_rejected_collision_packet = 0; /* count packets were TX request were rejected due to collision with another packet already programmed */
static uint32_t meas_nb_tx_rejected_collision_beacon = 0; /* count packets were TX request were rejected due to collision with a beacon already programmed */
static uint32_t meas_nb_tx_rejected_too_late = 0; /* count packets were TX request were rejected because it is too late to program it */
static uint32_t meas_nb_tx_rejected_too_early = 0; /* count packets were TX request were rejected because timestamp is too much in advance */
static uint32_t meas_nb_beacon_queued = 0; /* count beacon inserted in jit queue */
static uint32_t meas_nb_beacon_sent = 0; /* count beacon actually sent to concentrator */
static uint32_t meas_nb_beacon_rejected = 0; /* count beacon rejected for queuing */

/* auto-quit function */
static uint32_t autoquit_threshold = 0; /* enable auto-quit after a number of non-acknowledged PULL_DATA (0 = disabled)*/

/* Just In Time TX scheduling */
static struct jit_queue_s jit_queue;

/* Gateway specificities */
static int8_t antenna_gain = 0;

/* TX capabilities */
 /* TX gain table */


static uint32_t tx_freq_min[LGW_RF_CHAIN_NB] = {470000000 , 470000000};  /* lowest frequency supported by TX chain */
static uint32_t tx_freq_max[LGW_RF_CHAIN_NB] = {510000000 , 510000000}; /* highest frequency supported by TX chain */

/* network sockets */
static int sock_up; /* socket for upstream traffic */
static int sock_down; /* socket for downstream traffic */

/* network protocol variables */
static struct timeval push_timeout_half = {0, (PUSH_TIMEOUT_MS * 500)}; /* cut in half, critical for throughput */
static struct timeval pull_timeout = {0, (PULL_TIMEOUT_MS * 1000)}; /* non critical for throughput */


/* hardware access control and correction */
pthread_mutex_t mx_concent = PTHREAD_MUTEX_INITIALIZER; /* control access to the concentrator */
static pthread_mutex_t mx_xcorr = PTHREAD_MUTEX_INITIALIZER; /* control access to the XTAL correction */
static bool xtal_correct_ok = false; /* set true when XTAL correction is stable enough */
static double xtal_correct = 1.0;

uint32_t cp_nb_rx_rcv;
uint32_t cp_nb_rx_ok;
uint32_t cp_nb_rx_bad;
uint32_t cp_nb_rx_nocrc;
uint32_t cp_up_pkt_fwd;

volatile bool exit_sig = false;
volatile bool quit_sig = false;


// Set spreading factor (SF7 - SF12)
enum sf_t { SF7=7, SF8, SF9, SF10, SF11, SF12 };
int  sf = SF7;

// Set center frequency
uint32_t  freq = 470300000; // in Mhz! (868.1)
// Set location
float lat=0.0;
float lon=0.0;
int   alt=0;

/* Informal status fields */


// define servers
// TODO: use host names and dns
//#define SERVER1 "192.168.1.100"    // The Things Network: gw01.rimecloud.com
////#define SERVER2 "192.168.1.10"      // local
//#define PORT 1700                   // The port on which to send data
#define DEFAULT_SERVER      192.168.1.100   /* hostname also supported */
#define DEFAULT_PORT_UP     1700
#define DEFAULT_PORT_DW     1700
static char serv_addr[64] = STR(DEFAULT_SERVER); /* address of the server (host name or IPv4/IPv6) */
static char serv_port_up[8] = STR(DEFAULT_PORT_UP); /* server port for upstream traffic */
static char serv_port_down[8] = STR(DEFAULT_PORT_DW); /* server port for downstream traffic */
static int keepalive_time = DEFAULT_KEEPALIVE; /* send a PULL_DATA request every X seconds, negative = disabled */

/* statistics collection configuration variables */
static unsigned stat_interval = DEFAULT_STAT; /* time interval (in sec) at which statistics are collected and displayed */

void thread_up();
void thread_down();
void thread_jit();

static void sig_handler(int sigio)
{
    if (sigio == SIGQUIT) {
        quit_sig = true;
    } else if ((sigio == SIGINT) || (sigio == SIGTERM)) {
        exit_sig = true;
    }
    return;
}



static double difftimespec(struct timespec end, struct timespec beginning) {
    double x;

    x = 1E-9 * (double)(end.tv_nsec - beginning.tv_nsec);
    x += (double)(end.tv_sec - beginning.tv_sec);

    return x;
}

void wait_ms(unsigned long a) {
    struct timespec dly;
    struct timespec rem;

    dly.tv_sec = a / 1000;
    dly.tv_nsec = ((long)a % 1000) * 1000000;

    DEBUG_PRINTF("NOTE dly: %ld sec %ld ns\n", dly.tv_sec, dly.tv_nsec);

    if((dly.tv_sec > 0) || ((dly.tv_sec == 0) && (dly.tv_nsec > 100000))) {
        clock_nanosleep(CLOCK_MONOTONIC, 0, &dly, &rem);
        DEBUG_PRINTF("NOTE remain: %ld sec %ld ns\n", rem.tv_sec, rem.tv_nsec);
    }
    return;
}





int lgw_start()
{
//	uint32_t Freq;
	uint8_t UPwr;
	SX1276InitIO();
	SX1276Reset();
	if(SX1276CheckSPI() == LGW_SPI_ERROR)
	{
		printf("Check SPI failed\n");
		return -1;
	}
	SX1276Init();
//	Freq = 470300000;
//	UPwr = 20;
//	SX1276FreqSet(Freq);

//	SX1276TxPower(UPwr);
//	SX1276SetOpMode(RFLR_OPMODE_STANDBY);

	SX1276RxStateEnter();

	return SUCCESS;

}

int lgw_stop()
{
	SX1276Reset();
	return SX1276Disconnect();
}

void lgw_receive()
{

	if(DIO0)
	{
		SX1276RxDataRead( &ucRxbuf[0], &receivedbytes );
		SX1276RxStateEnter();
	}
	else
	{
//		printf("nothing has happend\n");
		receivedbytes = 0;
	}
}

static int send_tx_ack(uint8_t token_h, uint8_t token_l, enum jit_error_e error) {
    uint8_t buff_ack[64]; /* buffer to give feedback to server */
    int buff_index;

    /* reset buffer */
    memset(&buff_ack, 0, sizeof buff_ack);

    /* Prepare downlink feedback to be sent to server */
    buff_ack[0] = PROTOCOL_VERSION;
    buff_ack[1] = token_h;
    buff_ack[2] = token_l;
    buff_ack[3] = PKT_TX_ACK;
    buff_ack[4] = (unsigned char)ifr.ifr_hwaddr.sa_data[0];
    buff_ack[5] = (unsigned char)ifr.ifr_hwaddr.sa_data[1];
    buff_ack[6] = (unsigned char)ifr.ifr_hwaddr.sa_data[2];
    buff_ack[7] = 0xFF;
    buff_ack[8] = 0xFF;
    buff_ack[9] = (unsigned char)ifr.ifr_hwaddr.sa_data[3];
    buff_ack[10] = (unsigned char)ifr.ifr_hwaddr.sa_data[4];
    buff_ack[11] = (unsigned char)ifr.ifr_hwaddr.sa_data[5];
    buff_index = 12; /* 12-byte header */

    /* Put no JSON string if there is nothing to report */
    if (error != JIT_ERROR_OK) {
        /* start of JSON structure */
        memcpy((void *)(buff_ack + buff_index), (void *)"{\"txpk_ack\":{", 13);
        buff_index += 13;
        /* set downlink error status in JSON structure */
        memcpy((void *)(buff_ack + buff_index), (void *)"\"error\":", 8);
        buff_index += 8;
        switch (error) {
            case JIT_ERROR_FULL:
            case JIT_ERROR_COLLISION_PACKET:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"COLLISION_PACKET\"", 18);
                buff_index += 18;
                /* update stats */
                pthread_mutex_lock(&mx_meas_dw);
                meas_nb_tx_rejected_collision_packet += 1;
                pthread_mutex_unlock(&mx_meas_dw);
                break;
            case JIT_ERROR_TOO_LATE:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"TOO_LATE\"", 10);
                buff_index += 10;
                /* update stats */
                pthread_mutex_lock(&mx_meas_dw);
                meas_nb_tx_rejected_too_late += 1;
                pthread_mutex_unlock(&mx_meas_dw);
                break;
            case JIT_ERROR_TOO_EARLY:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"TOO_EARLY\"", 11);
                buff_index += 11;
                /* update stats */
                pthread_mutex_lock(&mx_meas_dw);
                meas_nb_tx_rejected_too_early += 1;
                pthread_mutex_unlock(&mx_meas_dw);
                break;
            case JIT_ERROR_COLLISION_BEACON:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"COLLISION_BEACON\"", 18);
                buff_index += 18;
                /* update stats */
                pthread_mutex_lock(&mx_meas_dw);
                meas_nb_tx_rejected_collision_beacon += 1;
                pthread_mutex_unlock(&mx_meas_dw);
                break;
            case JIT_ERROR_TX_FREQ:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"TX_FREQ\"", 9);
                buff_index += 9;
                break;
            case JIT_ERROR_TX_POWER:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"TX_POWER\"", 10);
                buff_index += 10;
                break;
            case JIT_ERROR_GPS_UNLOCKED:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"GPS_UNLOCKED\"", 14);
                buff_index += 14;
                break;
            default:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"UNKNOWN\"", 9);
                buff_index += 9;
                break;
        }
        /* end of JSON structure */
        memcpy((void *)(buff_ack + buff_index), (void *)"}}", 2);
        buff_index += 2;
    }

    buff_ack[buff_index] = 0; /* add string terminator, for safety */

    /* send datagram to server */
    return send(sock_down, (void *)buff_ack, buff_index, 0);
}


int lgw_send(struct lgw_pkt_tx_s pkt_data) {
    int i, x;
    uint8_t buff[256+TX_METADATA_NB]; /* buffer to prepare the packet to send + metadata before SPI write burst */
    uint32_t part_int = 0; /* integer part for PLL register value calculation */
    uint32_t part_frac = 0; /* fractional part for PLL register value calculation */
    uint16_t fsk_dr_div; /* divider to configure for target datarate */
    int transfer_size = 0; /* data to transfer from host to TX databuffer */
    int payload_offset = 0; /* start of the payload content in the databuffer */
    uint8_t pow_index = 0; /* 4-bit value to set the firmware TX power */
    uint8_t target_mix_gain = 0; /* used to select the proper I/Q offset correction */
    uint32_t count_trig = 0; /* timestamp value in trigger mode corrected for TX start delay */
    bool tx_allowed = false;
    uint16_t tx_start_delay;
    bool tx_notch_enable = false;

//    /* check if the concentrator is running */
//    if (lgw_is_started == false) {
//        DEBUG_MSG("ERROR: CONCENTRATOR IS NOT RUNNING, START IT BEFORE SENDING\n");
//        return LGW_HAL_ERROR;
//    }
//
//    /* check input range (segfault prevention) */
//    if (pkt_data.rf_chain >= LGW_RF_CHAIN_NB) {
//        DEBUG_MSG("ERROR: INVALID RF_CHAIN TO SEND PACKETS\n");
//        return LGW_HAL_ERROR;
//    }
//
//    /* check input variables */
//    if (rf_tx_enable[pkt_data.rf_chain] == false) {
//        DEBUG_MSG("ERROR: SELECTED RF_CHAIN IS DISABLED FOR TX ON SELECTED BOARD\n");
//        return LGW_HAL_ERROR;
//    }
//    if (rf_enable[pkt_data.rf_chain] == false) {
//        DEBUG_MSG("ERROR: SELECTED RF_CHAIN IS DISABLED\n");
//        return LGW_HAL_ERROR;
//    }
    if (!IS_TX_MODE(pkt_data.tx_mode)) {
    	MSG("ERROR: TX_MODE NOT SUPPORTED\n");
        return LGW_HAL_ERROR;
    }
    if (pkt_data.modulation == MOD_LORA) {
        if (!IS_LORA_BW(pkt_data.bandwidth)) {
        	MSG("ERROR: BANDWIDTH NOT SUPPORTED BY LORA TX\n");
            return LGW_HAL_ERROR;
        }
        if (!IS_LORA_STD_DR(pkt_data.datarate)) {
        	MSG("ERROR: DATARATE NOT SUPPORTED BY LORA TX\n");
            return LGW_HAL_ERROR;
        }
        if (!IS_LORA_CR(pkt_data.coderate)) {
        	MSG("ERROR: CODERATE NOT SUPPORTED BY LORA TX\n");
            return LGW_HAL_ERROR;
        }
        if (pkt_data.size > 255) {
        	MSG("ERROR: PAYLOAD LENGTH TOO BIG FOR LORA TX\n");
            return LGW_HAL_ERROR;
        }
    } else if (pkt_data.modulation == MOD_FSK) {
        if((pkt_data.f_dev < 1) || (pkt_data.f_dev > 200)) {
        	MSG("ERROR: TX FREQUENCY DEVIATION OUT OF ACCEPTABLE RANGE\n");
            return LGW_HAL_ERROR;
        }
        if(!IS_FSK_DR(pkt_data.datarate)) {
        	MSG("ERROR: DATARATE NOT SUPPORTED BY FSK IF CHAIN\n");
            return LGW_HAL_ERROR;
        }
        if (pkt_data.size > 255) {
        	MSG("ERROR: PAYLOAD LENGTH TOO BIG FOR FSK TX\n");
            return LGW_HAL_ERROR;
        }
    } else {
    	MSG("ERROR: INVALID TX MODULATION\n");
        return LGW_HAL_ERROR;
    }

    /* Enable notch filter for LoRa 125kHz */
    if ((pkt_data.modulation == MOD_LORA) && (pkt_data.bandwidth == BW_125KHZ)) {
        tx_notch_enable = true;
    }

    /* Get the TX start delay to be applied for this TX */
//    tx_start_delay = lgw_get_tx_start_delay(tx_notch_enable, pkt_data.bandwidth);

    /* interpretation of TX power */
//    for (pow_index = txgain_lut.size-1; pow_index > 0; pow_index--) {
//        if (txgain_lut.lut[pow_index].rf_power <= pkt_data.rf_power) {
//            break;
//        }
//    }

    /* loading TX imbalance correction */
//    target_mix_gain = txgain_lut.lut[pow_index].mix_gain;
//    if (pkt_data.rf_chain == 0) { /* use radio A calibration table */
//        lgw_reg_w(LGW_TX_OFFSET_I, cal_offset_a_i[target_mix_gain - 8]);
//        lgw_reg_w(LGW_TX_OFFSET_Q, cal_offset_a_q[target_mix_gain - 8]);
//    } else { /* use radio B calibration table */
//        lgw_reg_w(LGW_TX_OFFSET_I, cal_offset_b_i[target_mix_gain - 8]);
//        lgw_reg_w(LGW_TX_OFFSET_Q, cal_offset_b_q[target_mix_gain - 8]);
//    }

    /* Set digital gain from LUT */
//    lgw_reg_w(LGW_TX_GAIN, txgain_lut.lut[pow_index].dig_gain);

    /* fixed metadata, useful payload and misc metadata compositing */
    transfer_size =  pkt_data.size; /*  */
//    payload_offset = TX_METADATA_NB; /* start the payload just after the metadata */

    /* metadata 0 to 2, TX PLL frequency */
//    switch (rf_radio_type[0]) { /* we assume that there is only one radio type on the board */
//        case LGW_RADIO_TYPE_SX1255:
//            part_int = pkt_data.freq_hz / (SX125x_32MHz_FRAC << 7); /* integer part, gives the MSB */
//            part_frac = ((pkt_data.freq_hz % (SX125x_32MHz_FRAC << 7)) << 9) / SX125x_32MHz_FRAC; /* fractional part, gives middle part and LSB */
//            break;
//        case LGW_RADIO_TYPE_SX1257:
//            part_int = pkt_data.freq_hz / (SX125x_32MHz_FRAC << 8); /* integer part, gives the MSB */
//            part_frac = ((pkt_data.freq_hz % (SX125x_32MHz_FRAC << 8)) << 8) / SX125x_32MHz_FRAC; /* fractional part, gives middle part and LSB */
//            break;
//        default:
//            DEBUG_PRINTF("ERROR: UNEXPECTED VALUE %d FOR RADIO TYPE\n", rf_radio_type[0]);
//            break;
//    }

//    buff[0] = 0xFF & part_int; /* Most Significant Byte */
//    buff[1] = 0xFF & (part_frac >> 8); /* middle byte */
//    buff[2] = 0xFF & part_frac; /* Least Significant Byte */

    /* metadata 3 to 6, timestamp trigger value */
    /* TX state machine must be triggered at (T0 - lgw_i_tx_start_delay_us) for packet to start being emitted at T0 */
//    if (pkt_data.tx_mode == TIMESTAMPED)
//    {
//        count_trig = pkt_data.count_us - (uint32_t)tx_start_delay;
//        buff[3] = 0xFF & (count_trig >> 24);
//        buff[4] = 0xFF & (count_trig >> 16);
//        buff[5] = 0xFF & (count_trig >> 8);
//        buff[6] = 0xFF &  count_trig;
//    }

    /* parameters depending on modulation  */
//    if (pkt_data.modulation == MOD_LORA) {
//        /* metadata 7, modulation type, radio chain selection and TX power */
//        buff[7] = (0x20 & (pkt_data.rf_chain << 5)) | (0x0F & pow_index); /* bit 4 is 0 -> LoRa modulation */
//
//        buff[8] = 0; /* metadata 8, not used */
//
//        /* metadata 9, CRC, LoRa CR & SF */
//        switch (pkt_data.datarate) {
//            case DR_LORA_SF7: buff[9] = 7; break;
//            case DR_LORA_SF8: buff[9] = 8; break;
//            case DR_LORA_SF9: buff[9] = 9; break;
//            case DR_LORA_SF10: buff[9] = 10; break;
//            case DR_LORA_SF11: buff[9] = 11; break;
//            case DR_LORA_SF12: buff[9] = 12; break;
//            default: DEBUG_PRINTF("ERROR: UNEXPECTED VALUE %d IN SWITCH STATEMENT\n", pkt_data.datarate);
//        }
//        switch (pkt_data.coderate) {
//            case CR_LORA_4_5: buff[9] |= 1 << 4; break;
//            case CR_LORA_4_6: buff[9] |= 2 << 4; break;
//            case CR_LORA_4_7: buff[9] |= 3 << 4; break;
//            case CR_LORA_4_8: buff[9] |= 4 << 4; break;
//            default: DEBUG_PRINTF("ERROR: UNEXPECTED VALUE %d IN SWITCH STATEMENT\n", pkt_data.coderate);
//        }
//        if (pkt_data.no_crc == false) {
//            buff[9] |= 0x80; /* set 'CRC enable' bit */
//        } else {
//            DEBUG_MSG("Info: packet will be sent without CRC\n");
//        }
//
//        /* metadata 10, payload size */
//        buff[10] = pkt_data.size;
//
//        /* metadata 11, implicit header, modulation bandwidth, PPM offset & polarity */
//        switch (pkt_data.bandwidth) {
//            case BW_125KHZ: buff[11] = 0; break;
//            case BW_250KHZ: buff[11] = 1; break;
//            case BW_500KHZ: buff[11] = 2; break;
//            default: DEBUG_PRINTF("ERROR: UNEXPECTED VALUE %d IN SWITCH STATEMENT\n", pkt_data.bandwidth);
//        }
//        if (pkt_data.no_header == true) {
//            buff[11] |= 0x04; /* set 'implicit header' bit */
//        }
//        if (SET_PPM_ON(pkt_data.bandwidth,pkt_data.datarate)) {
//            buff[11] |= 0x08; /* set 'PPM offset' bit at 1 */
//        }
//        if (pkt_data.invert_pol == true) {
//            buff[11] |= 0x10; /* set 'TX polarity' bit at 1 */
//        }
//
//        /* metadata 12 & 13, LoRa preamble size */
//        if (pkt_data.preamble == 0) { /* if not explicit, use recommended LoRa preamble size */
//            pkt_data.preamble = STD_LORA_PREAMBLE;
//        } else if (pkt_data.preamble < MIN_LORA_PREAMBLE) { /* enforce minimum preamble size */
//            pkt_data.preamble = MIN_LORA_PREAMBLE;
//            DEBUG_MSG("Note: preamble length adjusted to respect minimum LoRa preamble size\n");
//        }
//        buff[12] = 0xFF & (pkt_data.preamble >> 8);
//        buff[13] = 0xFF & pkt_data.preamble;
//
//        /* metadata 14 & 15, not used */
//        buff[14] = 0;
//        buff[15] = 0;
//
//        /* MSB of RF frequency is now used in AGC firmware to implement large/narrow filtering in SX1257/55 */
//        buff[0] &= 0x3F; /* Unset 2 MSBs of frequency code */
//        if (pkt_data.bandwidth == BW_500KHZ) {
//            buff[0] |= 0x80; /* Set MSB bit to enlarge analog filter for 500kHz BW */
//        }
//
//        /* Set MSB-1 bit to enable digital filter if required */
//        if (tx_notch_enable == true) {
//            DEBUG_MSG("INFO: Enabling TX notch filter\n");
//            buff[0] |= 0x40;
//        }
//    }
//    else if (pkt_data.modulation == MOD_FSK) {
//        /* metadata 7, modulation type, radio chain selection and TX power */
//        buff[7] = (0x20 & (pkt_data.rf_chain << 5)) | 0x10 | (0x0F & pow_index); /* bit 4 is 1 -> FSK modulation */
//
//        buff[8] = 0; /* metadata 8, not used */
//
//        /* metadata 9, frequency deviation */
//        buff[9] = pkt_data.f_dev;
//
//        /* metadata 10, payload size */
//        buff[10] = pkt_data.size;
//        /* TODO: how to handle 255 bytes packets ?!? */
//
//        /* metadata 11, packet mode, CRC, encoding */
//        buff[11] = 0x01 | (pkt_data.no_crc?0:0x02) | (0x02 << 2); /* always in variable length packet mode, whitening, and CCITT CRC if CRC is not disabled  */
//
//        /* metadata 12 & 13, FSK preamble size */
//        if (pkt_data.preamble == 0) { /* if not explicit, use LoRa MAC preamble size */
//            pkt_data.preamble = STD_FSK_PREAMBLE;
//        } else if (pkt_data.preamble < MIN_FSK_PREAMBLE) { /* enforce minimum preamble size */
//            pkt_data.preamble = MIN_FSK_PREAMBLE;
//            DEBUG_MSG("Note: preamble length adjusted to respect minimum FSK preamble size\n");
//        }
//        buff[12] = 0xFF & (pkt_data.preamble >> 8);
//        buff[13] = 0xFF & pkt_data.preamble;
//
//        /* metadata 14 & 15, FSK baudrate */
//        fsk_dr_div = (uint16_t)((uint32_t)LGW_XTAL_FREQU / pkt_data.datarate); /* Ok for datarate between 500bps and 250kbps */
//        buff[14] = 0xFF & (fsk_dr_div >> 8);
//        buff[15] = 0xFF & fsk_dr_div;
//
//        /* insert payload size in the packet for variable mode */
//        buff[16] = pkt_data.size;
//        ++transfer_size; /* one more byte to transfer to the TX modem */
//        ++payload_offset; /* start the payload with one more byte of offset */
//
//        /* MSB of RF frequency is now used in AGC firmware to implement large/narrow filtering in SX1257/55 */
//        buff[0] &= 0x7F; /* Always use narrow band for FSK (force MSB to 0) */
//
//    }
//    else {
//        DEBUG_MSG("ERROR: INVALID TX MODULATION..\n");
//        return LGW_HAL_ERROR;
//    }

    /* Configure TX start delay based on TX notch filter */
//    lgw_reg_w(LGW_TX_START_DELAY, tx_start_delay);

    /* copy payload from user struct to buffer containing metadata */
    memcpy((void *)(buff), (void *)(pkt_data.payload), pkt_data.size);

    for(int i=0; i <transfer_size;i++)
    {
    	printf("%x ",buff[i]);
    }
    printf("\n");
    sleep(0.3);


    if(SX1276Tx1Data(&buff[0], transfer_size)!=0)
    {
    	MSG("INFO: TX1 SEND ERROR .........\n");
//    	return LGW_HAL_ERROR;

    }
    else
    	printf("TX1 SENDING....\n");

    sleep(1);

    if(SX1276Tx2Data(&buff[0], transfer_size)!=0)
    {
    	MSG("INFO: TX2 SEND ERROR .........\n");
   //    	return LGW_HAL_ERROR;

    }
    else
    	printf("TX2 SENDIGN....\n");

    SX1276RxStateEnter();
//    /* reset TX command flags */
//    lgw_abort_tx();
//
//    /* put metadata + payload in the TX data buffer */
//    lgw_reg_w(LGW_TX_DATA_BUF_ADDR, 0);
//    lgw_reg_wb(LGW_TX_DATA_BUF_DATA, buff, transfer_size);
//    DEBUG_ARRAY(i, transfer_size, buff);
//
//    x = lbt_is_channel_free(&pkt_data, tx_start_delay, &tx_allowed);
//    if (x != LGW_LBT_SUCCESS) {
//        DEBUG_MSG("ERROR: Failed to check channel availability for TX\n");
//        return LGW_HAL_ERROR;
//    }
//    if (tx_allowed == true) {
//        switch(pkt_data.tx_mode) {
//            case IMMEDIATE:
//                lgw_reg_w(LGW_TX_TRIG_IMMEDIATE, 1);
//                break;
//
//            case TIMESTAMPED:
//                lgw_reg_w(LGW_TX_TRIG_DELAYED, 1);
//                break;
//
//            case ON_GPS:
//                lgw_reg_w(LGW_TX_TRIG_GPS, 1);
//                break;
//
//            default:
//                DEBUG_PRINTF("ERROR: UNEXPECTED VALUE %d IN SWITCH STATEMENT\n", pkt_data.tx_mode);
//                return LGW_HAL_ERROR;
//        }
//    } else {
//        DEBUG_MSG("ERROR: Cannot send packet, channel is busy (LBT)\n");
//        return LGW_LBT_ISSUE;
//    }

    return LGW_HAL_SUCCESS;
}

int main()
{
	printf("this is LoRa test!\n");
	lgw_start();
//	uint8_t buff[10] = {"nihao,lora"};
	while(1)
	{
		lgw_receive();
		 if ((receivedbytes <= 0) )
		 {
			 wait_ms(FETCH_SLEEP_MS);
			 continue;
		 }
		 else
		 {
			 printf("%s\n",ucRxbuf);
			 for (int i  = 0; i < receivedbytes; i++)
			 {
				 printf("%x ",ucRxbuf[i]);
			 }
			 printf("\n");
		 }
//		if(SX1276Tx1Data(&buff[0], 10)!=0)
//		{
//			MSG("INFO: TX1 SEND ERROR .........\n");
//
//		}
	}

	return 0;
}







//int main()
//{
//	int ret;
//	struct sigaction sigact; /* SIGQUIT&SIGINT&SIGTERM signal handling */
//
//   /* variables to get local copies of measurements */
//	uint32_t cp_nb_rx_rcv;
//	uint32_t cp_nb_rx_ok;
//	uint32_t cp_nb_rx_bad;
//	uint32_t cp_nb_rx_nocrc;
//	uint32_t cp_up_pkt_fwd;
//	uint32_t cp_up_network_byte;
//	uint32_t cp_up_payload_byte;
//	uint32_t cp_up_dgram_sent;
//	uint32_t cp_up_ack_rcv;
//	uint32_t cp_dw_pull_sent;
//	uint32_t cp_dw_ack_rcv;
//	uint32_t cp_dw_dgram_rcv;
//	uint32_t cp_dw_network_byte;
//	uint32_t cp_dw_payload_byte;
//	uint32_t cp_nb_tx_ok;
//	uint32_t cp_nb_tx_fail;
//	uint32_t cp_nb_tx_requested = 0;
//	uint32_t cp_nb_tx_rejected_collision_packet = 0;
//	uint32_t cp_nb_tx_rejected_collision_beacon = 0;
//	uint32_t cp_nb_tx_rejected_too_late = 0;
//	uint32_t cp_nb_tx_rejected_too_early = 0;
//	uint32_t cp_nb_beacon_queued = 0;
//	uint32_t cp_nb_beacon_sent = 0;
//	uint32_t cp_nb_beacon_rejected = 0;
//
//
//
//	 /* threads */
//	pthread_t thrid_up; //周期性从concentrator取mote上行应用报文并组帧成PUSH_DATA消息发送给 server
//	pthread_t thrid_down; //维持 GW 和 serv 之间链路，接收 serv下行应用报文、组织 beacon 并加入JiT 队列
//	pthread_t thrid_jit;//周期性从 JiT 队列取报文并发送到concentrator （通过 concentrator 发送到 mote）
//
//
//    /* network socket creation */
//    struct addrinfo hints;
//    struct addrinfo *result; /* store result of getaddrinfo */
//    struct addrinfo *q; /* pointer to move into *result data */
//    char host_name[64];
//    char port_name[64];
//
//    /* statistics variable */
//	time_t t;
//	char stat_timestamp[24];
//	float rx_ok_ratio;
//	float rx_bad_ratio;
//	float rx_nocrc_ratio;
//	float up_ack_ratio;
//	float dw_ack_ratio;
//
//    struct timeval nowtime;
//
//
//    /* get timezone info */
//    tzset();
//
//    /* prepare hints to open network sockets */
//	memset(&hints, 0, sizeof hints);
//	hints.ai_family = AF_INET; /* WA: Forcing IPv4 as AF_UNSPEC makes connection on localhost to fail */
//	hints.ai_socktype = SOCK_DGRAM;
//
//	 /* look for server address w/ upstream port */
//	ret = getaddrinfo(serv_addr, serv_port_up, &hints, &result);
//	if (ret != 0) {
//		MSG("ERROR: [up] getaddrinfo on address %s (PORT %s) returned %s\n", serv_addr, serv_port_up, gai_strerror(ret));
//		exit(EXIT_FAILURE);
//	}
//
//	/* try to open socket for upstream traffic */
//	for (q=result; q!=NULL; q=q->ai_next) {
//		sock_up = socket(q->ai_family, q->ai_socktype,q->ai_protocol);
//		if (sock_up == -1) continue; /* try next field */
//		else break; /* success, get out of loop */
//	}
//	if (q == NULL) {
//		MSG("ERROR: [up] failed to open socket to any of server %s addresses (port %s)\n", serv_addr, serv_port_up);
//		ret = 1;
//		for (q=result; q!=NULL; q=q->ai_next) {
//			getnameinfo(q->ai_addr, q->ai_addrlen, host_name, sizeof host_name, port_name, sizeof port_name, NI_NUMERICHOST);
//			MSG("INFO: [up] result %i host:%s service:%s\n", ret, host_name, port_name);
//			++ret;
//		}
//		exit(EXIT_FAILURE);
//	}
//
//	/* connect so we can send/receive packet with the server only */
//	ret = connect(sock_up, q->ai_addr, q->ai_addrlen);
//	if (ret != 0) {
//		MSG("ERROR: [up] connect returned %s\n", strerror(errno));
//		exit(EXIT_FAILURE);
//	}
//	freeaddrinfo(result);
//
//	/* look for server address w/ downstream port */
//	ret = getaddrinfo(serv_addr, serv_port_down, &hints, &result);
//	if (ret != 0) {
//		MSG("ERROR: [down] getaddrinfo on address %s (port %s) returned %s\n", serv_addr, serv_port_up, gai_strerror(ret));
//		exit(EXIT_FAILURE);
//	}
//
//	/* try to open socket for downstream traffic */
//	for (q=result; q!=NULL; q=q->ai_next) {
//		sock_down = socket(q->ai_family, q->ai_socktype,q->ai_protocol);
//		if (sock_down == -1) continue; /* try next field */
//		else break; /* success, get out of loop */
//	}
//	if (q == NULL) {
//		MSG("ERROR: [down] failed to open socket to any of server %s addresses (port %s)\n", serv_addr, serv_port_up);
//		ret = 1;
//		for (q=result; q!=NULL; q=q->ai_next) {
//			getnameinfo(q->ai_addr, q->ai_addrlen, host_name, sizeof host_name, port_name, sizeof port_name, NI_NUMERICHOST);
//			MSG("INFO: [down] result %i host:%s service:%s\n", ret, host_name, port_name);
//			++ret;
//		}
//		exit(EXIT_FAILURE);
//	}
//
//	/* connect so we can send/receive packet with the server only */
//	ret = connect(sock_down, q->ai_addr, q->ai_addrlen);
//	if (ret != 0) {
//		MSG("ERROR: [down] connect returned %s\n", strerror(errno));
//		exit(EXIT_FAILURE);
//	}
//	freeaddrinfo(result);
//
//
//    /* display result */
//	ifr.ifr_addr.sa_family = AF_INET;
//	strncpy(ifr.ifr_name, "eth0", IFNAMSIZ-1);  // can we rely on eth0?
//	ioctl(sock_up, SIOCGIFHWADDR, &ifr);
//    printf("Gateway ID: %.2x:%.2x:%.2x:ff:ff:%.2x:%.2x:%.2x\n",
//           (unsigned char)ifr.ifr_hwaddr.sa_data[0],
//           (unsigned char)ifr.ifr_hwaddr.sa_data[1],
//           (unsigned char)ifr.ifr_hwaddr.sa_data[2],
//           (unsigned char)ifr.ifr_hwaddr.sa_data[3],
//           (unsigned char)ifr.ifr_hwaddr.sa_data[4],
//           (unsigned char)ifr.ifr_hwaddr.sa_data[5]);
//    printf("Listening at SF%i on %.6lf Mhz.\n", sf,(double)freq/1000000);
//    printf("------------------\n");
//
//    /* starting the gateway */
//    ret = lgw_start();
//
//    if (ret == SUCCESS)
//    {
//    	MSG("INFO: [main] concentrator started, packet can now be received\n");
//    }
//    else
//    {
//    	MSG("ERROR: [main] failed to start the concentrator\n");
//    	exit(EXIT_FAILURE);
//    }
//
//
//
//    /* spawn threads to manage upstream and downstream */
//    ret = pthread_create( &thrid_up, NULL, (void * (*)(void *))thread_up, NULL);
//    if (ret != 0)
//    {
//    	MSG("ERROR: [main] impossible to create upstream thread\n");
//    	exit(EXIT_FAILURE);
//    }
//    ret = pthread_create( &thrid_down, NULL, (void * (*)(void *))thread_down, NULL);
//    if (ret != 0)
//    {
//    	MSG("ERROR: [main] impossible to create upstream thread\n");
//    	exit(EXIT_FAILURE);
//    }
//    ret = pthread_create( &thrid_jit, NULL, (void * (*)(void *))thread_jit, NULL);
//    if (ret != 0)
//    {
//    	MSG("ERROR: [main] impossible to create upstream thread\n");
//    	exit(EXIT_FAILURE);
//    }
//
//    /* configure signal handling */
//    sigemptyset(&sigact.sa_mask);
//    sigact.sa_flags = 0;
//    sigact.sa_handler = sig_handler;
//    sigaction(SIGQUIT, &sigact, NULL); /* Ctrl-\ */
//    sigaction(SIGINT, &sigact, NULL); /* Ctrl-C */
//    sigaction(SIGTERM, &sigact, NULL); /* default "kill" command */
//
//    /* main loop task : statistics collection */
//    while (!exit_sig && !quit_sig)
//    {
//           /* wait for next reporting interval */
//           wait_ms(1000 * stat_interval);
//
//           /* get timestamp for statistics */
//           t = time(NULL);
//           strftime(stat_timestamp, sizeof stat_timestamp, "%F %T %Z", gmtime(&t));
//
//           /* access upstream statistics, copy and reset them */
//           pthread_mutex_lock(&mx_meas_up);
//           cp_nb_rx_rcv       = meas_nb_rx_rcv;
//           cp_nb_rx_ok        = meas_nb_rx_ok;
//           cp_nb_rx_bad       = meas_nb_rx_bad;
//           cp_nb_rx_nocrc     = meas_nb_rx_nocrc;
//           cp_up_pkt_fwd      = meas_up_pkt_fwd;
//           cp_up_network_byte = meas_up_network_byte;
//           cp_up_payload_byte = meas_up_payload_byte;
//           cp_up_dgram_sent   = meas_up_dgram_sent;
//           cp_up_ack_rcv      = meas_up_ack_rcv;
//           meas_nb_rx_rcv = 0;
//           meas_nb_rx_ok = 0;
//           meas_nb_rx_bad = 0;
//           meas_nb_rx_nocrc = 0;
//           meas_up_pkt_fwd = 0;
//           meas_up_network_byte = 0;
//           meas_up_payload_byte = 0;
//           meas_up_dgram_sent = 0;
//           meas_up_ack_rcv = 0;
//           pthread_mutex_unlock(&mx_meas_up);
//           if (cp_nb_rx_rcv > 0) {
//               rx_ok_ratio = (float)cp_nb_rx_ok / (float)cp_nb_rx_rcv;
//               rx_bad_ratio = (float)cp_nb_rx_bad / (float)cp_nb_rx_rcv;
//               rx_nocrc_ratio = (float)cp_nb_rx_nocrc / (float)cp_nb_rx_rcv;
//           } else {
//               rx_ok_ratio = 0.0;
//               rx_bad_ratio = 0.0;
//               rx_nocrc_ratio = 0.0;
//           }
//           if (cp_up_dgram_sent > 0) {
//               up_ack_ratio = (float)cp_up_ack_rcv / (float)cp_up_dgram_sent;
//           } else {
//               up_ack_ratio = 0.0;
//           }
//
//           /* access downstream statistics, copy and reset them */
//           pthread_mutex_lock(&mx_meas_dw);
//           cp_dw_pull_sent    =  meas_dw_pull_sent;
//           cp_dw_ack_rcv      =  meas_dw_ack_rcv;
//           cp_dw_dgram_rcv    =  meas_dw_dgram_rcv;
//           cp_dw_network_byte =  meas_dw_network_byte;
//           cp_dw_payload_byte =  meas_dw_payload_byte;
//           cp_nb_tx_ok        =  meas_nb_tx_ok;
//           cp_nb_tx_fail      =  meas_nb_tx_fail;
//           cp_nb_tx_requested                 +=  meas_nb_tx_requested;
//           cp_nb_tx_rejected_collision_packet +=  meas_nb_tx_rejected_collision_packet;
//           cp_nb_tx_rejected_collision_beacon +=  meas_nb_tx_rejected_collision_beacon;
//           cp_nb_tx_rejected_too_late         +=  meas_nb_tx_rejected_too_late;
//           cp_nb_tx_rejected_too_early        +=  meas_nb_tx_rejected_too_early;
//           cp_nb_beacon_queued   +=  meas_nb_beacon_queued;
//           cp_nb_beacon_sent     +=  meas_nb_beacon_sent;
//           cp_nb_beacon_rejected +=  meas_nb_beacon_rejected;
//           meas_dw_pull_sent = 0;
//           meas_dw_ack_rcv = 0;
//           meas_dw_dgram_rcv = 0;
//           meas_dw_network_byte = 0;
//           meas_dw_payload_byte = 0;
//           meas_nb_tx_ok = 0;
//           meas_nb_tx_fail = 0;
//           meas_nb_tx_requested = 0;
//           meas_nb_tx_rejected_collision_packet = 0;
//           meas_nb_tx_rejected_collision_beacon = 0;
//           meas_nb_tx_rejected_too_late = 0;
//           meas_nb_tx_rejected_too_early = 0;
//           meas_nb_beacon_queued = 0;
//           meas_nb_beacon_sent = 0;
//           meas_nb_beacon_rejected = 0;
//           pthread_mutex_unlock(&mx_meas_dw);
//           if (cp_dw_pull_sent > 0) {
//               dw_ack_ratio = (float)cp_dw_ack_rcv / (float)cp_dw_pull_sent;
//           } else {
//               dw_ack_ratio = 0.0;
//           }
//
//           /* access GPS statistics, copy them */
////           if (gps_enabled == true) {
////               pthread_mutex_lock(&mx_meas_gps);
////               coord_ok = gps_coord_valid;
////               cp_gps_coord = meas_gps_coord;
////               pthread_mutex_unlock(&mx_meas_gps);
////           }
//
////           /* overwrite with reference coordinates if function is enabled */
////           if (gps_fake_enable == true) {
////               cp_gps_coord = reference_coord;
////           }
//
//           /* display a report */
//           printf("\n##### %s #####\n", stat_timestamp);
//           printf("### [UPSTREAM] ###\n");
//           printf("# RF packets received by concentrator: %u\n", cp_nb_rx_rcv);
//           printf("# CRC_OK: %.2f%%, CRC_FAIL: %.2f%%, NO_CRC: %.2f%%\n", 100.0 * rx_ok_ratio, 100.0 * rx_bad_ratio, 100.0 * rx_nocrc_ratio);
//           printf("# RF packets forwarded: %u (%u bytes)\n", cp_up_pkt_fwd, cp_up_payload_byte);
//           printf("# PUSH_DATA datagrams sent: %u (%u bytes)\n", cp_up_dgram_sent, cp_up_network_byte);
//           printf("# PUSH_DATA acknowledged: %.2f%%\n", 100.0 * up_ack_ratio);
//           printf("### [DOWNSTREAM] ###\n");
//           printf("# PULL_DATA sent: %u (%.2f%% acknowledged)\n", cp_dw_pull_sent, 100.0 * dw_ack_ratio);
//           printf("# PULL_RESP(onse) datagrams received: %u (%u bytes)\n", cp_dw_dgram_rcv, cp_dw_network_byte);
//           printf("# RF packets sent to concentrator: %u (%u bytes)\n", (cp_nb_tx_ok+cp_nb_tx_fail), cp_dw_payload_byte);
//           printf("# TX errors: %u\n", cp_nb_tx_fail);
//           if (cp_nb_tx_requested != 0 ) {
//               printf("# TX rejected (collision packet): %.2f%% (req:%u, rej:%u)\n", 100.0 * cp_nb_tx_rejected_collision_packet / cp_nb_tx_requested, cp_nb_tx_requested, cp_nb_tx_rejected_collision_packet);
//               printf("# TX rejected (collision beacon): %.2f%% (req:%u, rej:%u)\n", 100.0 * cp_nb_tx_rejected_collision_beacon / cp_nb_tx_requested, cp_nb_tx_requested, cp_nb_tx_rejected_collision_beacon);
//               printf("# TX rejected (too late): %.2f%% (req:%u, rej:%u)\n", 100.0 * cp_nb_tx_rejected_too_late / cp_nb_tx_requested, cp_nb_tx_requested, cp_nb_tx_rejected_too_late);
//               printf("# TX rejected (too early): %.2f%% (req:%u, rej:%u)\n", 100.0 * cp_nb_tx_rejected_too_early / cp_nb_tx_requested, cp_nb_tx_requested, cp_nb_tx_rejected_too_early);
//           }
//           printf("# BEACON queued: %u\n", cp_nb_beacon_queued);
//           printf("# BEACON sent so far: %u\n", cp_nb_beacon_sent);
//           printf("# BEACON rejected: %u\n", cp_nb_beacon_rejected);
//           printf("### [JIT] ###\n");
//           /* get timestamp captured on PPM pulse  */
////           pthread_mutex_lock(&mx_concent);
////           i = lgw_get_trigcnt(&trig_tstamp);
////           pthread_mutex_unlock(&mx_concent);
////           if (i != LGW_HAL_SUCCESS) {
////               printf("# SX1301 time (PPS): unknown\n");
////           } else {
////               printf("# SX1301 time (PPS): %u\n", trig_tstamp);
////           }
////           jit_print_queue (&jit_queue, false, DEBUG_LOG);
////           printf("### [GPS] ###\n");
////           if (gps_enabled == true) {
////               /* no need for mutex, display is not critical */
////               if (gps_ref_valid == true) {
////                   printf("# Valid time reference (age: %li sec)\n", (long)difftime(time(NULL), time_reference_gps.systime));
////               } else {
////                   printf("# Invalid time reference (age: %li sec)\n", (long)difftime(time(NULL), time_reference_gps.systime));
////               }
////               if (coord_ok == true) {
////                   printf("# GPS coordinates: latitude %.5f, longitude %.5f, altitude %i m\n", cp_gps_coord.lat, cp_gps_coord.lon, cp_gps_coord.alt);
////               } else {
////                   printf("# no valid GPS coordinates available yet\n");
////               }
////           } else if (gps_fake_enable == true) {
////               printf("# GPS *FAKE* coordinates: latitude %.5f, longitude %.5f, altitude %i m\n", cp_gps_coord.lat, cp_gps_coord.lon, cp_gps_coord.alt);
////           } else {
////               printf("# GPS sync is disabled\n");
////           }
////           printf("##### END #####\n");
//
//           /* generate a JSON report (will be sent to server by upstream thread) */
//           pthread_mutex_lock(&mx_stat_rep);
//           snprintf(status_report, STATUS_SIZE, "\"stat\":{\"time\":\"%s\",\"rxnb\":%u,\"rxok\":%u,\"rxfw\":%u,\"ackr\":%.1f,\"dwnb\":%u,\"txnb\":%u}", stat_timestamp, cp_nb_rx_rcv, cp_nb_rx_ok, cp_up_pkt_fwd, 100.0 * up_ack_ratio, cp_dw_dgram_rcv, cp_nb_tx_ok);
//           report_ready = true;
//           pthread_mutex_unlock(&mx_stat_rep);
//       }
//
//       /* wait for upstream thread to finish (1 fetch cycle max) */
//       pthread_join(thrid_up, NULL);
//       pthread_cancel(thrid_down); /* don't wait for downstream thread */
//       pthread_cancel(thrid_jit); /* don't wait for jit thread */
//
//       /* if an exit signal was received, try to quit properly */
//       if (exit_sig) {
//           /* shut down network sockets */
//           shutdown(s, SHUT_RDWR);
//           /* stop the hardware */
//           ret = lgw_stop();
//           if (ret == SUCCESS) {
//               MSG("INFO: concentrator stopped successfully\n");
//           } else {
//               MSG("WARNING: failed to stop concentrator successfully\n");
//           }
//       }
//       MSG("INFO: Exiting packet forwarder program\n");
//       exit(EXIT_SUCCESS);
//}

/* -------------------------------------------------------------------------- */
/* --- THREAD 1: RECEIVING PACKETS AND FORWARDING THEM ---------------------- */

void thread_up(void)
{
//	printf("-----------------------");
//	printf("------UP LINK--------");
//	printf("-----------------------");
	int i,j;
    uint8_t  SNR;
//    int rssicorr;
    struct timespec send_time;
    struct timespec recv_time;
    /* report management variable */
    bool send_report = false;

    /* mote info variables */
   uint32_t mote_addr = 0;
   uint16_t mote_fcnt = 0;

    /* data buffers */
    uint8_t buff_up[TX_BUFF_SIZE]; /* buffer to compose the upstream packet */
    int buff_index;
    uint8_t buff_ack[32]; /* buffer to receive acknowledges */

    /* set upstream socket RX timeout */
    i = setsockopt(sock_up, SOL_SOCKET, SO_RCVTIMEO, (void *)&push_timeout_half, sizeof push_timeout_half);
    if (i != 0) {
        MSG("ERROR: [up] setsockopt returned %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }


    /* pre-fill the data buffer with fixed fields */
    buff_up[0] = PROTOCOL_VERSION;
	buff_up[3] = PKT_PUSH_DATA;
	buff_up[4] = (unsigned char)ifr.ifr_hwaddr.sa_data[0];
	buff_up[5] = (unsigned char)ifr.ifr_hwaddr.sa_data[1];
	buff_up[6] = (unsigned char)ifr.ifr_hwaddr.sa_data[2];
	buff_up[7] = 0xFF;
	buff_up[8] = 0xFF;
	buff_up[9] = (unsigned char)ifr.ifr_hwaddr.sa_data[3];
	buff_up[10] = (unsigned char)ifr.ifr_hwaddr.sa_data[4];
	buff_up[11] = (unsigned char)ifr.ifr_hwaddr.sa_data[5];

    while(!exit_sig && !quit_sig)
	{

    	/* fetch packets */
		pthread_mutex_lock(&mx_concent);
		lgw_receive();
		pthread_mutex_unlock(&mx_concent);

        /* check if there are status report to send */
        send_report = report_ready; /* copy the variable so it doesn't change mid-function */

        /* wait a short time if no packets, nor status report */
        if ((receivedbytes <= 0) )
        {

        	wait_ms(FETCH_SLEEP_MS);
        	continue;
        }

		SNR = GetLoRaSNR();
		printf("Packet RSSI: %f, ",GetPackLoRaRSSI());
		printf("RSSI: %f, ",SX1276LoRaReadRssi());
		printf("SNR: %i, ",(int)SNR);
		printf("Length: %i",(int)receivedbytes);
		printf("\n");
		printf("receive :%s\n",ucRxbuf);
		printf("buff:");
		for (int i  = 0; i < receivedbytes; i++)
		{
			printf("%x ",ucRxbuf[i]);
		}
		printf("\n");

//		j = bin_to_b64(ucRxbuf, receivedbytes, (char *)(b64), 341);
		//fwrite(b64, sizeof(char), j, stdout);

		/* start composing datagram with the header */
		uint8_t token_h = (uint8_t)rand(); /* random token */
		uint8_t token_l = (uint8_t)rand(); /* random token */
		buff_up[1] = token_h;
		buff_up[2] = token_l;
		buff_index = 12; /* 12-byte header */

		// TODO: tmst can jump is time is (re)set, not good.
		struct timeval now;
		gettimeofday(&now, NULL);
		uint32_t tmst = (uint32_t)(now.tv_sec*1000000 + now.tv_usec);

		/* start of JSON structure */
		memcpy((void *)(buff_up + buff_index), (void *)"{\"rxpk\":[", 9);
		buff_index += 9;

        /* Get mote information from current packet (addr, fcnt) */
        /* FHDR - DevAddr */
        mote_addr  = ucRxbuf[1];
        mote_addr |= ucRxbuf[2] << 8;
        mote_addr |= ucRxbuf[3] << 16;
        mote_addr |= ucRxbuf[4] << 24;
        /* FHDR - FCnt */
        mote_fcnt  = ucRxbuf[6];
        mote_fcnt |= ucRxbuf[7] << 8;

        /* basic packet filtering */
        pthread_mutex_lock(&mx_meas_up);
        meas_nb_rx_rcv += 1;
        meas_nb_rx_ok += 1;
        printf( "\nINFO: Received pkt from mote: %08X (fcnt=%u)\n", mote_addr, mote_fcnt );
        meas_up_pkt_fwd += 1;
        meas_up_payload_byte += receivedbytes;
        pthread_mutex_unlock(&mx_meas_up);
		buff_up[buff_index] = '{';
		++buff_index;
		j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, "\"tmst\":%u", tmst);
		buff_index += j;
		j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"chan\":%1u,\"rfch\":%1u,\"freq\":%.6lf", 0, 0, (double)freq/1000000);
		buff_index += j;
		memcpy((void *)(buff_up + buff_index), (void *)",\"stat\":1", 9);
		buff_index += 9;
		memcpy((void *)(buff_up + buff_index), (void *)",\"modu\":\"LORA\"", 14);
		buff_index += 14;
		/* Lora datarate & bandwidth, 16-19 useful chars */
		switch (sf) {
		case SF7:
			memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF7", 12);
			buff_index += 12;
			break;
		case SF8:
			memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF8", 12);
			buff_index += 12;
			break;
		case SF9:
			memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF9", 12);
			buff_index += 12;
			break;
		case SF10:
			memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF10", 13);
			buff_index += 13;
			break;
		case SF11:
			memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF11", 13);
			buff_index += 13;
			break;
		case SF12:
			memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF12", 13);
			buff_index += 13;
			break;
		default:
			memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF?", 12);
			buff_index += 12;
		}
		memcpy((void *)(buff_up + buff_index), (void *)"BW125\"", 6);
		buff_index += 6;
		memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"4/5\"", 13);
		buff_index += 13;
		j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"lsnr\":%.1f", (double)SNR);
		buff_index += j;
		j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"rssi\":%.0f,\"size\":%u", GetPackLoRaRSSI(), receivedbytes);
		buff_index += j;
		memcpy((void *)(buff_up + buff_index), (void *)",\"data\":\"", 9);
		buff_index += 9;
		j = bin_to_b64((uint8_t *)ucRxbuf, receivedbytes, (char *)(buff_up + buff_index), 341);
		  if (j>=0) {
			buff_index += j;
		} else {
			MSG("ERROR: [up] bin_to_b64 failed line %u\n", (__LINE__ - 5));
			exit(EXIT_FAILURE);
		}
		buff_up[buff_index] = '"';
		++buff_index;

		/* End of packet serialization */
		buff_up[buff_index] = '}';
		++buff_index;
		buff_up[buff_index] = ']';
		++buff_index;

		if (send_report == true)
		{
			buff_up[buff_index] = ',';
			++buff_index;
		}
		/* add status report if a new one is available */
		if (send_report == true) {
			pthread_mutex_lock(&mx_stat_rep);
			report_ready = false;
			j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, "%s", status_report);
			pthread_mutex_unlock(&mx_stat_rep);
			if (j > 0) {
				buff_index += j;
			} else {
				MSG("ERROR: [up] snprintf failed line %u\n", (__LINE__ - 5));
				exit(EXIT_FAILURE);
			}
		}

		/* end of JSON datagram payload */
		buff_up[buff_index] = '}';
		++buff_index;
		buff_up[buff_index] = 0; /* add string terminator, for safety */

		printf("rxpk update: %s\n", (char *)(buff_up + 12)); /* DEBUG: display JSON payload */

		/* send datagram to server */
		int ret = send(sock_up, (void *)buff_up, buff_index, 0);
		if(ret == -1)
			printf("[UP]:send error");
		clock_gettime(CLOCK_MONOTONIC, &send_time);
		pthread_mutex_lock(&mx_meas_up);
		meas_up_dgram_sent += 1;
		meas_up_network_byte += buff_index;

		/* wait for acknowledge (in 2 times, to catch extra packets) */
		for (i=0; i<2; ++i)
		{
			j = recv(sock_up, (void *)buff_ack, sizeof buff_ack, 0);
			clock_gettime(CLOCK_MONOTONIC, &recv_time);
			if (j == -1) {
				if (errno == EAGAIN) { /* timeout */
					continue;
				} else { /* server connection error */
					break;
				}
			} else if ((j < 4) || (buff_ack[0] != PROTOCOL_VERSION) || (buff_ack[3] != PKT_PUSH_ACK)) {
				MSG("WARNING: [up] ignored invalid non-ACL packet\n");
				continue;
			} else if ((buff_ack[1] != token_h) || (buff_ack[2] != token_l)) {
				MSG("WARNING: [up] ignored out-of sync ACK packet\n");
				continue;
			}
			else
			{
				MSG("INFO: [up] PUSH_ACK received in %i ms\n", (int)(1000 * difftimespec(recv_time, send_time)));
//				for(int s = 0;s < sizeof(buff_ack);s++)
//				{
//					printf("%x ",buff_ack[s]);
//				}
//				printf("\n");
				meas_up_ack_rcv += 1;
				break;
			}
		}
		pthread_mutex_unlock(&mx_meas_up);

	} // received a message
    MSG("\nINFO: End of upstream thread\n");
}


/* -------------------------------------------------------------------------- */
/* --- THREAD 2: POLLING SERVER AND ENQUEUING PACKETS IN JIT QUEUE ---------- */
void thread_down(void)
{

//	printf("-----------------------");
//	printf("------DOWN LINK--------");
//	printf("-----------------------");
	int i; /* loop variables */

	/* configuration and metadata for an outbound packet */
	struct lgw_pkt_tx_s txpkt;
    bool sent_immediate = false; /* option to sent the packet immediately */

    /* local timekeeping variables */
    struct timespec send_time; /* time of the pull request */
    struct timespec recv_time; /* time of return from recv socket call */

    /* data buffers */
    uint8_t buff_down[1000]; /* buffer to receive downstream packets */
    uint8_t buff_req[12]; /* buffer to compose pull requests */
    int msg_len;

    /* protocol variables */
    uint8_t token_h; /* random token for acknowledgement matching */
    uint8_t token_l; /* random token for acknowledgement matching */
    bool req_ack = false; /* keep track of whether PULL_DATA was acknowledged or not */

    /* JSON parsing variables */
    JSON_Value *root_val = NULL;
    JSON_Object *txpk_obj = NULL;
    JSON_Value *val = NULL; /* needed to detect the absence of some fields */
    const char *str; /* pointer to sub-strings in the JSON data */
    short x0, x1;
    uint64_t x2;
    double x3, x4;

    /* variables to send on GPS timestamp */
//    struct tref local_ref; /* time reference used for GPS <-> timestamp conversion */
//    struct timespec gps_tx; /* GPS time that needs to be converted to timestamp */

    /* beacon variables */
//    struct lgw_pkt_tx_s beacon_pkt;
    uint8_t beacon_chan;
    uint8_t beacon_loop;
    size_t beacon_RFU1_size = 0;
    size_t beacon_RFU2_size = 0;
    uint8_t beacon_pyld_idx = 0;
    time_t diff_beacon_time;
//    struct timespec next_beacon_gps_time; /* gps time of next beacon packet */
//    struct timespec last_beacon_gps_time; /* gps time of last enqueued beacon packet */
    int retry;

    /* beacon data fields, byte 0 is Least Significant Byte */
    int32_t field_latitude; /* 3 bytes, derived from reference latitude */
    int32_t field_longitude; /* 3 bytes, derived from reference longitude */
    uint16_t field_crc1, field_crc2;

    /* auto-quit variable */
    uint32_t autoquit_cnt = 0; /* count the number of PULL_DATA sent since the latest PULL_ACK */

    /* Just In Time downlink */
    struct timeval current_unix_time;
    struct timeval current_concentrator_time;
    enum jit_error_e jit_result = JIT_ERROR_OK;
    enum jit_pkt_type_e downlink_type;

    /* set downstream socket RX timeout */
    i = setsockopt(sock_down, SOL_SOCKET, SO_RCVTIMEO, (void *)&pull_timeout, sizeof pull_timeout);
    if (i != 0) {
        MSG("ERROR: [down] setsockopt returned %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* pre-fill the pull request buffer with fixed fields */
    buff_req[0] = PROTOCOL_VERSION;
    buff_req[3] = PKT_PULL_DATA;
    buff_req[4] = (unsigned char)ifr.ifr_hwaddr.sa_data[0];
    buff_req[5] = (unsigned char)ifr.ifr_hwaddr.sa_data[1];
    buff_req[6] = (unsigned char)ifr.ifr_hwaddr.sa_data[2];
    buff_req[7] = 0xFF;
    buff_req[8] = 0xFF;
    buff_req[9] = (unsigned char)ifr.ifr_hwaddr.sa_data[3];
    buff_req[10] = (unsigned char)ifr.ifr_hwaddr.sa_data[4];
    buff_req[11] = (unsigned char)ifr.ifr_hwaddr.sa_data[5];

////    /* beacon variables initialization */
////    last_beacon_gps_time.tv_sec = 0;
////    last_beacon_gps_time.tv_nsec = 0;
//
//    /* beacon packet parameters */
////    beacon_pkt.tx_mode = ON_GPS; /* send on PPS pulse */
////    beacon_pkt.rf_chain = 0; /* antenna A */
////    beacon_pkt.rf_power = beacon_power;
////    beacon_pkt.modulation = MOD_LORA;
////    switch (beacon_bw_hz) {
////        case 125000:
////            beacon_pkt.bandwidth = BW_125KHZ;
////            break;
////        case 500000:
////            beacon_pkt.bandwidth = BW_500KHZ;
////            break;
////        default:
////            /* should not happen */
////            MSG("ERROR: unsupported bandwidth for beacon\n");
////            exit(EXIT_FAILURE);
////    }
////    switch (beacon_datarate) {
////        case 8:
////            beacon_pkt.datarate = DR_LORA_SF8;
////            beacon_RFU1_size = 1;
////            beacon_RFU2_size = 3;
////            break;
////        case 9:
////            beacon_pkt.datarate = DR_LORA_SF9;
////            beacon_RFU1_size = 2;
////            beacon_RFU2_size = 0;
////            break;
////        case 10:
////            beacon_pkt.datarate = DR_LORA_SF10;
////            beacon_RFU1_size = 3;
////            beacon_RFU2_size = 1;
////            break;
////        case 12:
////            beacon_pkt.datarate = DR_LORA_SF12;
////            beacon_RFU1_size = 5;
////            beacon_RFU2_size = 3;
////            break;
////        default:
////            /* should not happen */
////            MSG("ERROR: unsupported datarate for beacon\n");
////            exit(EXIT_FAILURE);
////    }
////    beacon_pkt.size = beacon_RFU1_size + 4 + 2 + 7 + beacon_RFU2_size + 2;
////    beacon_pkt.coderate = CR_LORA_4_5;
////    beacon_pkt.invert_pol = false;
////    beacon_pkt.preamble = 10;
////    beacon_pkt.no_crc = true;
////    beacon_pkt.no_header = true;
//
//    /* network common part beacon fields (little endian) */
////    for (i = 0; i < (int)beacon_RFU1_size; i++) {
////        beacon_pkt.payload[beacon_pyld_idx++] = 0x0;
////    }
//
//    /* network common part beacon fields (little endian) */
////    beacon_pyld_idx += 4; /* time (variable), filled later */
////    beacon_pyld_idx += 2; /* crc1 (variable), filled later */
//
//    /* calculate the latitude and longitude that must be publicly reported */
////    field_latitude = (int32_t)((reference_coord.lat / 90.0) * (double)(1<<23));
////    if (field_latitude > (int32_t)0x007FFFFF) {
////        field_latitude = (int32_t)0x007FFFFF; /* +90 N is represented as 89.99999 N */
////    } else if (field_latitude < (int32_t)0xFF800000) {
////        field_latitude = (int32_t)0xFF800000;
////    }
////    field_longitude = (int32_t)((reference_coord.lon / 180.0) * (double)(1<<23));
////    if (field_longitude > (int32_t)0x007FFFFF) {
////        field_longitude = (int32_t)0x007FFFFF; /* +180 E is represented as 179.99999 E */
////    } else if (field_longitude < (int32_t)0xFF800000) {
////        field_longitude = (int32_t)0xFF800000;
////    }
//
//    /* gateway specific beacon fields */
////    beacon_pkt.payload[beacon_pyld_idx++] = beacon_infodesc;
////    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF &  field_latitude;
////    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_latitude >>  8);
////    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_latitude >> 16);
////    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF &  field_longitude;
////    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_longitude >>  8);
////    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_longitude >> 16);
//
//    /* RFU */
////    for (i = 0; i < (int)beacon_RFU2_size; i++) {
////        beacon_pkt.payload[beacon_pyld_idx++] = 0x0;
////    }
//
//    /* CRC of the beacon gateway specific part fields */
////    field_crc2 = crc16((beacon_pkt.payload + 6 + beacon_RFU1_size), 7 + beacon_RFU2_size);
////    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF &  field_crc2;
////    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_crc2 >> 8);
//
    /* JIT queue initialization */
    jit_queue_init(&jit_queue);

    while (!exit_sig && !quit_sig) {
//
//        /* auto-quit if the threshold is crossed */
//        if ((autoquit_threshold > 0) && (autoquit_cnt >= autoquit_threshold)) {
//            exit_sig = true;
//            MSG("INFO: [down] the last %u PULL_DATA were not ACKed, exiting application\n", autoquit_threshold);
//            break;
//        }

    	/* generate random token for request */
    	token_h = (uint8_t)rand(); /* random token */
        token_l = (uint8_t)rand(); /* random token */
        buff_req[1] = token_h;
        buff_req[2] = token_l;

        /* send PULL request and record time */
        send(sock_down, (void *)buff_req, sizeof buff_req, 0);
        clock_gettime(CLOCK_MONOTONIC, &send_time);
        pthread_mutex_lock(&mx_meas_dw);
        meas_dw_pull_sent += 1;
        pthread_mutex_unlock(&mx_meas_dw);
        req_ack = false;
        autoquit_cnt++;

        /* listen to packets and process them until a new PULL request must be sent */
        recv_time = send_time;
        while ((int)difftimespec(recv_time, send_time) < keepalive_time)
        {
            /* try to receive a datagram */
            msg_len = recv(sock_down, (void *)buff_down, (sizeof buff_down)-1, 0);
            clock_gettime(CLOCK_MONOTONIC, &recv_time);

            /* Pre-allocate beacon slots in JiT queue, to check downlink collisions */
            beacon_loop = JIT_NUM_BEACON_IN_QUEUE - jit_queue.num_beacon;
            retry = 0;
//            while (beacon_loop && (beacon_period != 0)) {
//                pthread_mutex_lock(&mx_timeref);
//                /* Wait for GPS to be ready before inserting beacons in JiT queue */
//                if ((gps_ref_valid == true) && (xtal_correct_ok == true)) {
//
//                    /* compute GPS time for next beacon to come      */
//                    /*   LoRaWAN: T = k*beacon_period + TBeaconDelay */
//                    /*            with TBeaconDelay = [1.5ms +/- 1µs]*/
//                    if (last_beacon_gps_time.tv_sec == 0) {
//                        /* if no beacon has been queued, get next slot from current GPS time */
//                        diff_beacon_time = time_reference_gps.gps.tv_sec % ((time_t)beacon_period);
//                        next_beacon_gps_time.tv_sec = time_reference_gps.gps.tv_sec +
//                                                        ((time_t)beacon_period - diff_beacon_time);
//                    } else {
//                        /* if there is already a beacon, take it as reference */
//                        next_beacon_gps_time.tv_sec = last_beacon_gps_time.tv_sec + beacon_period;
//                    }
//                    /* now we can add a beacon_period to the reference to get next beacon GPS time */
//                    next_beacon_gps_time.tv_sec += (retry * beacon_period);
//                    next_beacon_gps_time.tv_nsec = 0;
//
//#if DEBUG_BEACON
//                    {
//                    time_t time_unix;
//
//                    time_unix = time_reference_gps.gps.tv_sec + UNIX_GPS_EPOCH_OFFSET;
//                    MSG_DEBUG(DEBUG_BEACON, "GPS-now : %s", ctime(&time_unix));
//                    time_unix = last_beacon_gps_time.tv_sec + UNIX_GPS_EPOCH_OFFSET;
//                    MSG_DEBUG(DEBUG_BEACON, "GPS-last: %s", ctime(&time_unix));
//                    time_unix = next_beacon_gps_time.tv_sec + UNIX_GPS_EPOCH_OFFSET;
//                    MSG_DEBUG(DEBUG_BEACON, "GPS-next: %s", ctime(&time_unix));
//                    }
//#endif
//
//                    /* convert GPS time to concentrator time, and set packet counter for JiT trigger */
//                    lgw_gps2cnt(time_reference_gps, next_beacon_gps_time, &(beacon_pkt.count_us));
//                    pthread_mutex_unlock(&mx_timeref);
//
//                    /* apply frequency correction to beacon TX frequency */
//                    if (beacon_freq_nb > 1) {
//                        beacon_chan = (next_beacon_gps_time.tv_sec / beacon_period) % beacon_freq_nb; /* floor rounding */
//                    } else {
//                        beacon_chan = 0;
//                    }
//                    /* Compute beacon frequency */
//                    beacon_pkt.freq_hz = beacon_freq_hz + (beacon_chan * beacon_freq_step);
//
//                    /* load time in beacon payload */
//                    beacon_pyld_idx = beacon_RFU1_size;
//                    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF &  next_beacon_gps_time.tv_sec;
//                    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (next_beacon_gps_time.tv_sec >>  8);
//                    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (next_beacon_gps_time.tv_sec >> 16);
//                    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (next_beacon_gps_time.tv_sec >> 24);
//
//                    /* calculate CRC */
//                    field_crc1 = crc16(beacon_pkt.payload, 4 + beacon_RFU1_size); /* CRC for the network common part */
//                    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & field_crc1;
//                    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_crc1 >> 8);
//
//                    /* Insert beacon packet in JiT queue */
//                    gettimeofday(&current_unix_time, NULL);
//                    get_concentrator_time(&current_concentrator_time, current_unix_time);
//                    jit_result = jit_enqueue(&jit_queue, &current_concentrator_time, &beacon_pkt, JIT_PKT_TYPE_BEACON);
//                    if (jit_result == JIT_ERROR_OK) {
//                        /* update stats */
//                        pthread_mutex_lock(&mx_meas_dw);
//                        meas_nb_beacon_queued += 1;
//                        pthread_mutex_unlock(&mx_meas_dw);
//
//                        /* One more beacon in the queue */
//                        beacon_loop--;
//                        retry = 0;
//                        last_beacon_gps_time.tv_sec = next_beacon_gps_time.tv_sec; /* keep this beacon time as reference for next one to be programmed */
//
//                        /* display beacon payload */
//                        MSG("INFO: Beacon queued (count_us=%u, freq_hz=%u, size=%u):\n", beacon_pkt.count_us, beacon_pkt.freq_hz, beacon_pkt.size);
//                        printf( "   => " );
//                        for (i = 0; i < beacon_pkt.size; ++i) {
//                            MSG("%02X ", beacon_pkt.payload[i]);
//                        }
//                        MSG("\n");
//                    } else {
//                        MSG_DEBUG(DEBUG_BEACON, "--> beacon queuing failed with %d\n", jit_result);
//                        /* update stats */
//                        pthread_mutex_lock(&mx_meas_dw);
//                        if (jit_result != JIT_ERROR_COLLISION_BEACON) {
//                            meas_nb_beacon_rejected += 1;
//                        }
//                        pthread_mutex_unlock(&mx_meas_dw);
//                        /* In case previous enqueue failed, we retry one period later until it succeeds */
//                        /* Note: In case the GPS has been unlocked for a while, there can be lots of retries */
//                        /*       to be done from last beacon time to a new valid one */
//                        retry++;
//                        MSG_DEBUG(DEBUG_BEACON, "--> beacon queuing retry=%d\n", retry);
//                    }
//                } else {
//                    pthread_mutex_unlock(&mx_timeref);
//                    break;
//                }
//            }

            /* if no network message was received, got back to listening sock_down socket */
            if (msg_len == -1) {
//                MSG("WARNING: [down] recv returned %s\n", strerror(errno)); /* too verbose */
                continue;
            }

            /* if the datagram does not respect protocol, just ignore it */
            if ((msg_len < 4) || (buff_down[0] != PROTOCOL_VERSION) || ((buff_down[3] != PKT_PULL_RESP) && (buff_down[3] != PKT_PULL_ACK))) {
                MSG("WARNING: [down] ignoring invalid packet len=%d, protocol_version=%d, id=%d\n",
                        msg_len, buff_down[0], buff_down[3]);
                continue;
            }

            /* if the datagram is an ACK, check token */
            if (buff_down[3] == PKT_PULL_ACK) {
                if ((buff_down[1] == token_h) && (buff_down[2] == token_l)) {
                    if (req_ack) {
                        MSG("INFO: [down] duplicate ACK received :)\n");
                    } else { /* if that packet was not already acknowledged */
                        req_ack = true;
                        autoquit_cnt = 0;
                        pthread_mutex_lock(&mx_meas_dw);
                        meas_dw_ack_rcv += 1;
                        pthread_mutex_unlock(&mx_meas_dw);
                        MSG("INFO: [down] PULL_ACK received in %i ms\n", (int)(1000 * difftimespec(recv_time, send_time)));
                    }
                } else { /* out-of-sync token */
                    MSG("INFO: [down] received out-of-sync ACK\n");
                }
                continue;
            }

            /* the datagram is a PULL_RESP */
            buff_down[msg_len] = 0; /* add string terminator, just to be safe */
            MSG("INFO: [down] PULL_RESP received  - token[%d:%d] :)\n", buff_down[1], buff_down[2]); /* very verbose */
            printf("\nJSON down: %s\n", (char *)(buff_down + 4)); /* DEBUG: display JSON payload */

            /* initialize TX struct and try to parse JSON */
            memset(&txpkt, 0, sizeof txpkt);
            root_val = json_parse_string_with_comments((const char *)(buff_down + 4)); /* JSON offset */
            if (root_val == NULL) {
                MSG("WARNING: [down] invalid JSON, TX aborted\n");
                continue;
            }

            /* look for JSON sub-object 'txpk' */
            txpk_obj = json_object_get_object(json_value_get_object(root_val), "txpk");
            if (txpk_obj == NULL) {
                MSG("WARNING: [down] no \"txpk\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }

            /* Parse "immediate" tag, or target timestamp, or UTC time to be converted by GPS (mandatory) */
            i = json_object_get_boolean(txpk_obj,"imme"); /* can be 1 if true, 0 if false, or -1 if not a JSON boolean */
            if (i == 1) {
                /* TX procedure: send immediately */
                sent_immediate = true;
                downlink_type = JIT_PKT_TYPE_DOWNLINK_CLASS_C;
                MSG("INFO: [down] a packet will be sent in \"immediate\" mode\n");
            } else {
            	sent_immediate = false;
            	val = json_object_get_value(txpk_obj,"tmst");
                if (val != NULL) {
                	/* TX procedure: send on timestamp value */
                	txpkt.count_us = (uint32_t)json_value_get_number(val);

                    /* Concentrator timestamp is given, we consider it is a Class A downlink */
                	downlink_type = JIT_PKT_TYPE_DOWNLINK_CLASS_A;
                } else {
                    /* TX procedure: send on GPS time (converted to timestamp value) */
                    val = json_object_get_value(txpk_obj, "tmms");
                    if (val == NULL) {
                        MSG("WARNING: [down] no mandatory \"txpk.tmst\" or \"txpk.tmms\" objects in JSON, TX aborted\n");
                        json_value_free(root_val);
                        continue;
                    }
//                    if (gps_enabled == true) {
//                        pthread_mutex_lock(&mx_timeref);
//                        if (gps_ref_valid == true) {
//                            local_ref = time_reference_gps;
//                            pthread_mutex_unlock(&mx_timeref);
//                        } else {
//                            pthread_mutex_unlock(&mx_timeref);
//                            MSG("WARNING: [down] no valid GPS time reference yet, impossible to send packet on specific GPS time, TX aborted\n");
//                            json_value_free(root_val);
//
//                            /* send acknoledge datagram to server */
//                            send_tx_ack(buff_down[1], buff_down[2], JIT_ERROR_GPS_UNLOCKED);
//                            continue;
//                        }
//                    } else {
//                        MSG("WARNING: [down] GPS disabled, impossible to send packet on specific GPS time, TX aborted\n");
//                        json_value_free(root_val);
//
//                        /* send acknoledge datagram to server */
//                        send_tx_ack(buff_down[1], buff_down[2], JIT_ERROR_GPS_UNLOCKED);
//                        continue;
//                    }

//                    /* Get GPS time from JSON */
//                    x2 = (uint64_t)json_value_get_number(val);
//
//                    /* Convert GPS time from milliseconds to timespec */
//                    x3 = modf((double)x2/1E3, &x4);
//                    gps_tx.tv_sec = (time_t)x4; /* get seconds from integer part */
//                    gps_tx.tv_nsec = (long)(x3 * 1E9); /* get nanoseconds from fractional part */
//
//                    /* transform GPS time to timestamp */
//                    i = lgw_gps2cnt(local_ref, gps_tx, &(txpkt.count_us));
//                    if (i != LGW_GPS_SUCCESS) {
//                        MSG("WARNING: [down] could not convert GPS time to timestamp, TX aborted\n");
//                        json_value_free(root_val);
//                        continue;
//                    } else {
//                        MSG("INFO: [down] a packet will be sent on timestamp value %u (calculated from GPS time)\n", txpkt.count_us);
//                    }
//
//                    /* GPS timestamp is given, we consider it is a Class B downlink */
//                    downlink_type = JIT_PKT_TYPE_DOWNLINK_CLASS_B;
                }
            }

            /* Parse "No CRC" flag (optional field) */
            val = json_object_get_value(txpk_obj,"ncrc");
            if (val != NULL) {
                txpkt.no_crc = (bool)json_value_get_boolean(val);
            }

            /* parse target frequency (mandatory) */
            val = json_object_get_value(txpk_obj,"freq");
            if (val == NULL) {
                MSG("WARNING: [down] no mandatory \"txpk.freq\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            txpkt.freq_hz = (uint32_t)((double)(1.0e6) * json_value_get_number(val));

            /* parse RF chain used for TX (mandatory) */
            val = json_object_get_value(txpk_obj,"rfch");
            if (val == NULL) {
                MSG("WARNING: [down] no mandatory \"txpk.rfch\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            txpkt.rf_chain = (uint8_t)json_value_get_number(val);

            /* parse TX power (optional field) */
            val = json_object_get_value(txpk_obj,"powe");
            if (val != NULL) {
                txpkt.rf_power = (int8_t)json_value_get_number(val) - antenna_gain;
            }

            /* Parse modulation (mandatory) */
            str = json_object_get_string(txpk_obj, "modu");
            if (str == NULL) {
                MSG("WARNING: [down] no mandatory \"txpk.modu\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            if (strcmp(str, "LORA") == 0) {
                /* Lora modulation */
                txpkt.modulation = MOD_LORA;

                /* Parse Lora spreading-factor and modulation bandwidth (mandatory) */
                str = json_object_get_string(txpk_obj, "datr");
                if (str == NULL) {
                    MSG("WARNING: [down] no mandatory \"txpk.datr\" object in JSON, TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }
                i = sscanf(str, "SF%2hdBW%3hd", &x0, &x1);
                if (i != 2) {
                    MSG("WARNING: [down] format error in \"txpk.datr\", TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }
                switch (x0) {
                    case  7: txpkt.datarate = DR_LORA_SF7;  break;
                    case  8: txpkt.datarate = DR_LORA_SF8;  break;
                    case  9: txpkt.datarate = DR_LORA_SF9;  break;
                    case 10: txpkt.datarate = DR_LORA_SF10; break;
                    case 11: txpkt.datarate = DR_LORA_SF11; break;
                    case 12: txpkt.datarate = DR_LORA_SF12; break;
                    default:
                        MSG("WARNING: [down] format error in \"txpk.datr\", invalid SF, TX aborted\n");
                        json_value_free(root_val);
                        continue;
                }
                switch (x1) {
                    case 125: txpkt.bandwidth = BW_125KHZ; break;
                    case 250: txpkt.bandwidth = BW_250KHZ; break;
                    case 500: txpkt.bandwidth = BW_500KHZ; break;
                    default:
                        MSG("WARNING: [down] format error in \"txpk.datr\", invalid BW, TX aborted\n");
                        json_value_free(root_val);
                        continue;
                }

                /* Parse ECC coding rate (optional field) */
                str = json_object_get_string(txpk_obj, "codr");
                if (str == NULL) {
                    MSG("WARNING: [down] no mandatory \"txpk.codr\" object in json, TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }
                if      (strcmp(str, "4/5") == 0) txpkt.coderate = CR_LORA_4_5;
                else if (strcmp(str, "4/6") == 0) txpkt.coderate = CR_LORA_4_6;
                else if (strcmp(str, "2/3") == 0) txpkt.coderate = CR_LORA_4_6;
                else if (strcmp(str, "4/7") == 0) txpkt.coderate = CR_LORA_4_7;
                else if (strcmp(str, "4/8") == 0) txpkt.coderate = CR_LORA_4_8;
                else if (strcmp(str, "1/2") == 0) txpkt.coderate = CR_LORA_4_8;
                else {
                    MSG("WARNING: [down] format error in \"txpk.codr\", TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }

                /* Parse signal polarity switch (optional field) */
                val = json_object_get_value(txpk_obj,"ipol");
                if (val != NULL) {
                    txpkt.invert_pol = (bool)json_value_get_boolean(val);
                }

                /* parse Lora preamble length (optional field, optimum min value enforced) */
                val = json_object_get_value(txpk_obj,"prea");
                if (val != NULL) {
                    i = (int)json_value_get_number(val);
                    if (i >= MIN_LORA_PREAMB) {
                        txpkt.preamble = (uint16_t)i;
                    } else {
                        txpkt.preamble = (uint16_t)MIN_LORA_PREAMB;
                    }
                } else {
                    txpkt.preamble = (uint16_t)STD_LORA_PREAMB;
                }

            } else if (strcmp(str, "FSK") == 0) {
                /* FSK modulation */
                txpkt.modulation = MOD_FSK;

                /* parse FSK bitrate (mandatory) */
                val = json_object_get_value(txpk_obj,"datr");
                if (val == NULL) {
                    MSG("WARNING: [down] no mandatory \"txpk.datr\" object in JSON, TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }
                txpkt.datarate = (uint32_t)(json_value_get_number(val));

                /* parse frequency deviation (mandatory) */
                val = json_object_get_value(txpk_obj,"fdev");
                if (val == NULL) {
                    MSG("WARNING: [down] no mandatory \"txpk.fdev\" object in JSON, TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }
                txpkt.f_dev = (uint8_t)(json_value_get_number(val) / 1000.0); /* JSON value in Hz, txpkt.f_dev in kHz */

                /* parse FSK preamble length (optional field, optimum min value enforced) */
                val = json_object_get_value(txpk_obj,"prea");
                if (val != NULL) {
                    i = (int)json_value_get_number(val);
                    if (i >= MIN_FSK_PREAMB) {
                        txpkt.preamble = (uint16_t)i;
                    } else {
                        txpkt.preamble = (uint16_t)MIN_FSK_PREAMB;
                    }
                } else {
                    txpkt.preamble = (uint16_t)STD_FSK_PREAMB;
                }

            } else {
                MSG("WARNING: [down] invalid modulation in \"txpk.modu\", TX aborted\n");
                json_value_free(root_val);
                continue;
            }

            /* Parse payload length (mandatory) */
            val = json_object_get_value(txpk_obj,"size");
            if (val == NULL) {
                MSG("WARNING: [down] no mandatory \"txpk.size\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            txpkt.size = (uint16_t)json_value_get_number(val);

            /* Parse payload data (mandatory) */
            str = json_object_get_string(txpk_obj, "data");
            if (str == NULL) {
                MSG("WARNING: [down] no mandatory \"txpk.data\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            i = b64_to_bin(str, strlen(str), txpkt.payload, sizeof txpkt.payload);
            if (i != txpkt.size) {
                MSG("WARNING: [down] mismatch between .size and .data size once converter to binary\n");
            }

            /* free the JSON parse tree from memory */
            json_value_free(root_val);

            /* select TX mode */
            if (sent_immediate) {
                txpkt.tx_mode = IMMEDIATE;
            } else {
                txpkt.tx_mode = TIMESTAMPED;
            }

            /* record measurement data */
            pthread_mutex_lock(&mx_meas_dw);
            meas_dw_dgram_rcv += 1; /* count only datagrams with no JSON errors */
            meas_dw_network_byte += msg_len; /* meas_dw_network_byte */
            meas_dw_payload_byte += txpkt.size;
            pthread_mutex_unlock(&mx_meas_dw);

            /* check TX parameter before trying to queue packet */
            jit_result = JIT_ERROR_OK;
            if ((txpkt.freq_hz < tx_freq_min[txpkt.rf_chain]) || (txpkt.freq_hz > tx_freq_max[txpkt.rf_chain])) {
                jit_result = JIT_ERROR_TX_FREQ;
                MSG("ERROR: Packet REJECTED, unsupported frequency - %u (min:%u,max:%u)\n", txpkt.freq_hz, tx_freq_min[txpkt.rf_chain], tx_freq_max[txpkt.rf_chain]);
            }
            if (jit_result == JIT_ERROR_OK) {
                for (i=0; i<txlut.size; i++) {
                    if (txlut.lut[i].rf_power == txpkt.rf_power) {
                        /* this RF power is supported, we can continue */
                        break;
                    }
                }
                if (i == txlut.size) {
                    /* this RF power is not supported */
                    jit_result = JIT_ERROR_TX_POWER;
                    MSG("ERROR: Packet REJECTED, unsupported RF power for TX - %d\n", txpkt.rf_power);
                }
            }

            /* insert packet to be sent into JIT queue */
            if (jit_result == JIT_ERROR_OK) {
            	gettimeofday(&current_unix_time, NULL);
            	get_concentrator_time(&current_concentrator_time, current_unix_time);
            	jit_result = jit_enqueue(&jit_queue, &current_concentrator_time, &txpkt, downlink_type);
                if (jit_result != JIT_ERROR_OK) {
                	printf("ERROR: Packet REJECTED (jit error=%d)\n", jit_result);
                }
                pthread_mutex_lock(&mx_meas_dw);
                meas_nb_tx_requested += 1;
                pthread_mutex_unlock(&mx_meas_dw);
            }

            /* Send acknoledge datagram to server */
            send_tx_ack(buff_down[1], buff_down[2], jit_result);

        }
    }
    MSG("\nINFO: End of downstream thread\n");
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 3: CHECKING PACKETS TO BE SENT FROM JIT QUEUE AND SEND THEM --- */

void thread_jit(void)
{
//	printf("-----------------------");
//	printf("------JIT LINK--------");
//	printf("-----------------------");

	int result = LGW_HAL_SUCCESS;
	struct lgw_pkt_tx_s pkt;
	int pkt_index = -1;
	struct timeval current_unix_time;
	struct timeval current_concentrator_time;
	enum jit_error_e jit_result;
	enum jit_pkt_type_e pkt_type;
	uint8_t tx_status;

	while (!exit_sig && !quit_sig)
	{
		wait_ms(10);

		/* transfer data and metadata to the concentrator, and schedule TX */
		gettimeofday(&current_unix_time, NULL);
		get_concentrator_time(&current_concentrator_time, current_unix_time);
		jit_result = jit_peek(&jit_queue, &current_concentrator_time, &pkt_index);
		if (jit_result == JIT_ERROR_OK)
		{
			if (pkt_index > -1)
			{
				jit_result = jit_dequeue(&jit_queue, pkt_index, &pkt, &pkt_type);
				if (jit_result == JIT_ERROR_OK)
				{
					/* update beacon stats */
					if (pkt_type == JIT_PKT_TYPE_BEACON)
					{
						/* Compensate breacon frequency with xtal error */
						pthread_mutex_lock(&mx_xcorr);
						pkt.freq_hz = (uint32_t)(xtal_correct * (double)pkt.freq_hz);
						MSG_DEBUG(DEBUG_BEACON, "beacon_pkt.freq_hz=%u (xtal_correct=%.15lf)\n", pkt.freq_hz, xtal_correct);
						pthread_mutex_unlock(&mx_xcorr);

						/* Update statistics */
						pthread_mutex_lock(&mx_meas_dw);
						meas_nb_beacon_sent += 1;
						pthread_mutex_unlock(&mx_meas_dw);
						MSG("INFO: Beacon dequeued (count_us=%u)\n", pkt.count_us);
					}

					/* check if concentrator is free for sending new packet */
					pthread_mutex_lock(&mx_concent); /* may have to wait for a fetch to finish */
					result = LGW_HAL_SUCCESS;
//					result = lgw_status(TX_STATUS, &tx_status);
					tx_status = LGW_HAL_SUCCESS;
					pthread_mutex_unlock(&mx_concent); /* free concentrator ASAP */
					if (result == LGW_HAL_ERROR)
					{
						MSG("WARNING: [jit] lgw_status failed\n");
					}
					else
					{
						if (tx_status == TX_EMITTING)
						{
							MSG("ERROR: concentrator is currently emitting\n");
							MSG("INFO: [jit] lgw_status returned TX_EMITTING\n");
							continue;
						}
						else if (tx_status == TX_SCHEDULED)
						{
							MSG("WARNING: a downlink was already scheduled, overwritting it...\n");
							MSG("INFO: [jit] lgw_status returned TX_SCHEDULED\n");
						}
						else
						{
							/* Nothing to do */
						}
					}

					/* send packet to concentrator */
					pthread_mutex_lock(&mx_concent); /* may have to wait for a fetch to finish */
					result = lgw_send(pkt);
					pthread_mutex_unlock(&mx_concent); /* free concentrator ASAP */
					if (result == LGW_HAL_ERROR)
					{
						pthread_mutex_lock(&mx_meas_dw);
						meas_nb_tx_fail += 1;
						pthread_mutex_unlock(&mx_meas_dw);
						MSG("WARNING: [jit] lgw_send failed\n");
						continue;
					}
					else
					{
						pthread_mutex_lock(&mx_meas_dw);
						meas_nb_tx_ok += 1;
						pthread_mutex_unlock(&mx_meas_dw);
						MSG_DEBUG(DEBUG_PKT_FWD, "lgw_send done: count_us=%u\n", pkt.count_us);
					}
				}
				else
				{
					MSG("ERROR: jit_dequeue failed with %d\n", jit_result);
				}
			}
		}
		else if (jit_result == JIT_ERROR_EMPTY)
		{
			/* Do nothing, it can happen */
		}
		else
		{
			MSG("ERROR: jit_peek failed with %d\n", jit_result);
		}
	}
}







/*********************************************************************************************************
  End Of File
*********************************************************************************************************/
