#include "drivers/st7789.h"
#include "esp_log.h"

#define CMD(x)    st7789_send_cmd(x)
#define DATA(x,y) st7789_send_data(x,y)
#define BYTE(x)   st7789_send_data_byte(x)
#define WAIT(x)   st7789_wait(x)

#define WIDTH     240
#define HEIGHT    240
#define BPP       8
#define FB_SIZE   ((BPP*WIDTH*HEIGHT)/8)
#define FB_CHUNK_SIZE (ST779_PARALLEL_LINES * (BPP*WIDTH)/8)

#define FB_PIXCHUNK ((uint32_t *)(&framebuffer[fb_blk_off]))
#define FB_PIXCHUNK2 ((uint32_t *)(&framebuffer[fb_blk_off+4]))

#define MIX_ALPHA(x,y,a) ((x*(15-a) + (y*a))/15)

#define P1MASK    0xFFFF0F00
#define P1MASKP   0x0000F0FF
#define P2MASK    0xFF00F0FF
#define P2MASKP   0x00FF0F00

spi_device_handle_t spi;
ledc_timer_config_t backlight_timer = {
  .duty_resolution = LEDC_TIMER_13_BIT, // resolution of PWM duty
  .freq_hz = 5000,                      // frequency of PWM signal
#if SOC_LEDC_SUPPORT_HS_MODE
  .speed_mode = LEDC_HIGH_SPEED_MODE,   // timer mode
#else
  .speed_mode = LEDC_LOW_SPEED_MODE,
#endif
  .timer_num = LEDC_TIMER_0,            // timer index
  .clk_cfg = LEDC_AUTO_CLK,             // Auto select the source clock
};

ledc_channel_config_t backlight_config = {
  .channel    = LEDC_CHANNEL_0,
  .duty       = 0,
  .gpio_num   = ST7789_BL_IO,
#if SOC_LEDC_SUPPORT_HS_MODE
  .speed_mode = LEDC_HIGH_SPEED_MODE,
#else
  .speed_mode = LEDC_LOW_SPEED_MODE,
#endif
  .hpoint     = 0,
  .timer_sel  = LEDC_TIMER_0
};

RTC_DATA_ATTR static bool g_inv_x = true;
RTC_DATA_ATTR static bool g_inv_y = true;

/* Drawing window. */
DRAM_ATTR static int g_dw_x0 = 0;
DRAM_ATTR static int g_dw_y0 = 0;
DRAM_ATTR static int g_dw_x1 = WIDTH - 1;
DRAM_ATTR static int g_dw_y1 = HEIGHT - 1;

__attribute__ ((aligned(4)))
DRAM_ATTR static uint8_t databuf[16];

/* Framebuffer. We need one more byte to handle pixels with 32-bit values. */
__attribute__ ((aligned(4)))
DRAM_ATTR static uint8_t framebuffer[FB_SIZE];

/* Frame chunk. We need to upscale our 8bpp pixels to 16 bpp before sending them. */
DRAM_ATTR static uint16_t framechunk[FB_CHUNK_SIZE];

static uint16_t COLOR_LUT[64]={
  0x0000, 0x000a, 0x0014, 0x001f, 0x0540, 0x054a, 0x0554, 0x055f,
  0x0a80, 0x0a8a, 0x0a94, 0x0a9f, 0x0fc0, 0x0fca, 0x0fd4, 0x0fdf,
  0x5000, 0x500a, 0x5014, 0x501f, 0x5540, 0x554a, 0x5554, 0x555f,
  0x5a80, 0x5a8a, 0x5a94, 0x5a9f, 0x5fc0, 0x5fca, 0x5fd4, 0x5fdf,
  0xa000, 0xa00a, 0xa014, 0xa01f, 0xa540, 0xa54a, 0xa554, 0xa55f,
  0xaa80, 0xaa8a, 0xaa94, 0xaa9f, 0xafc0, 0xafca, 0xafd4, 0xafdf,
  0xf800, 0xf80a, 0xf814, 0xf81f, 0xfd40, 0xfd4a, 0xfd54, 0xfd5f,
  0xfa80, 0xfa8a, 0xfa94, 0xfa9f, 0xffc0, 0xffca, 0xffd4, 0xffdf
};


typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes; //No of data in data; bit 7 = delay after set; 0xFF = end of cmds.
} init_cmd_t;

