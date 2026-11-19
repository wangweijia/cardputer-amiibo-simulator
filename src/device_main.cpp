#include "device_input.h"
#include "cap_listener.h"

#include <lvgl.h>
#include <src/drivers/display/fb/lv_linux_fbdev.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t interrupted = 0;

void stop(int)
{
    interrupted = 1;
}

constexpr std::array keys{KEY_Z, KEY_X, KEY_C, KEY_F, KEY_ESC, KEY_ENTER, KEY_N, KEY_R};
constexpr std::array controls{'z', 'x', 'c', 'f', '\x1b', '\n', 'n', 'r'};

int open_keypad()
{
    int fallback = -1;
    std::error_code error;
    std::filesystem::directory_iterator entry("/dev/input", error), end;
    for (; !error && entry != end; entry.increment(error)) {
        if (entry->path().filename().string().rfind("event", 0) != 0) {
            continue;
        }
        const int fd = open(entry->path().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        unsigned char bits[(KEY_MAX / 8) + 1]{};
        bool matches = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0;
        for (const int key : keys) {
            matches = matches && (bits[key / 8] & (1u << (key % 8)));
        }
        char name[128]{};
        if (matches && ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
            std::strcmp(name, "tca8418c") == 0) {
            if (fallback >= 0) {
                close(fallback);
            }
            return fd;
        }
        if (matches && fallback < 0) {
            fallback = fd;
        } else {
            close(fd);
        }
    }
    if (error) {
        std::fprintf(stderr, "AmiiboZero: cannot enumerate /dev/input: %s\n", error.message().c_str());
    }
    return fallback;
}

template<typename KeyHandler>
bool read_keys(int fd, DeviceInput& input, bool& resync, KeyHandler handle_key)
{
    input_event event{};
    for (;;) {
        const auto count = read(fd, &event, sizeof(event));
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return true;
        }
        if (count != sizeof(event)) {
            std::fprintf(stderr, "AmiiboZero: keypad read failed: %s\n",
                         count < 0 ? std::strerror(errno) : "incomplete event");
            return false;
        }
        if (event.type == EV_SYN && event.code == SYN_DROPPED) {
            input.cancel_hold();
            resync = true;
        } else if (resync) {
            if (event.type == EV_SYN && event.code == SYN_REPORT) {
                resync = false;
            }
        } else if (event.type == EV_KEY && (event.value == 0 || event.value == 1)) {
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (event.code == keys[i]) {
                    if (controls[i] == '\x1b') {
                        input.key('\x1b', event.value == 1, DeviceInput::Clock::now());
                    } else if (event.value == 1) {
                        handle_key(controls[i]);
                    }
                    break;
                }
            }
        }
    }
}

