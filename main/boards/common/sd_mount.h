#pragma once

#include <string>
#include <esp_err.h>
#include <driver/gpio.h>

class SdMount {
public:
    // Singleton
    static SdMount& GetInstance();

    // Khởi tạo:
    //  - Nếu có NVS ("wifi": sd_clk/sd_cmd/sd_d0/sd_d3) → dùng chân trong NVS
    //  - Nếu không có NVS        → dùng default MUMA S3 (17,18,21,13)
    //  - Thử mount SD một lần
    esp_err_t Init();

    // Re-init sau khi đã ghi chân SDMMC mới vào NVS
    //  - Deinit() → load pin từ NVS → thử mount lại
    esp_err_t ReinitFromNvs();

    // Gọi định kỳ nếu muốn hỗ trợ cắm/rút thẻ trong runtime
    void Loop();

    // Tháo SD + tắt SDMMC host
    void Deinit();

    // Trạng thái
    bool IsMounted() const { return mounted_; }

    // Thông tin đơn giản
    std::string GetMountPoint() const { return mount_point_; }
    std::string GetCardName()  const { return card_name_; }

    struct SdCardInfo {
        uint32_t capacityMB = 0;
        uint32_t speedKBps  = 0;   // reserved
    };

    SdCardInfo GetCardInfo() const { return info_; }

private:
    SdMount();
    ~SdMount();

    // Kiểm tra có thẻ hay không (dùng D3 nếu có)
    bool DetectInserted();

private:
    bool mounted_;
    bool last_detect_state_;

    std::string mount_point_;
    std::string card_name_;

    SdCardInfo info_;
};
