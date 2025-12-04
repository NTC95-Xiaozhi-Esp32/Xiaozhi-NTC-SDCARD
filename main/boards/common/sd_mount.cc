#include "sd_mount.h"

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>
#include <driver/sdmmc_defs.h>
#include <sdmmc_cmd.h>
#include <driver/gpio.h>
#include <cstring>
#include <nvs.h>
#include <nvs_flash.h>

#define TAG "SDMOUNT"

// ========================================================
//  SDMMC Pin Map (1-bit ONLY)
//  - Mặc định: MUMA S3 (CLK=17, CMD=18, D0=21, D3=13)
//  - Thiết bị khác: ghi đè qua NVS (namespace "wifi")
//      sd_clk, sd_cmd, sd_d0, sd_d3
// ========================================================
struct SdPinMap {
    gpio_num_t clk;
    gpio_num_t cmd;
    gpio_num_t d0;
    gpio_num_t d3;   // detect hoặc NC
};

static SdPinMap sd_pins = {
    GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC
};

// Lưu card để unmount cho đúng
static sdmmc_card_t* s_card = nullptr;


// ========================================================
//  Load pinmap từ NVS nếu có
//  Namespace: "wifi"
//   - sd_clk
//   - sd_cmd
//   - sd_d0
//   - sd_d3
// ========================================================
static bool LoadPinmapFromNvs()
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK)
        return false;

    int32_t clk, cmd, d0, d3;
    bool ok = true;

    ok &= (nvs_get_i32(nvs, "sd_clk", &clk) == ESP_OK);
    ok &= (nvs_get_i32(nvs, "sd_cmd", &cmd) == ESP_OK);
    ok &= (nvs_get_i32(nvs, "sd_d0",  &d0)  == ESP_OK);
    ok &= (nvs_get_i32(nvs, "sd_d3",  &d3)  == ESP_OK);

    nvs_close(nvs);

    if (!ok) return false;

    sd_pins.clk = (gpio_num_t)clk;
    sd_pins.cmd = (gpio_num_t)cmd;
    sd_pins.d0  = (gpio_num_t)d0;
    sd_pins.d3  = (gpio_num_t)d3;

    ESP_LOGW(TAG, "⚡ Using SDMMC pins from NVS (User Config)");
    ESP_LOGW(TAG, "CLK=%d CMD=%d D0=%d D3=%d", clk, cmd, d0, d3);

    return true;
}

// ========================================================
//  SdMount class
// ========================================================
SdMount::SdMount()
: mounted_(false),
  last_detect_state_(true),
  mount_point_("/sdcard"),
  card_name_("")
{}

SdMount::~SdMount() {
    Deinit();
}

SdMount& SdMount::GetInstance() {
    static SdMount instance;
    return instance;
}

esp_err_t SdMount::Init()
{
    // ===== 1) Ưu tiên dùng chân từ NVS =====
    if (LoadPinmapFromNvs())
    {
        ESP_LOGW(TAG, "SDMMC pinmap loaded from NVS → bypass default MUMA.");
    }
    else
    {
        // ===== 2) Không có NVS → dùng default MUMA S3 =====
        sd_pins = { GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_21, GPIO_NUM_13 };
        ESP_LOGI(TAG, "Using default MUMA S3 SDMMC pinmap (CLK=17,CMD=18,D0=21,D3=13)");
    }

    // ===== 3) Thiết lập GPIO detect chân D3 nếu có =====
    if (sd_pins.d3 != GPIO_NUM_NC) {
        gpio_set_direction(sd_pins.d3, GPIO_MODE_INPUT);
        gpio_pullup_en(sd_pins.d3);
    }

    ESP_LOGI(TAG, "💾 SD Init → try mount SDMMC (1-bit)");
    Loop();    // thử auto-mount nếu có thẻ
    return ESP_OK;
}

// ========================================================
//  ReinitFromNvs()
//  - Dùng sau khi cấu hình chân SDMMC qua WiFi (ghi vào NVS)
//  - Tự Deinit() + load lại pin + re-config detect + auto-mount
// ========================================================
esp_err_t SdMount::ReinitFromNvs()
{
    ESP_LOGI(TAG, "🔁 Re-init SD from NVS...");

    // 1) Giải phóng SD hiện tại + tắt host
    Deinit();

    // 2) Load lại pin từ NVS
    if (!LoadPinmapFromNvs()) {
        ESP_LOGW(TAG, "⚠ No SD pinmap in NVS → skip auto-mount.");
        // Thiết bị vẫn chạy bình thường, chỉ là chưa mount được SD
        return ESP_FAIL;
    }

    // 3) Re-config chân detect
    if (sd_pins.d3 != GPIO_NUM_NC) {
        gpio_set_direction(sd_pins.d3, GPIO_MODE_INPUT);
        gpio_pullup_en(sd_pins.d3);
    }

    // 4) Thử mount lại tự động
    Loop();

    return mounted_ ? ESP_OK : ESP_FAIL;
}

