#include "app.h"
#include "cfg.h"
#include "cic.h"
#include "dd.h"
#include "flash.h"
#include "fpga.h"
#include "rtc.h"
#include "timer.h"
#include "update.h"
#include "usb.h"
#include "version.h"
#include "writeback.h"


#define BOOTLOADER_ADDRESS          (0x04E00000UL)
#define BOOTLOADER_LENGTH           (1920 * 1024)

#define MEMORY_LENGTH               (0x05002980UL)

#define RX_FLUSH_ADDRESS            (0x07F00000UL)
#define RX_FLUSH_LENGTH             (1 * 1024 * 1024)

#define DEBUG_WRITE_TIMEOUT_TICKS   (100)


enum rx_state {
    RX_STATE_IDLE,
    RX_STATE_ARGS,
    RX_STATE_DATA,
    RX_STATE_FLUSH,
};

enum tx_state {
    TX_STATE_IDLE,
    TX_STATE_TOKEN,
    TX_STATE_DATA,
    TX_STATE_DMA,
    TX_STATE_FLUSH,
};


struct process {
    enum rx_state rx_state;
    uint8_t rx_counter;
    uint8_t rx_cmd;
    uint32_t rx_args[2];
    bool rx_dma_running;

    enum tx_state tx_state;
    uint8_t tx_counter;
    usb_tx_info_t tx_info;
    uint32_t tx_token;
    bool tx_dma_running;

    bool flush_response;
    bool flush_packet;

    bool response_pending;
    bool response_error;
    usb_tx_info_t response_info;

    bool packet_pending;
    usb_tx_info_t packet_info;

    bool read_ready;
    uint32_t read_length;
    uint32_t read_address;
};


static struct process p;


static const char CMD_TOKEN[3] = { 'C', 'M', 'D' };
static const uint32_t CMP_TOKEN = (0x434D5000UL);
static const uint32_t ERR_TOKEN = (0x45525200UL);
static const uint32_t PKT_TOKEN = (0x504B5400UL);


static bool usb_dma_ready (void) {
    return !((fpga_reg_get(REG_USB_DMA_SCR) & DMA_SCR_BUSY));
}

static bool usb_rx_byte (uint8_t *data) {
    if (fpga_usb_status_get() & USB_STATUS_RXNE) {
        *data = fpga_usb_pop();
        return true;
    }
    return false;
}

static bool usb_tx_byte (uint8_t data) {
    if (fpga_usb_status_get() & USB_STATUS_TXE) {
        fpga_usb_push(data);
        return true;
    }
    return false;
}

static uint8_t usb_rx_word_counter = 0;
static uint32_t usb_rx_word_buffer = 0;

static bool usb_rx_word (uint32_t *data) {
    uint8_t tmp;
    while (usb_rx_byte(&tmp)) {
        usb_rx_word_buffer = (usb_rx_word_buffer << 8) | tmp;
        usb_rx_word_counter += 1;
        if (usb_rx_word_counter == 4) {
            usb_rx_word_counter = 0;
            *data = usb_rx_word_buffer;
            usb_rx_word_buffer = 0;
            return true;
        }
    }
    return false;
}

static uint8_t usb_tx_word_counter = 0;

static bool usb_tx_word (uint32_t data) {
    while (usb_tx_byte(data >> ((3 - usb_tx_word_counter) * 8))) {
        usb_tx_word_counter += 1;
        if (usb_tx_word_counter == 4) {
            usb_tx_word_counter = 0;
            return true;
        }
    }
    return false;
}

static uint8_t usb_rx_cmd_counter = 0;

static bool usb_rx_cmd (uint8_t *cmd) {
    uint8_t data;
    while (usb_rx_byte(&data)) {
        if (usb_rx_cmd_counter == 3) {
            *cmd = data;
            usb_rx_cmd_counter = 0;
            return true;
        }
        if (data != CMD_TOKEN[usb_rx_cmd_counter++]) {
            usb_rx_cmd_counter = 0;
            return false;
        }
    }
    return false;
}

static bool usb_validate_address_length (uint32_t address, uint32_t length, bool exclude_bootloader) {
    if ((address >= MEMORY_LENGTH) || (length > MEMORY_LENGTH)) {
        return true;
    }
    if ((address + length) > MEMORY_LENGTH) {
        return true;
    }
    if (exclude_bootloader) {
        if (((address + length) > BOOTLOADER_ADDRESS) && (address < (BOOTLOADER_ADDRESS + BOOTLOADER_LENGTH))) {
            return true;
        }
    }
    return false;
}

