#include "gui/app_window.h"
#include "gui/converter_ui.h"
#include <nfd.h>

int main() {
    NFD_Init();

    u2g::AppWindow window;
    if (!window.init()) {
        NFD_Quit();
        return 1;
    }

    u2g::ConverterUI ui;

    while (!window.shouldClose()) {
        window.beginFrame();
        ui.render();
        window.endFrame();
    }

    window.shutdown();
    NFD_Quit();
    return 0;
}
