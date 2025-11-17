/*
 * Camera Web Server Implementation
 * Provides HTTP endpoints for camera streaming and image capture
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_encode.h"
#include "esp_video_ioctl.h"
#include "camera_init.h"
#include "camera_server.h"

static const char *TAG = "camera_server";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

#define STREAM_BUFFER_COUNT          3
#define CAMERA_LOCK_TIMEOUT_MS       10000

typedef struct {
    void *addr;
    size_t length;
} mapped_buffer_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixformat;
    SemaphoreHandle_t lock;
} camera_stream_state_t;

typedef struct {
    jpeg_encoder_handle_t handle;
    jpeg_enc_input_format_t src_format;
    jpeg_down_sampling_type_t sub_sample;
    uint32_t src_stride;
    uint8_t *out_buf;
    size_t out_buf_size;
    uint8_t quality;
    bool initialized;
} jpeg_encoder_state_t;

// Simple HTML page for viewing the stream
static const char *INDEX_HTML = 
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "    <title>ESP32-P4 Camera</title>\n"
    "    <style>\n"
    "        body { font-family: Arial; text-align: center; margin: 20px; }\n"
    "        img { max-width: 90%; height: auto; border: 2px solid #333; }\n"
    "        h1 { color: #333; }\n"
    "        .buttons { margin: 20px; }\n"
    "        button { padding: 10px 20px; margin: 5px; font-size: 16px; cursor: pointer; }\n"
    "    </style>\n"
    "</head>\n"
    "<body>\n"
    "    <h1>ESP32-P4 OV5647 Camera</h1>\n"
    "    <div class='buttons'>\n"
    "        <button onclick='location.reload()'>Refresh</button>\n"
    "        <button onclick='captureImage()'>Capture Image</button>\n"
    "    </div>\n"
    "    <img id='stream' src='/stream' />\n"
    "    <script>\n"
    "        function captureImage() {\n"
    "            window.open('/capture', '_blank');\n"
    "        }\n"
    "    </script>\n"
    "</body>\n"
    "</html>";

static httpd_handle_t s_server = NULL;
static int s_camera_fd = -1;
static camera_stream_state_t s_stream_state = {
    .width = 0,
    .height = 0,
    .pixformat = V4L2_PIX_FMT_JPEG,
    .lock = NULL,
};
static jpeg_encoder_state_t s_jpeg_state = {0};

static esp_err_t camera_state_init(void)
{
    if (s_stream_state.lock) {
        return ESP_OK;
    }

    s_stream_state.lock = xSemaphoreCreateMutex();
    if (!s_stream_state.lock) {
        ESP_LOGE(TAG, "Failed to create camera lock");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t jpeg_encoder_init(void)
{
    if (s_jpeg_state.initialized) {
        return ESP_OK;
    }

    uint8_t src_bpp = 0;
    jpeg_enc_input_format_t src_format;
    jpeg_down_sampling_type_t sub_sample;
    esp_err_t err = ESP_OK;

    switch (s_stream_state.pixformat) {
    case V4L2_PIX_FMT_RGB565:
        src_format = JPEG_ENCODE_IN_FORMAT_RGB565;
        sub_sample = JPEG_DOWN_SAMPLING_YUV422;
        src_bpp = 16;
        break;
    case V4L2_PIX_FMT_RGB24:
        src_format = JPEG_ENCODE_IN_FORMAT_RGB888;
        sub_sample = JPEG_DOWN_SAMPLING_YUV444;
        src_bpp = 24;
        break;
    case V4L2_PIX_FMT_YUV422P:
        src_format = JPEG_ENCODE_IN_FORMAT_YUV422;
        sub_sample = JPEG_DOWN_SAMPLING_YUV422;
        src_bpp = 16;
        break;
    case V4L2_PIX_FMT_GREY:
        src_format = JPEG_ENCODE_IN_FORMAT_GRAY;
        sub_sample = JPEG_DOWN_SAMPLING_GRAY;
        src_bpp = 8;
        break;
    default:
        ESP_LOGE(TAG, "Unsupported pixel format for JPEG encoding: " V4L2_FMT_STR, V4L2_FMT_STR_ARG(s_stream_state.pixformat));
        return ESP_ERR_NOT_SUPPORTED;
    }

    jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 200,
    };

    err = jpeg_new_encoder_engine(&eng_cfg, &s_jpeg_state.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG encoder (%s)", esp_err_to_name(err));
        return err;
    }

    size_t src_stride = (size_t)s_stream_state.width * s_stream_state.height * src_bpp / 8;
    size_t out_size = src_stride;
    jpeg_encode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t actual_size = 0;
    uint8_t *buf = (uint8_t *)jpeg_alloc_encoder_mem(out_size, &mem_cfg, &actual_size);
    if (!buf) {
        jpeg_del_encoder_engine(s_jpeg_state.handle);
        s_jpeg_state.handle = NULL;
        ESP_LOGE(TAG, "Failed to allocate JPEG output buffer");
        return ESP_ERR_NO_MEM;
    }

    s_jpeg_state.src_format = src_format;
    s_jpeg_state.sub_sample = sub_sample;
    s_jpeg_state.src_stride = src_stride;
    s_jpeg_state.out_buf = buf;
    s_jpeg_state.out_buf_size = actual_size ? actual_size : out_size;
    s_jpeg_state.quality = 75;
    s_jpeg_state.initialized = true;

    ESP_LOGI(TAG, "JPEG encoder ready (%ux%u)", s_stream_state.width, s_stream_state.height);
    return ESP_OK;
}

static void jpeg_encoder_deinit(void)
{
    if (s_jpeg_state.out_buf) {
        free(s_jpeg_state.out_buf);
        s_jpeg_state.out_buf = NULL;
    }

    if (s_jpeg_state.handle) {
        jpeg_del_encoder_engine(s_jpeg_state.handle);
        s_jpeg_state.handle = NULL;
    }

    s_jpeg_state.initialized = false;
}

static esp_err_t jpeg_encode_frame(const uint8_t *src, size_t src_size, const uint8_t **jpeg_buf, size_t *jpeg_size)
{
    if (!s_jpeg_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    jpeg_encode_cfg_t cfg = {
        .src_type = s_jpeg_state.src_format,
        .sub_sample = s_jpeg_state.sub_sample,
        .image_quality = s_jpeg_state.quality,
        .width = s_stream_state.width,
        .height = s_stream_state.height,
    };

    uint32_t out_size = 0;
    esp_err_t err = jpeg_encoder_process(s_jpeg_state.handle,
                                         &cfg,
                                         (uint8_t *)src,
                                         src_size,
                                         s_jpeg_state.out_buf,
                                         s_jpeg_state.out_buf_size,
                                         &out_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encode failed (%s)", esp_err_to_name(err));
        return err;
    }

    *jpeg_buf = s_jpeg_state.out_buf;
    *jpeg_size = out_size;
    return ESP_OK;
}

static bool camera_lock_acquire(void)
{
    if (!s_stream_state.lock) {
        return false;
    }
    return xSemaphoreTake(s_stream_state.lock, pdMS_TO_TICKS(CAMERA_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void camera_lock_release(void)
{
    if (s_stream_state.lock) {
        xSemaphoreGive(s_stream_state.lock);
    }
}

static esp_err_t configure_camera_device(int camera_fd)
{
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(camera_fd, VIDIOC_G_FMT, &fmt) < 0) {
        ESP_LOGE(TAG, "Failed to get video format: errno=%d", errno);
        return ESP_FAIL;
    }

    uint32_t desired_width = CONFIG_FACE_DET_ISP_WIDTH;
    uint32_t desired_height = CONFIG_FACE_DET_ISP_HEIGHT;

    if (desired_width > 0 && desired_height > 0) {
        fmt.fmt.pix.width = desired_width;
        fmt.fmt.pix.height = desired_height;
        ESP_LOGI(TAG, "Requesting ISP output %ux%u", desired_width, desired_height);
    }

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        struct v4l2_format rgb_fmt = fmt;
        rgb_fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        rgb_fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (desired_width > 0 && desired_height > 0) {
            rgb_fmt.fmt.pix.width = desired_width;
            rgb_fmt.fmt.pix.height = desired_height;
        }

        if (ioctl(camera_fd, VIDIOC_S_FMT, &rgb_fmt) == 0) {
            fmt = rgb_fmt;
            ESP_LOGI(TAG, "Switched camera stream to RGB565");
        } else {
            struct v4l2_format yuv_fmt = fmt;
            yuv_fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV422P;
            yuv_fmt.fmt.pix.field = V4L2_FIELD_NONE;
            if (desired_width > 0 && desired_height > 0) {
                yuv_fmt.fmt.pix.width = desired_width;
                yuv_fmt.fmt.pix.height = desired_height;
            }
            if (ioctl(camera_fd, VIDIOC_S_FMT, &yuv_fmt) == 0) {
                fmt = yuv_fmt;
                ESP_LOGI(TAG, "Switched camera stream to YUV422");
            } else {
                ESP_LOGW(TAG, "Failed to switch to RGB565/YUV422 format, errno=%d", errno);
            }
        }
    }

    s_stream_state.width = fmt.fmt.pix.width;
    s_stream_state.height = fmt.fmt.pix.height;
    s_stream_state.pixformat = fmt.fmt.pix.pixelformat;

    ESP_LOGI(TAG, "Camera format: %ux%u " V4L2_FMT_STR,
             s_stream_state.width,
             s_stream_state.height,
             V4L2_FMT_STR_ARG(s_stream_state.pixformat));
    return ESP_OK;
}

// Handler for the root page
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// Handler for MJPEG stream
static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!camera_lock_acquire()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera busy");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    struct v4l2_buffer buf;
    char part_buf[96];
    mapped_buffer_t *buffers = NULL;
    bool stream_started = false;

    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    struct v4l2_requestbuffers req_buf = {
        .count = STREAM_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    if (ioctl(s_camera_fd, VIDIOC_REQBUFS, &req_buf) < 0 || req_buf.count == 0) {
        ESP_LOGE(TAG, "Failed to request video buffers");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to alloc buffers");
        ret = ESP_FAIL;
        goto cleanup;
    }

    buffers = calloc(req_buf.count, sizeof(mapped_buffer_t));
    if (!buffers) {
        ESP_LOGE(TAG, "Failed to allocate buffer descriptors");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    for (uint32_t i = 0; i < req_buf.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(s_camera_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to query buffer %u", i);
            ret = ESP_FAIL;
            goto cleanup;
        }

        buffers[i].addr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_camera_fd, buf.m.offset);
        buffers[i].length = buf.length;

        if (buffers[i].addr == MAP_FAILED) {
            ESP_LOGE(TAG, "Failed to mmap buffer %u", i);
            buffers[i].addr = NULL;
            ret = ESP_FAIL;
            goto cleanup;
        }

        if (ioctl(s_camera_fd, VIDIOC_QBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to queue buffer %u", i);
            ret = ESP_FAIL;
            goto cleanup;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_camera_fd, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "Failed to start video stream");
        ret = ESP_FAIL;
        goto cleanup;
    }
    stream_started = true;
    ESP_LOGI(TAG, "Stream started (%ux%u)", s_stream_state.width, s_stream_state.height);

    while (1) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(s_camera_fd, VIDIOC_DQBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to dequeue buffer");
            ret = ESP_FAIL;
            break;
        }

        if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
            ioctl(s_camera_fd, VIDIOC_QBUF, &buf);
            continue;
        }

        const uint8_t *jpeg_buf = NULL;
        size_t jpeg_size = 0;
        if (jpeg_encode_frame((const uint8_t *)buffers[buf.index].addr, buf.bytesused, &jpeg_buf, &jpeg_size) != ESP_OK) {
            ioctl(s_camera_fd, VIDIOC_QBUF, &buf);
            ret = ESP_FAIL;
            break;
        }

        if (httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) != ESP_OK) {
            ioctl(s_camera_fd, VIDIOC_QBUF, &buf);
            ret = ESP_FAIL;
            break;
        }

        int hdr_len = snprintf(part_buf, sizeof(part_buf), STREAM_PART, (unsigned int)jpeg_size);
        if (hdr_len <= 0 ||
            httpd_resp_send_chunk(req, part_buf, hdr_len) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)jpeg_buf, jpeg_size) != ESP_OK) {
            ioctl(s_camera_fd, VIDIOC_QBUF, &buf);
            ret = ESP_FAIL;
            break;
        }

        if (ioctl(s_camera_fd, VIDIOC_QBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to requeue buffer");
            ret = ESP_FAIL;
            break;
        }
    }

cleanup:
    if (stream_started) {
        enum v4l2_buf_type type_stop = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_camera_fd, VIDIOC_STREAMOFF, &type_stop);
    }

    if (buffers) {
        for (uint32_t i = 0; i < req_buf.count; i++) {
            if (buffers[i].addr) {
                munmap(buffers[i].addr, buffers[i].length);
            }
        }
        free(buffers);
    }

    camera_lock_release();
    ESP_LOGI(TAG, "Stream stopped");
    return ret;
}

// Handler for single image capture
static esp_err_t capture_handler(httpd_req_t *req)
{
    if (!camera_lock_acquire()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera busy");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    struct v4l2_buffer buf = {0};
    mapped_buffer_t buffer = {0};
    bool stream_started = false;

    struct v4l2_requestbuffers req_buf = {
        .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    if (ioctl(s_camera_fd, VIDIOC_REQBUFS, &req_buf) < 0 || req_buf.count == 0) {
        ESP_LOGE(TAG, "Failed to request capture buffer");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to alloc buffer");
        ret = ESP_FAIL;
        goto cleanup;
    }

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;

    if (ioctl(s_camera_fd, VIDIOC_QUERYBUF, &buf) < 0) {
        ESP_LOGE(TAG, "Failed to query capture buffer");
        ret = ESP_FAIL;
        goto cleanup;
    }

    buffer.addr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_camera_fd, buf.m.offset);
    buffer.length = buf.length;
    if (buffer.addr == MAP_FAILED) {
        ESP_LOGE(TAG, "Failed to mmap capture buffer");
        buffer.addr = NULL;
        ret = ESP_FAIL;
        goto cleanup;
    }

    if (ioctl(s_camera_fd, VIDIOC_QBUF, &buf) < 0) {
        ESP_LOGE(TAG, "Failed to queue capture buffer");
        ret = ESP_FAIL;
        goto cleanup;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_camera_fd, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "Failed to start capture stream");
        ret = ESP_FAIL;
        goto cleanup;
    }
    stream_started = true;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_camera_fd, VIDIOC_DQBUF, &buf) < 0) {
        ESP_LOGE(TAG, "Failed to dequeue capture buffer");
        ret = ESP_FAIL;
        goto cleanup;
    }

    const uint8_t *jpeg_buf = NULL;
    size_t jpeg_size = 0;
    if (jpeg_encode_frame((const uint8_t *)buffer.addr, buf.bytesused, &jpeg_buf, &jpeg_size) == ESP_OK) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
        ret = httpd_resp_send(req, (const char *)jpeg_buf, jpeg_size);
    } else {
        ret = ESP_FAIL;
    }

cleanup:
    if (stream_started) {
        enum v4l2_buf_type type_stop = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_camera_fd, VIDIOC_STREAMOFF, &type_stop);
    }

    if (buffer.addr) {
        munmap(buffer.addr, buffer.length);
    }

    camera_lock_release();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Capture completed");
    }
    return ret;
}

esp_err_t camera_server_start(int camera_fd)
{
    if (camera_fd < 0) {
        ESP_LOGE(TAG, "Invalid camera file descriptor");
        return ESP_ERR_INVALID_ARG;
    }

    s_camera_fd = camera_fd;
    esp_err_t err = camera_state_init();
    if (err != ESP_OK) {
        return err;
    }

    err = configure_camera_device(s_camera_fd);
    if (err != ESP_OK) {
        return err;
    }

    err = jpeg_encoder_init();
    if (err != ESP_OK) {
        return err;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 8;
    config.max_resp_headers = 8;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        jpeg_encoder_deinit();
        return ESP_FAIL;
    }

    // Register URI handlers
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &index_uri);

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &stream_uri);

    httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &capture_uri);

    ESP_LOGI(TAG, "Camera web server started successfully");
    return ESP_OK;
}

void camera_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }

    if (s_stream_state.lock) {
        vSemaphoreDelete(s_stream_state.lock);
        s_stream_state.lock = NULL;
    }

    jpeg_encoder_deinit();
}