class HomeScreen {
public:
    HomeScreen(std::filesystem::path directory, amiibo::NfcBackend& backend, lv_font_t* role_font)
        : flow_(std::move(directory), backend)
    {
        auto* root = lv_screen_active();
        lv_obj_set_style_bg_color(root, lv_color_hex(0x101827), 0);
        lv_obj_set_style_text_color(root, lv_color_hex(0xF5F7FC), 0);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

        title_ = lv_label_create(root);
        lv_label_set_text(title_, "AmiiboZero");
        lv_obj_set_pos(title_, 14, 8);

        detail_ = lv_label_create(root);
        lv_obj_set_pos(detail_, 14, 52);
        lv_obj_set_width(detail_, 292);
        lv_label_set_long_mode(detail_, LV_LABEL_LONG_WRAP);
        footer_ = lv_label_create(root);
        lv_obj_align(footer_, LV_ALIGN_BOTTOM_MID, 0, -6);

        list_title_ = lv_label_create(root);
        lv_obj_set_pos(list_title_, 14, 8);
        lv_obj_add_flag(list_title_, LV_OBJ_FLAG_HIDDEN);
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            auto* row = lv_label_create(root);
            lv_obj_set_size(row, 292, 23);
            lv_obj_set_pos(row, 14, 28 + 24 * i);
            lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
            if (role_font) {
                lv_obj_set_style_text_font(row, role_font, 0);
            }
            lv_obj_set_style_pad_left(row, 5, 0);
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            rows_[i] = row;
        }
        list_status_ = lv_label_create(root);
        lv_obj_set_pos(list_status_, 14, 127);
        lv_obj_set_width(list_status_, 292);
        lv_label_set_long_mode(list_status_, LV_LABEL_LONG_DOT);
        lv_obj_add_flag(list_status_, LV_OBJ_FLAG_HIDDEN);
        draw(nullptr);
    }

    void key(char key)
    {
        if (!flow_.enabled()) {
            if (key == '\n') {
                lv_label_set_text(detail_, "Starting NFC...");
                lv_refr_now(nullptr);
                draw(flow_.enable());
            }
            return;
        }

        if (key == 'n') {
            flow_.disable();
            draw(nullptr);
        } else if (flow_.listening()) {
            if (key == 'z') {
                flow_.stop_session();
                draw(nullptr);
            } else if (key == 'r') {
                flow_.request_next_identity();
            }
        } else if (key == '\n') {
            draw(flow_.confirm());
        } else if (flow_.roles().move(key)) {
            draw(nullptr);
        }
    }

    void shutdown() { flow_.disable(); }

    void update(const DeviceInput& input)
    {
        if (const auto* error = flow_.poll_error()) {
            draw(error);
        }
        if (const auto* status = flow_.status()) {
            if (listening_status_ != status) {
                listening_status_ = status;
                lv_label_set_text(detail_, status);
            }
        }
        const char* text = input.holding() ? "Keep holding Esc to exit" :
            !flow_.enabled() ? "Enter: enable  |  Hold Esc 3s: exit" :
            flow_.listening() ? "R: next UID  Z: back  N: off" :
            "F/X: move  Z/C: page  N: NFC off";
        if (footer_text_ != text) {
            footer_text_ = text;
            lv_label_set_text_static(footer_, text);
        }
    }

