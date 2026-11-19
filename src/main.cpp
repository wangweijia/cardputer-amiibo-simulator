#include "bin_catalog.h"

#include <iostream>
#include <string_view>

int main(int argc, char** argv)
{
    if (argc == 1) {
        std::cout << "AmiiboZero: NFC emulation is not available.\n";
    } else if (argc == 3 && std::string_view(argv[1]) == "--list") {
        const auto catalog = amiibo::scan_bins(argv[2]);
        if (catalog.error) {
            std::cerr << "Cannot scan " << argv[2] << ": " << catalog.error.message() << '\n';
            return 1;
        }
        for (const auto& entry : catalog.entries) {
            std::cout << entry.path.filename() << ": " << amiibo::describe(entry.status) << '\n';
        }
        if (catalog.entries.empty()) {
            std::cout << "No role BIN files found.\n";
        }
    } else if (argc == 3 && std::string_view(argv[1]) == "--inspect") {
        const auto bin = amiibo::load_bin(argv[2]);
        if (!bin.image) {
            std::cerr << "BIN rejected: " << amiibo::describe(bin.status) << '\n';
            return 1;
        }
        std::cout << "Loaded a role BIN; basic checks passed. No NFC emulation started.\n";
    } else {
        std::cerr << "Usage: amiibozero [--list DIRECTORY | --inspect FILE]\n";
        return 2;
    }
}
