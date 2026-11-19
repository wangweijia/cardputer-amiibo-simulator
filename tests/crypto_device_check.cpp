// Explicit opt-in validation. Never run by CTest, and never prints private data.
#include "amiibo_crypto.h"
#include <algorithm>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    amiibo::AmiiboCrypto crypto;
    if (const auto* error = crypto.load(argv[1])) { std::cerr << error << '\n'; return 1; }
    const auto catalog = amiibo::scan_bins(argv[1]);
    if (catalog.error) { return 1; }
    unsigned verified = 0, failed = 0, identities = 0;
    for (const auto& entry : catalog.entries) {
        const auto source = amiibo::load_bin(entry.path);
        if (!source.image) { ++failed; continue; }
        amiibo::PlainImage plain{};
        if (!crypto.unpack(*source.image, plain)) { ++failed; continue; }
        ++verified;
        amiibo::Image previous = *source.image;
        for (unsigned n = 0; n < 64; ++n) {
            amiibo::Image generated{};
            if (crypto.new_identity(*source.image, generated) ||
                !crypto.unpack(generated, plain) || generated == previous ||
                !std::equal(source.image->begin() + 0x54, source.image->begin() + 0x5c, generated.begin() + 0x54)) {
                ++failed; break;
            }
            previous = generated;
            ++identities;
        }
    }
    std::cout << "roles=" << catalog.entries.size() << " verified=" << verified
              << " fresh_authenticated_identities=" << identities << " failures=" << failed << '\n';
    return failed || verified == 0 ? 1 : 0;
}