private:
    static void set_visible(lv_obj_t* object, bool visible)
    {
        if (visible) {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void draw(const char* feedback)
    {
        listening_status_.clear();
        const bool listening = flow_.listening();
        const bool show = flow_.enabled() && !listening;
        if (listening && !spinner_) {
            spinner_ = lv_spinner_create(lv_screen_active());
            lv_obj_set_size(spinner_, 36, 36);
            lv_obj_align(spinner_, LV_ALIGN_TOP_MID, 0, 44);
            lv_spinner_set_anim_params(spinner_, 950, 220);
            lv_obj_set_style_arc_color(spinner_, lv_color_hex(0x243450), LV_PART_MAIN);
            lv_obj_set_style_arc_color(spinner_, lv_color_hex(0x40D5A5), LV_PART_INDICATOR);
            lv_obj_set_style_arc_width(spinner_, 4, LV_PART_MAIN);
            lv_obj_set_style_arc_width(spinner_, 4, LV_PART_INDICATOR);
        } else if (!listening && spinner_) {
            lv_obj_delete(spinner_);
            spinner_ = nullptr;
        }
        set_visible(title_, !show);
        if (!show) {
            for (auto* row : rows_) {
                set_visible(row, false);
            }
        }
        set_visible(detail_, !show);
        set_visible(list_title_, show);
        set_visible(list_status_, show);
        if (show) {
            draw_roles(feedback);
        } else {
            lv_label_set_text(title_, listening ? "NFC: listening" : "AmiiboZero");
            lv_obj_set_y(detail_, listening ? 94 : 52);
            lv_obj_set_style_text_align(detail_, listening ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
            lv_label_set_text(detail_, feedback ? feedback :
                !flow_.enabled() ? "NFC is off.\nEnable NFC to choose a role BIN." :
                                  "Waiting for NFC reader...");
        }
    }

    void draw_roles(const char* feedback)
    {
        const auto& roles = flow_.roles();
        const auto& catalog = roles.catalog();
        const auto count = catalog.entries.size();
        const auto title = count == 0 ? std::string("NFC armed | Role BINs") :
            "NFC armed | Role BINs  " + std::to_string(roles.focus() + 1) + "/" + std::to_string(count);
        lv_label_set_text(list_title_, title.c_str());
        for (std::size_t row = 0; row < rows_.size(); ++row) {
            const auto idx = roles.page_start() + row;
            auto* label = rows_[row];
            set_visible(label, idx < count);
            if (idx >= count) {
                continue;
            }
            const auto& entry = catalog.entries[idx];
            lv_label_set_text(label, entry.path.filename().c_str());
            lv_obj_set_style_bg_opa(label, idx == roles.focus() ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_color(label, lv_color_hex(0x156A77), 0);
            lv_obj_set_style_text_color(label, lv_color_hex(entry.status == amiibo::BinStatus::valid
                                                               ? 0xF5F7FC : 0xFFB58B), 0);
        }
        if (catalog.error && !feedback) {
            feedback = "Cannot scan role directory.";
        } else if (count == 0 && !feedback) {
            feedback = "No BIN files in the role directory.";
        } else if (!feedback) {
            feedback = catalog.entries[roles.focus()].status == amiibo::BinStatus::valid
                ? "Enter: use BIN and listen"
                : amiibo::describe(catalog.entries[roles.focus()].status);
        }
        lv_label_set_text(list_status_, feedback);
    }

    amiibo::NfcFlow flow_;
    std::array<lv_obj_t*, amiibo::RoleBrowser::page_size> rows_{};
    lv_obj_t* title_{};
    lv_obj_t* detail_{};
    lv_obj_t* footer_{};
    lv_obj_t* list_title_{};
    lv_obj_t* list_status_{};
    lv_obj_t* spinner_{};
    const char* footer_text_ = nullptr;
    std::string listening_status_;
};

lv_font_t* load_role_font()
{
    for (const char* path : {"/usr/share/APPLaunch/share/font/NotoSansCJK-Regular.ttc",
                             "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"}) {
        if (access(path, R_OK) == 0) {
            if (auto* font = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                                     15, LV_FREETYPE_FONT_STYLE_NORMAL)) {
                return font;
            }
        }
    }
    std::fputs("AmiiboZero: no readable CJK font; falling back to default font\n", stderr);
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    std::filesystem::path directory;
    if (argc == 3 && std::strcmp(argv[1], "--data-dir") == 0 && argv[2][0] != '\0') {
        directory = argv[2];
    } else if (argc == 1) {
        const auto* home = std::getenv("HOME");
        if (!home || home[0] != '/') {
            std::fputs("AmiiboZero: HOME is unavailable; specify --data-dir\n", stderr);
            return 2;
        }
        directory = std::filesystem::path(home) / "amiibo_sim";
    } else {
        std::fputs("Usage: amiibozero [--data-dir DIRECTORY]\n", stderr);
        return 2;
    }
    const int fd = open_keypad();
    if (fd < 0) {
        std::fputs("AmiiboZero: no readable keypad with Z/X/C/F/Esc/Enter/N\n", stderr);
        return 1;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    lv_init();
    lv_tick_set_cb([]() -> uint32_t {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            DeviceInput::Clock::now().time_since_epoch()).count());
    });

    auto* display = lv_linux_fbdev_create();
    if (!display || lv_linux_fbdev_set_file(display, "/dev/fb0") != LV_RESULT_OK) {
        std::fputs("AmiiboZero: cannot initialize /dev/fb0\n", stderr);
        lv_deinit();
        close(fd);
        return 1;
    }

    auto* role_font = load_role_font();
    amiibo::CapListener backend(directory);
    HomeScreen screen(std::move(directory), backend, role_font);
    DeviceInput input;
    bool resync = false;
    bool keypad_ok = true;
    while (!interrupted && !input.quitting()) {
        keypad_ok = read_keys(fd, input, resync, [&](char key) {
            if (!input.quitting() && !interrupted) {
                screen.key(key);
            }
        });
        if (!keypad_ok) {
            break;
        }
        input.tick(DeviceInput::Clock::now());
        screen.update(input);
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    screen.shutdown();
    lv_display_delete(display);
    if (role_font) {
        lv_freetype_font_delete(role_font);
    }
    lv_deinit();
    close(fd);
    return keypad_ok ? 0 : 1;
}
