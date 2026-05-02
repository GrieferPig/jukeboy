#include "qemu_openeth_service.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"

#include "runtime_env.h"

static const char *TAG = "qemu_openeth";

#define QEMU_OPENETH_LINK_UP_BIT BIT0
#define QEMU_OPENETH_GOT_IP_BIT BIT1
#define QEMU_OPENETH_STARTUP_LEASE_TIMEOUT_MS 15000

static esp_eth_handle_t s_eth_handle;
static esp_eth_netif_glue_handle_t s_eth_glue;
static esp_netif_t *s_eth_netif;
static EventGroupHandle_t s_events;
static esp_event_handler_instance_t s_eth_event_handler;
static esp_event_handler_instance_t s_ip_event_handler;
static esp_netif_ip_info_t s_ip_info;
static TickType_t s_ip_wait_deadline;
static bool s_initialized;
static bool s_started;
static bool s_have_ip;
static bool s_ip_wait_active;

static TickType_t qemu_openeth_service_wait_timeout_ticks(void)
{
    TickType_t wait_ticks = pdMS_TO_TICKS(QEMU_OPENETH_STARTUP_LEASE_TIMEOUT_MS);
    return wait_ticks == 0 ? 1 : wait_ticks;
}

static bool qemu_openeth_service_deadline_reached(TickType_t now, TickType_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void qemu_openeth_service_reset_state(void)
{
    s_eth_handle = NULL;
    s_eth_glue = NULL;
    s_eth_netif = NULL;
    s_eth_event_handler = NULL;
    s_ip_event_handler = NULL;
    s_started = false;
    s_have_ip = false;
    s_ip_wait_active = false;
    s_ip_wait_deadline = 0;
    memset(&s_ip_info, 0, sizeof(s_ip_info));
}

static void qemu_openeth_service_unregister_handlers(void)
{
    if (s_ip_event_handler != NULL)
    {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, s_ip_event_handler);
        s_ip_event_handler = NULL;
    }

    if (s_eth_event_handler != NULL)
    {
        esp_event_handler_instance_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, s_eth_event_handler);
        s_eth_event_handler = NULL;
    }
}

static void qemu_openeth_service_destroy_driver(void)
{
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;

    if (s_eth_glue != NULL)
    {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }

    if (s_eth_handle == NULL)
    {
        return;
    }

    if (s_started)
    {
        esp_err_t err = esp_eth_stop(s_eth_handle);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(TAG, "failed to stop OpenETH driver: %s", esp_err_to_name(err));
        }
        s_started = false;
    }

    esp_eth_get_mac_instance(s_eth_handle, &mac);
    esp_eth_get_phy_instance(s_eth_handle, &phy);

    esp_err_t err = esp_eth_driver_uninstall(s_eth_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "failed to uninstall OpenETH driver: %s", esp_err_to_name(err));
    }
    s_eth_handle = NULL;

    if (phy != NULL)
    {
        phy->del(phy);
    }
    if (mac != NULL)
    {
        mac->del(mac);
    }
}

static void qemu_openeth_service_cleanup(void)
{
    qemu_openeth_service_unregister_handlers();
    qemu_openeth_service_destroy_driver();

    if (s_eth_netif != NULL)
    {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }

    if (s_events != NULL)
    {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }

    s_initialized = false;
    qemu_openeth_service_reset_state();
}

static void qemu_openeth_eth_event_handler(void *arg,
                                           esp_event_base_t event_base,
                                           int32_t event_id,
                                           void *event_data)
{
    if (event_base != ETH_EVENT || event_data == NULL)
    {
        return;
    }

    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
    if (eth_handle != s_eth_handle)
    {
        return;
    }

    switch (event_id)
    {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "QEMU OpenETH started");
        break;
    case ETHERNET_EVENT_CONNECTED:
    {
        uint8_t mac_addr[ETH_ADDR_LEN] = {0};
        if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr) == ESP_OK)
        {
            ESP_LOGI(TAG,
                     "QEMU OpenETH link up, MAC %02x:%02x:%02x:%02x:%02x:%02x",
                     mac_addr[0],
                     mac_addr[1],
                     mac_addr[2],
                     mac_addr[3],
                     mac_addr[4],
                     mac_addr[5]);
        }
        else
        {
            ESP_LOGI(TAG, "QEMU OpenETH link up");
        }
        if (s_events != NULL)
        {
            xEventGroupSetBits(s_events, QEMU_OPENETH_LINK_UP_BIT);
        }
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "QEMU OpenETH link down");
        s_have_ip = false;
        memset(&s_ip_info, 0, sizeof(s_ip_info));
        if (s_events != NULL)
        {
            xEventGroupClearBits(s_events, QEMU_OPENETH_LINK_UP_BIT | QEMU_OPENETH_GOT_IP_BIT);
        }
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "QEMU OpenETH stopped");
        s_have_ip = false;
        memset(&s_ip_info, 0, sizeof(s_ip_info));
        if (s_events != NULL)
        {
            xEventGroupClearBits(s_events, QEMU_OPENETH_LINK_UP_BIT | QEMU_OPENETH_GOT_IP_BIT);
        }
        break;
    default:
        break;
    }
}