static void usb_rx_process (void) {
    if (p.rx_state == RX_STATE_IDLE) {
        if (!p.response_pending && usb_rx_cmd(&p.rx_cmd)) {
            p.rx_state = RX_STATE_ARGS;
            p.rx_counter = 0;
            p.rx_dma_running = false;
            p.flush_response = false;
            p.flush_packet = false;
            p.response_error = false;
            p.response_info.cmd = p.rx_cmd;
            p.response_info.data_length = 0;
            p.response_info.dma_length = 0;
            p.response_info.done_callback = NULL;
            if (p.rx_cmd == 'U') {
                timer_set(TIMER_ID_USB, DEBUG_WRITE_TIMEOUT_TICKS);
            }
        }
    }

    if (p.rx_state == RX_STATE_ARGS) {
        while (usb_rx_word(&p.rx_args[p.rx_counter])) {
            p.rx_counter += 1;
            if (p.rx_counter == 2) {
                p.rx_counter = 0;
                p.rx_state = RX_STATE_DATA;
                break;
            }
        }
    }

    if (p.rx_state == RX_STATE_DATA) {
        switch (p.rx_cmd) {
            case 'v':
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                p.response_info.data_length = 4;
                p.response_info.data[0] = cfg_get_identifier();
                break;

            case 'V':
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                p.response_info.data_length = 8;
                version_firmware(&p.response_info.data[0], &p.response_info.data[1]);
                break;

            case 'R':
                cfg_reset_state();
                cic_reset_parameters();
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                break;

            case 'B':
                cic_set_parameters(p.rx_args);
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                break;

            case 'c':
                p.response_error = cfg_query(p.rx_args);
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                p.response_info.data_length = 4;
                p.response_info.data[0] = p.rx_args[1];
                break;

            case 'C':
                p.response_error = cfg_update(p.rx_args);
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                break;

            case 'a':
                p.response_error = cfg_query_setting(p.rx_args);
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                p.response_info.data_length = 4;
                p.response_info.data[0] = p.rx_args[1];
                break;

            case 'A':
                p.response_error = cfg_update_setting(p.rx_args);
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                break;

            case 't':
                cfg_get_time(p.rx_args);
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                p.response_info.data_length = 8;
                p.response_info.data[0] = p.rx_args[0];
                p.response_info.data[1] = p.rx_args[1];
                break;

            case 'T':
                cfg_set_time(p.rx_args);
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                break;

            case 'm':
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                if (usb_validate_address_length(p.rx_args[0], p.rx_args[1], false)) {
                    p.response_error = true;
                } else {
                    p.response_info.dma_address = p.rx_args[0];
                    p.response_info.dma_length = p.rx_args[1];
                }
                break;

            case 'M':
                if (usb_dma_ready()) {
                    if (!p.rx_dma_running) {
                        if (usb_validate_address_length(p.rx_args[0], p.rx_args[1], true)) {
                            p.rx_state = RX_STATE_FLUSH;
                            p.flush_response = true;
                        } else {
                            fpga_reg_set(REG_USB_DMA_ADDRESS, p.rx_args[0]);
                            fpga_reg_set(REG_USB_DMA_LENGTH, p.rx_args[1]);
                            fpga_reg_set(REG_USB_DMA_SCR, DMA_SCR_DIRECTION | DMA_SCR_START);
                            p.rx_dma_running = true;
                        }
                    } else {
                        p.rx_state = RX_STATE_IDLE;
                        p.response_pending = true;
                    }
                }
                break;

            case 'U':
                if (p.rx_args[1] == 0) {
                    p.rx_state = RX_STATE_IDLE;
                } else if (usb_dma_ready()) {
                    if (p.read_length > 0) {
                        uint32_t length = (p.read_length > p.rx_args[1]) ? p.rx_args[1] : p.read_length;
                        if (!p.rx_dma_running) {
                            fpga_reg_set(REG_USB_DMA_ADDRESS, p.read_address);
                            fpga_reg_set(REG_USB_DMA_LENGTH, length);
                            fpga_reg_set(REG_USB_DMA_SCR, DMA_SCR_DIRECTION | DMA_SCR_START);
                            p.rx_dma_running = true;
                            p.read_ready = false;
                        } else {
                            p.rx_args[1] -= length;
                            p.rx_dma_running = false;
                            p.read_length -= length;
                            p.read_address += length;
                            p.read_ready = true;
                            timer_set(TIMER_ID_USB, DEBUG_WRITE_TIMEOUT_TICKS);
                        }
                    } else if (timer_get(TIMER_ID_USB) == 0) {
                        p.rx_state = RX_STATE_FLUSH;
                        p.flush_packet = true;
                    }
                }
                break;

            case 'D':
                dd_set_block_ready(p.rx_args[0] == 0);
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                break;

            case 'W':
                writeback_enable(WRITEBACK_USB);
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                break;

            case 'p':
                if (p.rx_args[0]) {
                    flash_wait_busy();
                }
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                p.response_info.data_length = 4;
                p.response_info.data[0] = FLASH_ERASE_BLOCK_SIZE;
                break;

            case 'P':
                if (usb_validate_address_length(p.rx_args[0], FLASH_ERASE_BLOCK_SIZE, true)) {
                    p.response_error = true;
                } else {
                    p.response_error = flash_erase_block(p.rx_args[0]);
                }
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                break;

            case 'f':
                cfg_set_rom_write_enable(false);
                p.response_info.data[0] = update_backup(p.rx_args[0], &p.response_info.data[1]);
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                p.response_error = (p.response_info.data[0] != UPDATE_OK);
                p.response_info.data_length = 8;
                break;

            case 'F':
                cfg_set_rom_write_enable(false);
                p.response_info.data[0] = update_prepare(p.rx_args[0], p.rx_args[1]);
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                p.response_info.data_length = 4;
                if (p.response_info.data[0] == UPDATE_OK) {
                    p.response_info.done_callback = update_start;
                } else {
                    p.response_error = true;
                }
                break;

            case '?':
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                p.response_info.data_length = 8;
                p.response_info.data[0] = fpga_reg_get(REG_DEBUG_0);
                p.response_info.data[1] = fpga_reg_get(REG_DEBUG_1);
                break;

            case '%':
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                p.response_info.data_length = 16;
                app_get_stack_usage(p.response_info.data);
                break;

            default:
                p.rx_state = RX_STATE_IDLE;
                p.response_pending = true;
                p.response_error = true;
                p.response_info.data_length = 4;
                p.response_info.data[0] = 0xFFFFFFFF;
                break;
        }
    }

    if (p.rx_state == RX_STATE_FLUSH) {
        if (usb_dma_ready()) {
            if (p.rx_args[1] != 0) {
                uint32_t length = (p.rx_args[1] > RX_FLUSH_LENGTH) ? RX_FLUSH_LENGTH : p.rx_args[1];
                fpga_reg_set(REG_USB_DMA_ADDRESS, RX_FLUSH_ADDRESS);
                fpga_reg_set(REG_USB_DMA_LENGTH, length);
                fpga_reg_set(REG_USB_DMA_SCR, DMA_SCR_DIRECTION | DMA_SCR_START);
                p.rx_args[1] -= length;
            } else {
                if (p.flush_response) {
                    p.rx_state = RX_STATE_IDLE;
                    p.response_pending = true;
                    p.response_error = true;
                } else if (p.flush_packet) {
                    usb_tx_info_t packet_info;
                    usb_create_packet(&packet_info, PACKET_CMD_DATA_FLUSHED);
                    if (usb_enqueue_packet(&packet_info)) {
                        p.rx_state = RX_STATE_IDLE;
                    }
                } else {
                    p.rx_state = RX_STATE_IDLE;
                }
            }
        }
    }
}