DRAM_ATTR static const init_cmd_t st_init_cmds[]={
  {ST7789_CMD_SLPOUT, {0}, 0},
  {ST7789_CMD_WAIT, {0}, 50}, // was 250ms
  {ST7789_CMD_COLMOD, {0x05}, 1}, /* COLMOD: 16 bits per pixel, 65K colors */
  {ST7789_CMD_WAIT, {0}, 10},
  {ST7789_CMD_CASET, {0x00, 0x00, 0x00, 0xF0}, 4},
  {ST7789_CMD_RASET, {0x00, 0x00, 0x00, 0xF0}, 4},
  {ST7789_CMD_INVON, {0x00}, 0},
  {ST7789_CMD_WAIT, {0}, 10},
  {ST7789_CMD_NORON, {0x00}, 0},
  {ST7789_CMD_WAIT, {0}, 10},
  {ST7789_CMD_DISPON, {0x00}, 0},
  //{ST7789_CMD_WAIT, {0}, 250},
  {0,{0}, 0xff}
};

/**
 * @brief Wait for given milliseconds
 * @param milliseconds: nimber of milliseconds to wait
 **/

void st7789_wait(int milliseconds)
{
  vTaskDelay(milliseconds/portTICK_PERIOD_MS);
}

void st7789_pre_transfer_callback(spi_transaction_t *t)
{
    int dc=(int)t->user;
    gpio_set_level(ST7789_SPI_DC_IO, dc);
}

esp_err_t IRAM_ATTR st7789_send_cmd(const uint8_t cmd)
{
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));       //Zero out the transaction
    t.length=8;                     //Command is 8 bits
    t.tx_buffer=&cmd;               //The data is the cmd itself
    t.user=(void*)0;                //D/C needs to be set to 0
    ret = spi_device_polling_transmit(spi, &t);  //Transmit!
    assert(ret==ESP_OK);
    return ret;
}

esp_err_t IRAM_ATTR st7789_send_data(const uint8_t *data, int len)
{
    esp_err_t ret;
    spi_transaction_t t;
    if (len==0) return ESP_FAIL;             //no need to send anything
    memset(&t, 0, sizeof(t));       //Zero out the transaction
    t.length=len*8;                 //Len is in bytes, transaction length is in bits.
    t.tx_buffer=data;               //Data
    t.user=(void*)1;                //D/C needs to be set to 1
    ret = spi_device_polling_transmit(spi, &t);  //Transmit!
    assert(ret==ESP_OK);
    return ret;
}

esp_err_t IRAM_ATTR st7789_send_data_byte(const uint8_t byte)
{
  return st7789_send_data(&byte, 1);
}


void st7789_init_display(void)
{
  int cmd = 0;

  /* Execute initialization sequence. */
  while (st_init_cmds[cmd].databytes!=0xff) {
    if (st_init_cmds[cmd].cmd == ST7789_CMD_WAIT)
    {
      vTaskDelay(st_init_cmds[cmd].databytes / portTICK_PERIOD_MS);
    }
    else
    {
      st7789_send_cmd(st_init_cmds[cmd].cmd);
      st7789_send_data(st_init_cmds[cmd].data, st_init_cmds[cmd].databytes&0x1F);
    }
    cmd++;
  }
}


esp_err_t st7789_init_backlight(void)
{
  esp_rom_gpio_pad_select_gpio(ST7789_BL_IO);
  gpio_set_direction(ST7789_BL_IO, GPIO_MODE_OUTPUT);
  
  /* Configure backlight for PWM (light control) */
  if (ledc_timer_config(&backlight_timer) != ESP_OK)
    ESP_LOGE("st7789", "oops, timer error");
  ledc_channel_config(&backlight_config);

  /* Set default duty cycle (0, backlight is off). */
  ledc_set_duty(backlight_config.speed_mode, backlight_config.channel, 0);
  ledc_update_duty(backlight_config.speed_mode, backlight_config.channel);

  return ESP_OK;
}

/**
 * @brief Initializes the ST7789 display
 * @retval ESP_OK on success, ESP_FAIL otherwise.
 **/