static void qemu_openeth_ip_event_handler(void *arg,
                                          esp_event_base_t event_base,
                                          int32_t event_id,
                                          void *event_data)
{
    if (event_base != IP_EVENT || event_id != IP_EVENT_ETH_GOT_IP || event_data == NULL)
    {
        return;
    }

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    if (event->esp_netif != s_eth_netif)
    {
        return;
    }

    s_ip_info = event->ip_info;
    s_have_ip = true;
    s_ip_wait_active = false;
    ESP_LOGI(TAG,
             "QEMU OpenETH got IPv4 address: ip=" IPSTR " gw=" IPSTR " mask=" IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.gw),
             IP2STR(&event->ip_info.netmask));
    if (s_events != NULL)
    {
        xEventGroupSetBits(s_events, QEMU_OPENETH_GOT_IP_BIT);
    }
}

esp_err_t qemu_openeth_service_init(void)
{
#if !CONFIG_ETH_USE_OPENETH
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_initialized)
    {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(app_is_running_in_qemu(), ESP_ERR_INVALID_STATE, TAG, "OpenETH is QEMU-only");

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    qemu_openeth_service_reset_state();
    s_events = xEventGroupCreate();
    if (s_events == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (s_eth_netif == NULL)
    {
        qemu_openeth_service_cleanup();
        return ESP_ERR_NO_MEM;
    }

    err = esp_event_handler_instance_register(ETH_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              qemu_openeth_eth_event_handler,
                                              NULL,
                                              &s_eth_event_handler);
    if (err != ESP_OK)
    {
        qemu_openeth_service_cleanup();
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_ETH_GOT_IP,
                                              qemu_openeth_ip_event_handler,
                                              NULL,
                                              &s_ip_event_handler);
    if (err != ESP_OK)
    {
        qemu_openeth_service_cleanup();
        return err;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ESP_ETH_PHY_ADDR_AUTO;
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);
    if (mac == NULL)
    {
        qemu_openeth_service_cleanup();
        return ESP_ERR_NO_MEM;
    }

    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (phy == NULL)
    {
        mac->del(mac);
        qemu_openeth_service_cleanup();
        return ESP_ERR_NO_MEM;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    eth_config.check_link_period_ms = 500;

    err = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (err != ESP_OK)
    {
        phy->del(phy);
        mac->del(mac);
        qemu_openeth_service_cleanup();
        return err;
    }

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (s_eth_glue == NULL)
    {
        qemu_openeth_service_cleanup();
        return ESP_ERR_NO_MEM;
    }

    err = esp_netif_attach(s_eth_netif, s_eth_glue);
    if (err != ESP_OK)
    {
        qemu_openeth_service_cleanup();
        return err;
    }

    err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK)
    {
        qemu_openeth_service_cleanup();
        return err;
    }

    s_started = true;
    s_initialized = true;
    s_ip_wait_active = true;
    s_ip_wait_deadline = xTaskGetTickCount() + qemu_openeth_service_wait_timeout_ticks();
    ESP_LOGI(TAG, "QEMU OpenETH driver initialized");
    return ESP_OK;
#endif
}

void qemu_openeth_service_process_once(void)
{
    if (!s_initialized || !s_started || !s_ip_wait_active)
    {
        return;
    }

    if (s_have_ip)
    {
        s_ip_wait_active = false;
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (!qemu_openeth_service_deadline_reached(now, s_ip_wait_deadline))
    {
        return;
    }

    ESP_LOGW(TAG, "QEMU OpenETH did not obtain an IPv4 lease within the startup window");
    s_ip_wait_active = false;
}

bool qemu_openeth_service_has_ip(void)
{
    return s_have_ip;
}