static void usb_tx_process (void) {
    if (p.tx_state == TX_STATE_IDLE) {
        if (p.response_pending) {
            p.response_pending = false;
            p.tx_state = TX_STATE_TOKEN;
            p.tx_counter = 0;
            p.tx_info = p.response_info;
            p.tx_token = p.response_error ? ERR_TOKEN : CMP_TOKEN;
            p.tx_dma_running = false;
        } else if (p.packet_pending) {
            p.packet_pending = false;
            p.tx_state = TX_STATE_TOKEN;
            p.tx_counter = 0;
            p.tx_info = p.packet_info;
            p.tx_token = PKT_TOKEN;
            p.tx_dma_running = false;
        }
    }

    if (p.tx_state == TX_STATE_TOKEN) {
        if (p.tx_counter == 0) {
            if (usb_tx_word(p.tx_token | p.tx_info.cmd)) {
                p.tx_counter += 1;
            }
        }
        if (p.tx_counter == 1) {
            if (usb_tx_word(p.tx_info.data_length + p.tx_info.dma_length)) {
                p.tx_state = TX_STATE_DATA;
                p.tx_counter = 0;
            }
        }
    }

    if (p.tx_state == TX_STATE_DATA) {
        if (p.tx_info.data_length > 0) {
            while (usb_tx_word(p.tx_info.data[p.tx_counter])) {
                p.tx_counter += 1;
                if (p.tx_counter == (p.tx_info.data_length / 4)) {
                    p.tx_state = TX_STATE_DMA;
                    p.tx_counter = 0;
                    break;
                }
            }
        } else {
            p.tx_state = TX_STATE_DMA;
        }
    }

    if (p.tx_state == TX_STATE_DMA) {
        if (p.tx_info.dma_length > 0) {
            if (usb_dma_ready()) {
                if (!p.tx_dma_running) {
                    p.tx_dma_running = true;
                    fpga_reg_set(REG_USB_DMA_ADDRESS, p.tx_info.dma_address);
                    fpga_reg_set(REG_USB_DMA_LENGTH, p.tx_info.dma_length);
                    fpga_reg_set(REG_USB_DMA_SCR, DMA_SCR_START);
                } else {
                    p.tx_state = TX_STATE_FLUSH;
                }
            }
        } else {
            p.tx_state = TX_STATE_FLUSH;
        }
    }

    if (p.tx_state == TX_STATE_FLUSH) {
        fpga_reg_set(REG_USB_SCR, USB_SCR_WRITE_FLUSH);
        if (p.tx_info.done_callback) {
            p.tx_info.done_callback();
        }
        p.tx_state = TX_STATE_IDLE;
    }
}


