/***
 * Required libraries:
 * Arduino_GFX: https://github.com/moononournation/Arduino_GFX.git
 * avilib: https://github.com/lanyou1900/avilib.git
 * libhelix: https://github.com/pschatzmann/arduino-libhelix.git
 * JPEGDEC: https://github.com/bitbank2/JPEGDEC.git
 */

const char *root = "/root";
const char *avi_file = "/root/AviMp3Mjpeg320p15fps.avi";

#include <WiFi.h>

#include <FFat.h>
#include <LittleFS.h>
#include <SD_MMC.h>

extern "C"
{
#include <avilib.h>
}

#include "AD35_S3_PINS.h"

#include <JPEGDEC.h>
JPEGDEC jpegdec;

/* variables */
static avi_t *a;
static long frames, estimateBufferSize, aRate, aBytes, aChunks, actual_video_size;
static long w, h, aChans, aBits, aFormat;
static double fr;
static char *compressor;
static char *vidbuf;
static char *audbuf;
static size_t audbuf_read;
static size_t audbuf_remain = 0;
static size_t output_buf_size;
static bool isStopped = true;
static long curr_frame = 0;
static long skipped_frames = 0;
static unsigned long start_ms, curr_ms, next_frame_ms;
static unsigned long total_read_video_ms = 0;
static unsigned long total_decode_video_ms = 0;
static unsigned long total_show_video_ms = 0;

#include "esp32_audio.h"

// drawing callback
static int drawMCU(JPEGDRAW *pDraw)
{
  // Serial.printf("Draw pos = (%d, %d), size = %d x %d\n", pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight);
  unsigned long s = millis();
  gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  total_show_video_ms += millis() - s;
  return 1;
}

void setup()
{
  WiFi.mode(WIFI_OFF);

  Serial.begin(115200);
  // Serial.setDebugOutput(true);
  // while(!Serial);
  Serial.println("AviMp3Mjpeg_JPEGDEC");

#ifdef GFX_EXTRA_PRE_INIT
  GFX_EXTRA_PRE_INIT();
#endif

  // Init Display
  if (!gfx->begin())
  {
    Serial.println("gfx->begin() failed!");
  }
  gfx->fillScreen(BLACK);

#ifdef GFX_BL
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
#endif

  if (!FFat.begin(false, root))
  // if (!LittleFS.begin(false, root))
  // pinMode(SD_CS /* CS */, OUTPUT);
  // digitalWrite(SD_CS /* CS */, HIGH);
  // SD_MMC.setPins(SD_SCK /* CLK */, SD_MOSI /* CMD/MOSI */, SD_MISO /* D0/MISO */);
  // if (!SD_MMC.begin(root, true /* mode1bit */, false /* format_if_mount_failed */, SDMMC_FREQ_DEFAULT))
  {
    Serial.println("ERROR: File system mount failed!");
  }
  else
  {
    a = AVI_open_input_file(avi_file, 1);

    if (a)
    {
      frames = AVI_video_frames(a);
      w = AVI_video_width(a);
      h = AVI_video_height(a);
      fr = AVI_frame_rate(a);
      compressor = AVI_video_compressor(a);
      estimateBufferSize = w * h * 2 / 7;
      Serial.printf("AVI frames: %ld, %ld x %ld @ %.2f fps, format: %s, estimateBufferSize: %ld, ESP.getFreeHeap(): %ld\n", frames, w, h, fr, compressor, estimateBufferSize, (long)ESP.getFreeHeap());

      aChans = AVI_audio_channels(a);
      aBits = AVI_audio_bits(a);
      aFormat = AVI_audio_format(a);
      aRate = AVI_audio_rate(a);
      aBytes = AVI_audio_bytes(a);
      aChunks = AVI_audio_chunks(a);
      Serial.printf("Audio channels: %ld, bits: %ld, format: %ld, rate: %ld, bytes: %ld, chunks: %ld\n", aChans, aBits, aFormat, aRate, aBytes, aChunks);

      vidbuf = (char *)heap_caps_malloc(estimateBufferSize, MALLOC_CAP_8BIT);
      if (!vidbuf)
      {
        vidbuf = (char *)malloc(estimateBufferSize);
        if (!vidbuf)
        {
          Serial.println("vidbuf malloc failed!");
        }
      }

      audbuf = (char *)heap_caps_malloc(MP3_MAX_FRAME_SIZE, MALLOC_CAP_8BIT);
      if (!audbuf)
      {
        audbuf = (char *)malloc(MP3_MAX_FRAME_SIZE);
        if (!audbuf)
        {
          Serial.println("audbuf malloc failed!");
        }
      }

#ifdef AUDIO_EXTRA_PRE_INIT
      AUDIO_EXTRA_PRE_INIT();
#endif

      i2s_init(I2S_NUM_0,
               aRate /* sample_rate */,
               I2S_MCLK, /*!< MCK in out pin. Note that ESP32 supports setting MCK on GPIO0/GPIO1/GPIO3 only*/
               I2S_BCLK, /*!< BCK in out pin*/
               I2S_LRCK, /*!< WS in out pin*/
               I2S_DOUT, /*!< DATA out pin*/
               I2S_DIN   /*!< DATA in pin*/
      );
      i2s_zero_dma_buffer(I2S_NUM_0);

      isStopped = false;
      start_ms = millis();
      next_frame_ms = start_ms + ((curr_frame + 1) * 1000 / fr);

      Serial.println("Play AVI start");
      curr_ms = millis();
      start_ms = curr_ms;

      audbuf_read = AVI_read_audio(a, audbuf, MP3_MAX_FRAME_SIZE);
      audbuf_remain = audbuf_read;
      total_read_audio_ms += millis() - curr_ms;
      curr_ms = millis();

      Serial.println("Start play audio task");
      BaseType_t ret_val = mp3_player_task_start(a);
      if (ret_val != pdPASS)
      {
        Serial.printf("mp3_player_task_start failed: %d\n", ret_val);
      }
    }
  }
}

