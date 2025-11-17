cmake_minimum_required(VERSION 3.16)

# Add extra component directories
# Note: Order matters - dependencies should come first
set(EXTRA_COMPONENT_DIRS 
    "$ENV{IDF_PATH}/examples/common_components/protocol_examples_common"
    "${CMAKE_CURRENT_LIST_DIR}/support_folder/esp-video-components/esp_sccb_intf"
    "${CMAKE_CURRENT_LIST_DIR}/support_folder/esp-video-components/esp_ipa"
    "${CMAKE_CURRENT_LIST_DIR}/support_folder/esp-video-components/esp_cam_sensor"
    "${CMAKE_CURRENT_LIST_DIR}/support_folder/esp-video-components/esp_video"
    "${CMAKE_CURRENT_LIST_DIR}/support_folder/ESP32-P4-Module-DEV-KIT_Demo/ESP-IDF/4_ExpertTechniques/esp_brookesia_phone/components/human_face_detect"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ov5647)