// ========================================================
//  DetectInserted()
//  - MUMA: thực tế log cho thấy level=1 khi CÓ thẻ
//  - Nếu d3 = NC → bỏ qua auto-mount (thiết bị vẫn boot bình thường)
// ========================================================
bool SdMount::DetectInserted() {
    if (sd_pins.d3 == GPIO_NUM_NC) {
        // Không có chân detect → không auto-mount,
        // thiết bị vẫn chạy bình thường không thẻ.
        ESP_LOGW(TAG, "⚠ SD detect pin (D3) = NC → skip auto-mount.");
        return false;
    }

    int level = gpio_get_level(sd_pins.d3);

    // Với board của bạn: level = 1 khi CÓ thẻ
    bool inserted = (level == 1);

    if (inserted != last_detect_state_) {
        ESP_LOGI(TAG, "SD detect change: level=%d → %s",
                 level, inserted ? "INSERTED" : "REMOVED");
        last_detect_state_ = inserted;
    }

    return inserted;
}

// ========================================================
//  Loop() — auto mount an toàn
// ========================================================
void SdMount::Loop()
{
    if (mounted_ || !DetectInserted())
        return;

    ESP_LOGI(TAG, "🔌 Mount SD (SDMMC 1-bit)...");

    // ==== Kiểm tra pinmap hợp lệ ====
    if (sd_pins.clk == GPIO_NUM_NC ||
        sd_pins.cmd == GPIO_NUM_NC ||
        sd_pins.d0  == GPIO_NUM_NC)
    {
        ESP_LOGW(TAG,
            "⚠️ SDMMC pinmap invalid → Bỏ qua SD. "
            "Hãy nhập chân qua WiFi rồi gọi SdMount::ReinitFromNvs() hoặc Init() lại.");
        return;
    }

    // ==== HOST config ====
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    // ==== SLOT config ====
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = sd_pins.clk;
    slot.cmd = sd_pins.cmd;
    slot.d0  = sd_pins.d0;
    slot.d3  = sd_pins.d3;

    // ==== MOUNT config ====
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t* card = nullptr;
    esp_err_t ret;

    // ==== INIT HOST ====
    ret = sdmmc_host_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ sdmmc_host_init fail: %s", esp_err_to_name(ret));
        return;
    }

    // ==== INIT SLOT ====
    ret = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ sdmmc_host_init_slot fail: %s",
                 esp_err_to_name(ret));
        sdmmc_host_deinit();          // Ngăn ISR crash / leak
        return;
    }

    // ==== MOUNT ====
    ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, &card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ mount fail: %s", esp_err_to_name(ret));

        // Nếu không deinit, GDMA ISR vẫn chạy → InstrFetchProhibited / WDT
        sdmmc_host_deinit();

        mounted_ = false;
        s_card   = nullptr;
        return;
    }

    // ==== SUCCESS ====
    mounted_ = true;
    s_card   = card;

    // ===== Dump CID =====
    card_name_ = std::string(card->cid.name, 5);
    info_.capacityMB = card->csd.capacity / (1024 * 1024);

    const sdmmc_cid_t& c = card->cid;

    ESP_LOGI(TAG, "===== SD CID =====");
    ESP_LOGI(TAG, "MID: 0x%02X", c.mfg_id);

    char oem0 = (c.oem_id >> 8) & 0xFF;
    char oem1 = (c.oem_id >> 0) & 0xFF;
    ESP_LOGI(TAG, "OEM: %c%c", oem0, oem1);

    char pnm[6]; memcpy(pnm, c.name, 5); pnm[5] = 0;
    ESP_LOGI(TAG, "Product: %s", pnm);

    ESP_LOGI(TAG, "Revision: %d.%d",
             (c.revision >> 4) & 0x0F,
             c.revision & 0x0F);

    ESP_LOGI(TAG, "Serial: 0x%08X", c.serial);

    int month = c.date & 0x0F;
    int year  = 2000 + ((c.date >> 4) & 0xFF);

    ESP_LOGI(TAG, "Date: %02d/%04d", month, year);

    sdmmc_card_print_info(stdout, card);

    ESP_LOGI(TAG, "✅ SD mounted OK! (%s)", card_name_.c_str());
}

// ========================================================
//  Deinit() — unmount + tắt host
// ========================================================
void SdMount::Deinit()
{
    if (mounted_) {
        esp_vfs_fat_sdcard_unmount(mount_point_.c_str(), s_card);
        mounted_ = false;
        s_card   = nullptr;
        ESP_LOGI(TAG, "💨 SD unmounted.");
    }

    // Đảm bảo host được tắt, tránh "SDMMC host already initialized"
    sdmmc_host_deinit();
}
