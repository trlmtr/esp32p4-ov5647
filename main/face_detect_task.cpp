#include "face_detect_task.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>

#include <list>
#include <string>
#include <vector>

#include "sdkconfig.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "human_face_detect.hpp"

namespace {

constexpr char TAG[] = "face_detect";

struct MappedBuffer {
    void *addr = nullptr;
    size_t length = 0;
};

struct FaceDetectContext {
    int camera_fd = -1;
    bool should_stop = false;
    bool running = false;
    bool stream_started = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pixformat = 0;
    TaskHandle_t task_handle = nullptr;
    std::vector<MappedBuffer> buffers;
    HumanFaceDetect *detector = nullptr;
    esp_mqtt_client_handle_t mqtt_client = nullptr;
};

FaceDetectContext s_ctx;

esp_err_t configure_camera_device(FaceDetectContext &ctx)
{
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(ctx.camera_fd, VIDIOC_G_FMT, &fmt) < 0) {
        ESP_LOGE(TAG, "Failed to get video format: errno=%d", errno);
        return ESP_FAIL;
    }

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(ctx.camera_fd, VIDIOC_S_FMT, &fmt) < 0) {
            ESP_LOGE(TAG, "Failed to set RGB565 format, errno=%d", errno);
            return ESP_FAIL;
        }
    }

    ctx.width = fmt.fmt.pix.width;
    ctx.height = fmt.fmt.pix.height;
    ctx.pixformat = fmt.fmt.pix.pixelformat;

    ESP_LOGI(TAG, "Detection stream: %ux%u " V4L2_FMT_STR,
             ctx.width, ctx.height, V4L2_FMT_STR_ARG(ctx.pixformat));
    return ESP_OK;
}

