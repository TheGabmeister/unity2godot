#pragma once

struct GLFWwindow;

namespace u2g {

class AppWindow {
public:
    bool init();
    bool shouldClose();
    void beginFrame();
    void endFrame();
    void shutdown();
    GLFWwindow* handle() const { return window_; }

private:
    GLFWwindow* window_ = nullptr;
};

} // namespace u2g