void loop()
{
  if (!isStopped)
  {
    if (curr_frame < frames)
    {
      if (audbuf_remain == 0)
      {
        audbuf_read = AVI_read_audio(a, audbuf, MP3_MAX_FRAME_SIZE);
        audbuf_remain = audbuf_read;
        total_read_audio_ms += millis() - curr_ms;
        curr_ms = millis();
      }

      if (millis() < (next_frame_ms + 33)) // check show frame or skip frame
      {
        AVI_set_video_position(a, curr_frame);

        int iskeyframe;
        long video_bytes = AVI_frame_size(a, curr_frame);
        if (video_bytes > estimateBufferSize)
        {
          Serial.printf("video_bytes(%ld) > estimateBufferSize(%ld)\n", video_bytes, estimateBufferSize);
        }
        else
        {
          actual_video_size = AVI_read_frame(a, vidbuf, &iskeyframe);
          total_read_video_ms += millis() - curr_ms;
          curr_ms = millis();
          // Serial.printf("frame: %ld, iskeyframe: %ld, video_bytes: %ld, actual_video_size: %ld, audio_bytes: %ld, ESP.getFreeHeap(): %ld\n", curr_frame, iskeyframe, video_bytes, actual_video_size, audio_bytes, (long)ESP.getFreeHeap());

          jpegdec.openRAM((uint8_t *)vidbuf, actual_video_size, drawMCU);
          jpegdec.setPixelType(RGB565_BIG_ENDIAN);
          jpegdec.decode(0, 0, 0);
          jpegdec.close();
          total_decode_video_ms += millis() - curr_ms;
          curr_ms = millis();
        }
        while (millis() < (next_frame_ms - 33))
        {
          vTaskDelay(pdMS_TO_TICKS(1));
        }
      }
      else
      {
        ++skipped_frames;
        // Serial.printf("Skip frame %ld > %ld\n", millis(), next_frame_ms);
      }

      ++curr_frame;
      curr_ms = millis();
      next_frame_ms = start_ms + ((curr_frame + 1) * 1000 / fr);
    }
    else
    {
      int time_used = millis() - start_ms;
      int total_frames = curr_frame;
      audbuf_read = 0;
      AVI_close(a);
      isStopped = true;
      Serial.println("Play AVI end");

      long played_frames = total_frames - skipped_frames;
      float fps = 1000.0 * played_frames / time_used;
      total_decode_audio_ms -= total_play_audio_ms;
      total_decode_video_ms -= total_show_video_ms;
      Serial.printf("Played frames: %ld\n", played_frames);
      Serial.printf("Skipped frames: %ld (%0.1f %%)\n", skipped_frames, 100.0 * skipped_frames / total_frames);
      Serial.printf("Time used: %lu ms\n", time_used);
      Serial.printf("Expected FPS: %0.1f\n", fr);
      Serial.printf("Actual FPS: %0.1f\n", fps);
      Serial.printf("Read audio: %lu ms (%0.1f %%)\n", total_read_audio_ms, 100.0 * total_read_audio_ms / time_used);
      Serial.printf("Decode audio: %lu ms (%0.1f %%)\n", total_decode_audio_ms, 100.0 * total_decode_audio_ms / time_used);
      Serial.printf("Play audio: %lu ms (%0.1f %%)\n", total_play_audio_ms, 100.0 * total_play_audio_ms / time_used);
      Serial.printf("Read video: %lu ms (%0.1f %%)\n", total_read_video_ms, 100.0 * total_read_video_ms / time_used);
      Serial.printf("Decode video: %lu ms (%0.1f %%)\n", total_decode_video_ms, 100.0 * total_decode_video_ms / time_used);
      Serial.printf("Show video: %lu ms (%0.1f %%)\n", total_show_video_ms, 100.0 * total_show_video_ms / time_used);

#define CHART_MARGIN 32
#define LEGEND_A_COLOR 0x1BB6
#define LEGEND_B_COLOR 0xFBE1
#define LEGEND_C_COLOR 0x2D05
#define LEGEND_D_COLOR 0xD125
#define LEGEND_E_COLOR 0x9337
#define LEGEND_F_COLOR 0x8AA9
#define LEGEND_G_COLOR 0xE3B8
#define LEGEND_H_COLOR 0x7BEF
#define LEGEND_I_COLOR 0xBDE4
#define LEGEND_J_COLOR 0x15F9
      // gfx->setCursor(0, 0);
      gfx->setTextColor(WHITE);
      gfx->printf("Played frames: %d\n", played_frames);
      gfx->printf("Skipped frames: %ld (%0.1f %%)\n", skipped_frames, 100.0 * skipped_frames / total_frames);
      gfx->printf("Time used: %d ms\n", time_used);
      gfx->printf("Expected FPS: %0.1f\n", fr);
      gfx->printf("Actual FPS: %0.1f\n\n", fps);

      int16_t r1 = ((gfx->height() - CHART_MARGIN - CHART_MARGIN) / 2);
      int16_t r2 = r1 / 2;
      int16_t cx = gfx->width() - r1 - CHART_MARGIN;
      int16_t cy = r1 + CHART_MARGIN;

      float arc_start1 = 0;
      float arc_end1 = arc_start1 + max(2.0, 360.0 * total_read_audio_ms / time_used);
      for (int i = arc_start1 + 1; i < arc_end1; i += 2)
      {
        gfx->fillArc(cx, cy, r1, r2, arc_start1 - 90.0, i - 90.0, LEGEND_A_COLOR);
      }
      gfx->fillArc(cx, cy, r1, r2, arc_start1 - 90.0, arc_end1 - 90.0, LEGEND_A_COLOR);
      gfx->setTextColor(LEGEND_A_COLOR);
      gfx->printf("Read audio: %lu ms (%0.1f %%)\n", total_read_audio_ms, 100.0 * total_read_audio_ms / time_used);

      float arc_start2 = arc_end1;
      float arc_end2 = arc_start2 + max(2.0, 360.0 * total_read_video_ms / time_used);
      for (int i = arc_start2 + 1; i < arc_end2; i += 2)
      {
        gfx->fillArc(cx, cy, r1, r2, arc_start2 - 90.0, i - 90.0, LEGEND_B_COLOR);
      }
      gfx->fillArc(cx, cy, r1, r2, arc_start2 - 90.0, arc_end2 - 90.0, LEGEND_B_COLOR);
      gfx->setTextColor(LEGEND_B_COLOR);
      gfx->printf("Read video: %lu ms (%0.1f %%)\n", total_read_video_ms, 100.0 * total_read_video_ms / time_used);

      float arc_start3 = arc_end2;
      float arc_end3 = arc_start3 + max(2.0, 360.0 * total_decode_video_ms / time_used);
      for (int i = arc_start3 + 1; i < arc_end3; i += 2)
      {
        gfx->fillArc(cx, cy, r1, r2, arc_start3 - 90.0, i - 90.0, LEGEND_C_COLOR);
      }
      gfx->fillArc(cx, cy, r1, r2, arc_start3 - 90.0, arc_end3 - 90.0, LEGEND_C_COLOR);
      gfx->setTextColor(LEGEND_C_COLOR);
      gfx->printf("Decode video: %lu ms (%0.1f %%)\n", total_decode_video_ms, 100.0 * total_decode_video_ms / time_used);

      float arc_start4 = arc_end3;
      float arc_end4 = arc_start4 + max(2.0, 360.0 * total_show_video_ms / time_used);
      for (int i = arc_start4 + 1; i < arc_end4; i += 2)
      {
        gfx->fillArc(cx, cy, r1, r2, arc_start4 - 90.0, i - 90.0, LEGEND_D_COLOR);
      }
      gfx->fillArc(cx, cy, r1, r2, arc_start4 - 90.0, arc_end4 - 90.0, LEGEND_D_COLOR);
      gfx->setTextColor(LEGEND_D_COLOR);
      gfx->printf("Show video: %lu ms (%0.1f %%)\n", total_show_video_ms, 100.0 * total_show_video_ms / time_used);

      float arc_start5 = 0;
      float arc_end5 = arc_start5 + max(2.0, 360.0 * total_decode_audio_ms / time_used);
      for (int i = arc_start5 + 1; i < arc_end5; i += 2)
      {
        gfx->fillArc(cx, cy, r2, 0, arc_start5 - 90.0, i - 90.0, LEGEND_E_COLOR);
      }
      gfx->fillArc(cx, cy, r2, 0, arc_start5 - 90.0, arc_end5 - 90.0, LEGEND_E_COLOR);
      gfx->setTextColor(LEGEND_E_COLOR);
      gfx->printf("Decode audio: %lu ms (%0.1f %%)\n", total_decode_audio_ms, 100.0 * total_decode_audio_ms / time_used);
      gfx->setTextColor(LEGEND_G_COLOR);
      gfx->printf("Play audio: %lu ms (%0.1f %%)\n", total_play_audio_ms, 100.0 * total_play_audio_ms / time_used);
    }
  }
  else
  {
    delay(100);
  }
}