esp_err_t init_v4l2_buffers(FaceDetectContext &ctx)
{
    struct v4l2_requestbuffers req = {};
    req.count = CONFIG_FACE_DET_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(ctx.camera_fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        ESP_LOGE(TAG, "Failed to request buffers for detection");
        return ESP_FAIL;
    }

    ctx.buffers.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(ctx.camera_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to query buffer %u", i);
            return ESP_FAIL;
        }

        ctx.buffers[i].length = buf.length;
        ctx.buffers[i].addr =
            mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, ctx.camera_fd, buf.m.offset);
        if (ctx.buffers[i].addr == MAP_FAILED) {
            ctx.buffers[i].addr = nullptr;
            ESP_LOGE(TAG, "Failed to mmap buffer %u", i);
            return ESP_FAIL;
        }

        if (ioctl(ctx.camera_fd, VIDIOC_QBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to queue buffer %u", i);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

void release_buffers(FaceDetectContext &ctx)
{
    for (auto &buffer : ctx.buffers) {
        if (buffer.addr && buffer.addr != MAP_FAILED) {
            munmap(buffer.addr, buffer.length);
        }
        buffer.addr = nullptr;
        buffer.length = 0;
    }
    ctx.buffers.clear();
}

void publish_results(FaceDetectContext &ctx, const std::list<dl::detect::result_t> &results, int64_t timestamp_us)
{
    if (!ctx.mqtt_client) {
        return;
    }

    std::string payload;
    payload.reserve(128 + results.size() * 64);
    payload.append("{\"ts\":");
    payload.append(std::to_string(timestamp_us));
    payload.append(",\"width\":");
    payload.append(std::to_string(ctx.width));
    payload.append(",\"height\":");
    payload.append(std::to_string(ctx.height));
    payload.append(",\"faces\":[");

    bool first = true;
    for (const auto &res : results) {
        const auto &box = res.box;
        if (box.size() < 4) {
            continue;
        }

        if (!first) {
            payload.push_back(',');
        }
        first = false;

        const int x = box[0];
        const int y = box[1];
        const int w = box[2] - box[0];
        const int h = box[3] - box[1];

        payload.append("{\"x\":");
        payload.append(std::to_string(x));
        payload.append(",\"y\":");
        payload.append(std::to_string(y));
        payload.append(",\"w\":");
        payload.append(std::to_string(w));
        payload.append(",\"h\":");
        payload.append(std::to_string(h));
        payload.append(",\"score\":");
        payload.append(std::to_string(res.score));
        payload.push_back('}');
    }

    payload.append("]}");
    const int payload_len = static_cast<int>(payload.length());
    int msg_id = esp_mqtt_client_publish(ctx.mqtt_client,
                                         CONFIG_MQTT_TOPIC_FACE_EVENTS,
                                         payload.c_str(),
                                         payload_len,
                                         0,
                                         0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "MQTT publish failed");
    }
}

void detection_task(void *arg)
{
    auto *ctx = static_cast<FaceDetectContext *>(arg);
    do {
        if (!ctx->detector) {
            ESP_LOGE(TAG, "Detector not initialized");
            break;
        }

        if (configure_camera_device(*ctx) != ESP_OK) {
            break;
        }

        if (init_v4l2_buffers(*ctx) != ESP_OK) {
            break;
        }

        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(ctx->camera_fd, VIDIOC_STREAMON, &type) < 0) {
            ESP_LOGE(TAG, "Failed to start detection stream");
            break;
        }
        ctx->stream_started = true;

        const TickType_t interval =
            CONFIG_FACE_DET_MIN_INTERVAL_MS > 0 ? pdMS_TO_TICKS(CONFIG_FACE_DET_MIN_INTERVAL_MS) : 0;
        TickType_t last_wake = xTaskGetTickCount();
        int frame_count = 0;
        int face_count = 0;
        int64_t last_fps_log = esp_timer_get_time();

        while (!ctx->should_stop) {
            struct v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            if (ioctl(ctx->camera_fd, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (ctx->should_stop) {
                    break;
                }
                ESP_LOGE(TAG, "Failed to dequeue buffer: errno=%d", errno);
                break;
            }

            auto &mapped = ctx->buffers.at(buf.index);
            dl::image::img_t img;
            img.data = reinterpret_cast<uint16_t *>(mapped.addr);
            img.width = ctx->width;
            img.height = ctx->height;
            img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565;

            auto &det_results = ctx->detector->run(img);
            int64_t ts = static_cast<int64_t>(buf.timestamp.tv_sec) * 1000000LL + buf.timestamp.tv_usec;
            if (ts == 0) {
                ts = esp_timer_get_time();
            }
            if (det_results.size() > 0) face_count++;
            //publish_results(*ctx, det_results, ts);
            //ESP_LOGI(TAG, "Detections: %d faces", num_faces);
           
            if (ioctl(ctx->camera_fd, VIDIOC_QBUF, &buf) < 0) {
                ESP_LOGE(TAG, "Failed to requeue buffer: errno=%d", errno);
                break;
            }

            ++frame_count;
            int64_t now = esp_timer_get_time();
            if (now - last_fps_log >= 1000000) {
                ESP_LOGI(TAG, "Face detection FPS: %d, Faces detected: %d", frame_count, face_count);
                frame_count = 0;
                face_count = 0;
                last_fps_log = now;
            }

            if (interval > 0) {
                vTaskDelayUntil(&last_wake, interval);
            }
        }
    } while (0);

    if (ctx->stream_started) {
        enum v4l2_buf_type type_stop = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(ctx->camera_fd, VIDIOC_STREAMOFF, &type_stop);
        ctx->stream_started = false;
    }
    release_buffers(*ctx);
    if (ctx->detector) {
        delete ctx->detector;
        ctx->detector = nullptr;
    }
    ctx->task_handle = nullptr;
    ctx->running = false;
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t face_detect_start(int camera_fd, esp_mqtt_client_handle_t mqtt_client)
{
#if !CONFIG_APP_ENABLE_FACE_DETECTION
    (void)camera_fd;
    (void)mqtt_client;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (camera_fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.camera_fd = camera_fd;
    s_ctx.mqtt_client = mqtt_client;
    s_ctx.should_stop = false;
    s_ctx.detector = new HumanFaceDetect();
    if (!s_ctx.detector) {
        ESP_LOGE(TAG, "Failed to allocate detector");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        detection_task,
        "face_detect",
        CONFIG_FACE_DET_TASK_STACK_SIZE,
        &s_ctx,
        CONFIG_FACE_DET_TASK_PRIORITY,
        &s_ctx.task_handle,
        tskNO_AFFINITY);
    if (ret != pdPASS) {
        delete s_ctx.detector;
        s_ctx.detector = nullptr;
        ESP_LOGE(TAG, "Failed to create detection task");
        return ESP_FAIL;
    }

    s_ctx.running = true;
    ESP_LOGI(TAG, "Face detection task started");
    return ESP_OK;
#endif
}

void face_detect_stop(void)
{
#if CONFIG_APP_ENABLE_FACE_DETECTION
    if (!s_ctx.running) {
        return;
    }
    s_ctx.should_stop = true;
    if (s_ctx.task_handle) {
        // Wait for task to clean up
        while (s_ctx.running) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    s_ctx.mqtt_client = nullptr;
#endif
}