esp_err_t st7789_init(void)
{
  spi_bus_config_t bus_config = {
        .miso_io_num=-1,
        .mosi_io_num=ST7789_SPI_MOSI_IO,
        .sclk_io_num=ST7789_SPI_SCLK_IO,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1,
        .max_transfer_sz=ST779_PARALLEL_LINES*240*2+8,
    };

  spi_device_interface_config_t devcfg={
        .clock_speed_hz=ST7789_SPI_SPEED,
        .mode=0,
        .spics_io_num=ST7789_SPI_CS_IO,
        .queue_size=7,                          //We want to be able to queue 7 transactions at a time
        .pre_cb=st7789_pre_transfer_callback,  //Specify pre-transfer callback to handle D/C line
        .flags=/*SPI_DEVICE_HALFDUPLEX|*/SPI_DEVICE_NO_DUMMY
    };


  /* Initialize our SPI interface. */
  if (spi_bus_initialize(HSPI_HOST, &bus_config, ST7789_DMA_CHAN) == ESP_OK)
  {
      if (spi_bus_add_device(HSPI_HOST, &devcfg, &spi) == ESP_OK)
      {
        /* Initialize GPIOs */
        //gpio_pad_select_gpio(ST7789_BL_IO);
        esp_rom_gpio_pad_select_gpio(ST7789_SPI_DC_IO);
        esp_rom_gpio_pad_select_gpio(ST7789_SPI_CS_IO);
        //gpio_set_direction(ST7789_BL_IO, GPIO_MODE_OUTPUT);
        gpio_set_direction(ST7789_SPI_DC_IO, GPIO_MODE_OUTPUT);
        gpio_set_direction(ST7789_SPI_CS_IO, GPIO_MODE_OUTPUT);

#if 0
        /* Configure backlight for PWM (light control) */
        if (ledc_timer_config(&backlight_timer) != ESP_OK)
          ESP_LOGE("st7789", "oops, timer error");
        ledc_channel_config(&backlight_config);

        /* Set default duty cycle (0, backlight is off). */
        ledc_set_duty(backlight_config.speed_mode, backlight_config.channel, 0);
        ledc_update_duty(backlight_config.speed_mode, backlight_config.channel);
#endif
        /* Send init commands. */
        st7789_init_display();

        return ESP_OK;
      }
      else
        return ESP_FAIL;
  }
  else
    return ESP_FAIL;
}


/**
 * @brief Set screen backlight to max.
 **/

void st7789_backlight_on(void)
{
  ledc_set_duty(backlight_config.speed_mode, backlight_config.channel, 5000);
  ledc_update_duty(backlight_config.speed_mode, backlight_config.channel);
}


/**
 * @brief Set screen backlight level.
 * @param backlight_level, from 0 to 5000
 **/

void st7789_backlight_set(int backlight_level)
{
  ledc_set_duty(backlight_config.speed_mode, backlight_config.channel, backlight_level);
  ledc_update_duty(backlight_config.speed_mode, backlight_config.channel);
}

/**
 * @brief Get screen backlight level.
 * @return backlight level from 0 to 5000
 **/

int st7789_backlight_get()
{
  return ledc_get_duty(backlight_config.speed_mode, backlight_config.channel);
}

void st7789_set_drawing_window(int x0, int y0, int x1, int y1)
{
  int x,y;


  /* Ensure x0 < x1, y0 < y1. */
  x = (x0<x1)?x0:x1;
  y = (y0<y1)?y0:y1;

  if (x<0)
    x = 0;
  if (y<0)
    y = 0;

  g_dw_x1 = (x0>x1)?x0:x1;
  g_dw_y1 = (y0>y1)?y0:y1;

  if (g_dw_x1 > (WIDTH -1))
    g_dw_x1 = WIDTH - 1;
  if (g_dw_y1 > (HEIGHT -1))
    g_dw_y1 = HEIGHT - 1;

  g_dw_x0 = x;
  g_dw_y0 = y;
}

void st7789_get_drawing_window(int *x0, int *y0, int *x1, int *y1)
{
  *x0 = g_dw_x0;
  *y0 = g_dw_y0;
  *x1 = g_dw_x1;
  *y1 = g_dw_y1;
}

void st7789_set_window(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
  databuf[0] = x0 >> 8;
  databuf[1] = x0 & 0xFF;
  databuf[2] = x1 >> 8;
  databuf[3] = x1 & 0xFF;
  CMD(ST7789_CMD_CASET);
  DATA(databuf, 4);
  databuf[0] = y0 >> 8;
  databuf[1] = y0 & 0xFF;
  databuf[2] = y1 >> 8;
  databuf[3] = y1 & 0xFF;
  CMD(ST7789_CMD_RASET);
  DATA(databuf, 4);
  CMD(ST7789_CMD_RAMWR);
}