void usb_create_packet (usb_tx_info_t *info, usb_packet_cmd_e cmd) {
    info->cmd = (uint8_t) (cmd);
    info->data_length = 0;
    for (int i = 0; i < 4; i++) {
        info->data[i] = 0;
    }
    info->dma_length = 0;
    info->dma_address = 0;
    info->done_callback = NULL;
}

bool usb_enqueue_packet (usb_tx_info_t *info) {
    if (p.packet_pending) {
        return false;
    }
    p.packet_pending = true;
    p.packet_info = *info;
    return true;
}

bool usb_prepare_read (uint32_t *args) {
    if (!p.read_ready) {
        return false;
    }
    p.read_length = args[1];
    p.read_address = args[0];
    return true;
}

void usb_get_read_info (uint32_t *args) {
    uint32_t scr = fpga_reg_get(REG_USB_SCR);
    args[0] = 0;
    args[1] = 0;
    if (p.rx_state == RX_STATE_DATA && p.rx_cmd == 'U') {
        args[0] = p.rx_args[0] & 0xFF;
        args[1] = p.rx_args[1];
    }
    args[0] |= (p.read_length > 0) ? (1 << 31) : 0;
    args[0] |= (scr & USB_SCR_RESET_STATE) ? (1 << 30) : 0;
    args[0] |= (scr & USB_SCR_PWRSAV) ? (1 << 29) : 0;
}

void usb_init (void) {
    fpga_reg_set(REG_USB_DMA_SCR, DMA_SCR_STOP);
    fpga_reg_set(REG_USB_SCR, USB_SCR_FIFO_FLUSH);

    p.rx_state = RX_STATE_IDLE;
    p.tx_state = TX_STATE_IDLE;

    p.response_pending = false;
    p.packet_pending = false;

    p.read_ready = true;
    p.read_length = 0;
    p.read_address = 0;

    usb_rx_word_counter = 0;
    usb_rx_word_buffer = 0;
    usb_tx_word_counter = 0;
    usb_rx_cmd_counter = 0;
}

void usb_process (void) {
    uint32_t scr = fpga_reg_get(REG_USB_SCR);
    if (scr & (USB_SCR_PWRSAV | USB_SCR_RESET_STATE | USB_SCR_RESET_PENDING)) {
        if (p.packet_pending && p.packet_info.done_callback) {
            p.packet_pending = false;
            p.packet_info.done_callback();
        }
        if (scr & USB_SCR_RESET_PENDING) {
            if (p.tx_state != TX_STATE_IDLE && p.tx_info.done_callback) {
                p.tx_info.done_callback();
            }
            usb_init();
            fpga_reg_set(REG_USB_SCR, USB_SCR_RESET_ACK);
        }
    } else {
        usb_rx_process();
        usb_tx_process();
    }
}
