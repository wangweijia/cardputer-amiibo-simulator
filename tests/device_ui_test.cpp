#define main amiibozero_device_main
#include "device_main.cpp"
#undef main

#include <fstream>
#include <iostream>

bool visible_text(const char* expected)
{
    auto* root = lv_screen_active();
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        auto* child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) &&
            std::strstr(lv_label_get_text(child), expected)) { return true; }
    }
    return false;
}

void require_ui(bool ok) { if (!ok) { std::cerr << "Device UI transition failed\n"; std::exit(1); } }

struct TestBackend : amiibo::NfcBackend {
    const char* failure = "Test initialization error";
    int requests = 0;
    const char* initialize() override { return failure; }
    const char* start(const amiibo::Image&) override { return nullptr; }
    const char* status() override { return "UID #1 | Auto UID ON\nReady for NFC reader"; }
    void request_next_identity() override { ++requests; }
    void stop_session() override {}
    void stop() override {}
};

int main()
{
    auto temporary = (std::filesystem::temp_directory_path() / "amiibozero-ui-XXXXXX").string();
    require_ui(mkdtemp(temporary.data()));
    struct Fixture {
        std::string path;
        ~Fixture() { std::error_code error; std::filesystem::remove_all(path, error); }
    } fixture{temporary};
    const amiibo::Image data{4,0x11,0x22,0xbf,0x33,0x44,0x55,0x66,0x44};
    std::ofstream out(std::filesystem::path(temporary) / "role.bin", std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    out.close();
    lv_init();
    auto* display = lv_display_create(320, 170);
    static std::uint16_t pixels[320 * 170];
    lv_display_set_buffers(display, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    TestBackend backend;
    {
        HomeScreen screen(temporary, backend, nullptr);
        screen.key('\n');
        require_ui(visible_text(backend.failure) && !visible_text("Role BINs"));
        backend.failure = nullptr;
        screen.key('\n');
        require_ui(visible_text("Role BINs"));
        screen.key('\n');
        DeviceInput input;
        screen.update(input);
        require_ui(visible_text("Auto UID ON") && !visible_text("Role BINs"));
        screen.key('r');
        require_ui(backend.requests == 1);
        screen.key('z');
        require_ui(visible_text("Role BINs"));
        screen.key('n');
        require_ui(visible_text("NFC is off") && !visible_text("Role BINs"));
    }
    lv_display_delete(display);
    lv_deinit();
}