void st7789_set_fb(uint8_t *frame)
{
  memcpy(framebuffer, frame, FB_SIZE);
}


/**
 * @brief Send framebuffer to screen.
 **/

void st7789_commit_fb(void)
{
  int i, j;
  uint8_t pix;
  st7789_set_window(0, 0, WIDTH, HEIGHT);
  for (i=0; i<(FB_SIZE/FB_CHUNK_SIZE); i++)
  {
    /* 
      Upscale pixels from 8bpp to 16bpp.
      RGB 2-2-2 -> RGB 5-6-5

      For each color component, 2 bits to 5 or 6 bits.
    */
    for (j=0; j<FB_CHUNK_SIZE; j++)
    {
      pix = framebuffer[i*FB_CHUNK_SIZE+j];
      framechunk[j] = COLOR_LUT[pix & 0x3F];
    }

    st7789_send_data((uint8_t *)&framechunk, FB_CHUNK_SIZE*2);
  }
}


/**
 * @brief Fill screen with default color (black)
 **/

void st7789_blank(void)
{
  memset(framebuffer, 0, FB_SIZE);
}


/**
 * @brief Get color of a given pixel in framebuffer
 * @param x: pixel X coordinate
 * @param y: pixel Y coordinate
 * @return: pixel color (12 bits)
 **/

uint8_t _st7789_get_pixel(int x, int y)
{
  /* Sanity checks. */
  if ((x < g_dw_x0) || (x > g_dw_x1) || (y<g_dw_y0) || (y>g_dw_y1))
    return 0;

  /* Return color. */  
  return framebuffer[y*WIDTH + x];
}


/**
 * @brief Get color of a given pixel in framebuffer
 * @param x: pixel X coordinate
 * @param y: pixel Y coordinate
 * @return: pixel color (12 bits)
 **/

uint8_t st7789_get_pixel(int x, int y)
{
  uint8_t pix = 0;

  /* Sanity checks. */
  if ((x < g_dw_x0) || (x > g_dw_x1) || (y<g_dw_y0) || (y>g_dw_y1))
    return pix;

  /* Invert if required. */
  if (g_inv_x)
    x = WIDTH - x - 1;
  if (g_inv_y)
    y = HEIGHT - y - 1;

  return _st7789_get_pixel(x, y);
}


/**
 * @brief Set a pixel color in framebuffer
 * @param x: pixel X coordinate
 * @param y: pixel Y coordinate
 * @param color: pixel color (12 bits)
 **/

void st7789_set_pixel(int x, int y, uint8_t color)
{
  /* Sanity checks. */
  if ((x < g_dw_x0) || (x > g_dw_x1) || (y<g_dw_y0) || (y>g_dw_y1))
    return;
  
  /* Invert if required. */
  if (g_inv_x)
    x = WIDTH - x - 1;
  if (g_inv_y)
    y = HEIGHT - y - 1;

  /* Set pixel color. */
  framebuffer[y*WIDTH + x] = color;
}

/**
 * @brief Set a pixel color in framebuffer
 * @param x: pixel X coordinate
 * @param y: pixel Y coordinate
 * @param color: pixel color (12 bits)
 **/

void _st7789_set_pixel(int x, int y, uint8_t color)
{
  /* Sanity checks. */
  if ((x < g_dw_x0) || (x > g_dw_x1) || (y<g_dw_y0) || (y>g_dw_y1))
    return;

  /* Set pixel color. */
  framebuffer[y*WIDTH + x] = color;
}


/**
 * @brief Fills a region of the screen with a specific color
 * @param x: top-left X coordinate
 * @param y: top-left Y coordinate
 * @param width: region width
 * @param height: region height
 * @param color: 8 bpp color
 **/

void st7789_fill_region(int x, int y, int width, int height, uint8_t color)
{
  int _y;

  /* X and y cannot be less than zero. */
  if (x<g_dw_x0)
  {
    /* Fix width, exit if region is out of screen. */
    width -= (g_dw_x0 - x);
    if (width <= 0)
      return;
    x = g_dw_x0;
  }

  if (y<g_dw_y0)
  {
    /* Fix height, exit if region is out of screen. */
    height -= (g_dw_y0 - y);
    if (height <= 0)
      return;
    y = g_dw_y0;
  }

  /* Region must not exceed screen size. */
  if ((x+width) > g_dw_x1)
    width = g_dw_x1-x;
  if ((y+height) > g_dw_y1)
    height = g_dw_y1-y;

  if (width>0)
  {
    for (_y=y; _y<(y+height); _y++)
    {
      /* Otherwise use fast line drawing routine. */
      st7789_draw_fastline(x, _y, x+width-1, color);
    }
  }
}


