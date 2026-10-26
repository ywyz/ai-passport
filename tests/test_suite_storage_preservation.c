#include "../main/suite_storage.c"
#include <assert.h>
#include <stdio.h>
static esp_err_t result;
static unsigned erases;
esp_err_t nvs_flash_init(void) { return result; }
esp_err_t nvs_flash_erase(void) { erases++; return ESP_OK; }
void test_log(const char *tag,const char *format,...) { (void)tag; (void)format; }
int main(void)
{
    const esp_err_t outcomes[]={ESP_OK,ESP_ERR_NVS_NO_FREE_PAGES,ESP_ERR_NVS_NEW_VERSION_FOUND,ESP_FAIL};
    for (unsigned i=0;i<sizeof(outcomes)/sizeof(outcomes[0]);i++) {
        result=outcomes[i]; assert(suite_storage_init()==result); assert(!erases);
    }
    puts("NVS init failure preserves Wi-Fi/city storage: PASS"); return 0;
}
