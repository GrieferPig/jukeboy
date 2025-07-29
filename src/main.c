#include "ota_app.h"
#include "factory_app.h"
#include "macros.h"

void app_main(void)
{
#if BUILD_FACTORY_APP
    // This is a factory build
    factory_app_main();
#else
    // This is a normal OTA build
    ota_app_main();

    // Test fallback by crashing the app
    // hardfault();
#endif
}