/**
 * @brief Draw fast an horizontal line of color `color` between (x0,y) and (x1, y)
 * @param x0: X coordinate of the start of the line
 * @param y: Y cooordinate of the start of the line
 * @param x1: X coordinate of the end of the line
 * @param color: line color.
 **/

void st7789_draw_fastline(int x0, int y, int x1, uint8_t color)
{
  int _x0,_x1,_y;
  int n=0;

  if (g_inv_x)
  {
    _x0 = WIDTH - x0 - 1;
    _x1 = WIDTH - x1 - 1;

  }
  else
  {
    _x0 = x0;
    _x1 = x1;
  }

  /* Reorder. */
  if (_x0>_x1)
  {
    n=_x1;
    _x1 = _x0;
    _x0 = n;
  }


  if (g_inv_y)
    _y = HEIGHT - y - 1;
  else
    _y = y;

  /* Fill line of pixels. */
  memset(&framebuffer[_y*WIDTH+_x0], color, _x1 -_x0 + 1);
}


/**
 * @brief Copy line p_line to the output position
 * @param x: X coordinate of the start of the line
 * @param y: Y coordinate of the start of the line
 * @param p_line: pointer to an array of colors (uint16_t)
 * @param nb_pixels: number of pixels to copy
 **/

void st7789_copy_line(int x, int y, uint8_t *p_line, int nb_pixels)
{
  int _x,_y;
  uint8_t *p_dst,*p_src;
  int offset=0, i;

  /* If Y coordinate does not belong to our drawing window, no need to draw. */
  if ((y < g_dw_y0) || (y > g_dw_y1) )
    return;

  /* Don't draw if no pixel falls in the drawing window. */
  if ( ((x + nb_pixels) < g_dw_x0) || (x > (g_dw_x1)) )
    return;

  /* Apply our drawing window */
  if (x < g_dw_x0)
  {
    offset = (g_dw_x0 - x);
    nb_pixels -= offset;
    x = g_dw_x0;
  }

  /* Fix number of pixels to draw if line goes beyond our drawing window. */
  if ((x + nb_pixels) > g_dw_x1)
  {
    nb_pixels = (g_dw_x1 - x);
  }
  //printf("nb_pixels: %d\n", nb_pixels);

  /* Invert X coordinate if required. */
  if (g_inv_x)
    _x = WIDTH - x - 1;
  else
    _x=x;

  /* Invert Y coordinate if required. */
  if (g_inv_y)
    _y = HEIGHT - y - 1;
  else
    _y=y;

  /* Copy line. */
  if (g_inv_x)
  {
    p_dst = &framebuffer[_y*WIDTH + _x];
    p_src = p_line + offset;

    for (i=0; i<nb_pixels; i++)
    {
      *(p_dst--) = *p_src++;
    }
  }
  else
  {
    p_dst = &framebuffer[_y*WIDTH + _x];
    p_src = p_line + offset;

    for (i=0; i<nb_pixels; i++)
    {
      *(p_dst++) = *p_src++;
    }
  }
}

/**
 * @brief Draw a line of color `color` between (x0,y0) and (x1, y1)
 * @param x0: X coordinate of the start of the line
 * @param y0: Y cooordinate of the start of the line
 * @param x1: X coordinate of the end of the line
 * @param y1: y coordinate of the end of the line
 **/
void st7789_draw_line(int x0, int y0, int x1, int y1, uint8_t color)
{
  int x, y, dx, dy;
  float e;

  dy = y1 - y0;
  dx = x1 - x0;

  /* Vertical line ? */
  if (dx == 0)
  {
    /* Make sure y0 <= y1. */
    if (y0>y1)
    {
      dy = y0;
      y0 = y1;
      y1 = dy;
    }

    for (y=y0; y<=y1; y++)
      st7789_set_pixel(x0, y, color);
  }
  else
  {
    /* Horizontal line ? */
    if (dy == 0)
    {
      /* Make sure x0 <= x1. */
      if (x0>x1)
      {
        dx = x0;
        x0 = x1;
        x1 = dx;
      }

      /*
       * Use st7789_fill_region() rather than st7789_draw_fastline()
       * as it will check boundaries, adapt coordinates and relies
       * on fast line drawing.
       * 
       * If color alpha channel is set, use a classic for loop to
       * set pixels. We cannot use the fast way as we need to blend
       * pixels.
       */
      st7789_fill_region(x0, y0, x1 - x0 + 1, 1, color);
    }
    else
    {

      /*
       * Make sure x0 <= x1 and dx >= 0. Swaping (x0,y0) with (x1,y1)
       * remove 4 case by symmetry
       */
      if (x0 > x1)
      {
        x = x0;
        x0 = x1;
        x1 = x;
        y = y0;
        y0 = y1;
        y1 = y;
        dy = -dy;
        dx = -dx;
      }

      if (dy > 0)
      {
        if (dx >= dy) /* 1st quadran */
        {
          e = dx;
          dx *= 2;
          dy *= 2;
          for (; x0 <= x1; x0++)
          {
            st7789_set_pixel(x0, y0, color);
            e -= dy;
            if (e < 0) {
              y0++;
              e += dx;
            }
          }
        }
        else /* 2nd quadran */
        {
          e = dy;
          dx *= 2;
          dy *= 2;
          for (;y0 <= y1; y0++)
          {
            st7789_set_pixel(x0, y0, color);
            e -= dx;
            if (e < 0) {
              x0++;
              e += dy;
            }
          }
        }
      }
      else /* dy < 0 */
      {
        if (dx >= -dy) /* 3rd quadran */
        {
          e = dx;
          dx *= 2;
          dy *= 2;
          for (; x0 <= x1; x0++)
          {
            st7789_set_pixel(x0, y0, color);
            e += dy;
            if (e < 0) {
              y0--;
              e += dx;
            }
          }
        }
        else /* 4th quadran */
        {
          e = dy;
          dx *= 2;
          dy *= 2;
          for (;y0 >= y1; y0--)
          {
            st7789_set_pixel(x0, y0, color);
            e += dx;
            if (e > 0) {
              x0++;
              e += dy;
            }
          }
        }
      }
    }
  }
}


/**
 * st7789_draw_circle()
 * 
 * @brief: Draw a circle of a given radius at given coordinates
 * @param xc: circle center X coordinate
 * @param yc: circle center Y coordinate
 * @param r: circle radius
 * @param color: circle color
 **/

void st7789_draw_circle(int xc, int yc, int r, uint8_t color)
{
  int x = 0;
  int y = r;
  int d = r - 1;

  /* Does not support alpha channel. */
  color &= 0x0fff;

  while (y >= x)
  {
    st7789_set_pixel(xc + x, yc + y, color);
    st7789_set_pixel(xc + y, yc + x, color);
    st7789_set_pixel(xc - x, yc + y, color);
    st7789_set_pixel(xc - y, yc + x, color);
    st7789_set_pixel(xc + x, yc - y, color);
    st7789_set_pixel(xc + y, yc - x, color);
    st7789_set_pixel(xc - x, yc - y, color);
    st7789_set_pixel(xc - y, yc - x, color);

    if (d >= (2*x))
    {
      d = d - 2*x - 1;
      x++;
    }
    else if (d < (2*(r-y)))
    {
      d = d + 2*y - 1;
      y--;
    }
    else
    {
      d = d + 2*(y - x - 1);
      y--;
      x++;
    }
  }
}


/**
 * st7789_draw_disc()
 * 
 * @brief: Draw a disc of a given radius at given coordinates
 * @param xc: disc center X coordinate
 * @param yc: disc center Y coordinate
 * @param r: disc radius
 * @param color: disc color
 **/

void st7789_draw_disc(int xc, int yc, int r, uint8_t color)
{
  int i;

  for (i=0;i<r;i++)
  {
    if (i==0)
      st7789_set_pixel(xc, yc, color);
    else
      st7789_draw_circle(xc, yc, i, color);
  }
}


/**
 * st7789_set_inverted()
 * 
 * @brief: Invert the screen (or not).
 * @param inverted: true to invert, false to use standard mode.
 **/

void st7789_set_inverted(bool inverted)
{
  g_inv_x = !inverted;
  g_inv_y = !inverted;
}


/**
 * st7789_is_inverted()
 * 
 * @brief: Check if screen is inverted (rotated) or not.
 * @return: true if inverted, false otherwise.
 **/

bool st7789_is_inverted(void)
{
  return ((!g_inv_x) && (!g_inv_y));
}
