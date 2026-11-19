#include "amiibo_crypto.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <openssl/evp.h>

void check(bool ok, const char* message)
{
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

std::string sha256(const amiibo::Image& image)
{
    unsigned char digest[32];
    unsigned size = 0;
    check(EVP_Digest(image.data(), image.size(), digest, &size, EVP_sha256(), nullptr) == 1 && size == 32,
          "Cannot hash test vector");
    std::string hex;
    for (auto b : digest) { hex += "0123456789abcdef"[b >> 4]; hex += "0123456789abcdef"[b & 15]; }
    return hex;
}

int main()
{
    // Public synthetic keys, never real retail material. Expected hashes were
    // independently generated using Python hmac and the OpenSSL CLI AES primitive.
    amiibo::RetailKeys keys{};
    for (std::size_t i = 0; i < keys.size(); ++i) { keys[i] = (i * 13 + 7) & 255; }
    for (auto offset : {0, 80}) {
        std::fill_n(keys.begin() + offset + 16, 14, 0);
        const auto* name = offset == 0 ? "test data" : "test tag";
        std::memcpy(keys.data() + offset + 16, name, std::strlen(name));
        keys[offset + 31] = 16;
    }
    amiibo::AmiiboCrypto crypto;
    check(crypto.set_keys(keys), "Synthetic key setup failed");
    amiibo::PlainImage plain{};
    for (std::size_t i = 0; i < plain.size(); ++i) { plain[i] = (i * 7 + 11) & 255; }
    const std::array<std::uint8_t, 8> original_uid{4,0x11,0x22,0xbf,0x33,0x44,0x55,0x66};
    std::copy(original_uid.begin(), original_uid.end(), plain.begin() + 0x1d4);
    plain[0] = 0x44;
    amiibo::Image original{};
    for (std::size_t i = 520; i < original.size(); ++i) { original[i] = (i * 3) & 255; }
    check(crypto.pack(plain, original), "Synthetic pack failed");
    check(sha256(original) == "3b1687a23389ef8344e548aad055055f38d87d96f48db85e31fdc20ce4324470",
          "Encryption differs from independent known vector");
    const auto untouched = original;
    amiibo::PlainImage unpacked{};
    check(crypto.unpack(original, unpacked), "Synthetic HMAC verification failed");
    check(std::equal(plain.begin() + 0x28, plain.begin() + 0x1b4, unpacked.begin() + 0x28),
          "Application content changed on unpack");
    amiibo::Image rotated{};
    const amiibo::Uid uid{4,0xa1,0xb2,0xc3,0xd4,0xe5,0xf6};
    check(!crypto.rebind(original, uid, rotated), "UID rebinding failed");
    check(sha256(rotated) == "364b0f3e9c2fc5559d318066d180a9af4c81b9b3122bdfd79c007ddd9bdc5de2",
          "UID re-encryption differs from independent known vector");
    check(original == untouched && crypto.unpack(rotated, unpacked), "Source changed or new identity invalid");
    auto damaged = rotated;
    damaged[0xa0] ^= 1;
    check(!crypto.unpack(damaged, unpacked), "Altered ciphertext passed HMAC verification");
    check(std::all_of(unpacked.begin(), unpacked.end(), [](auto b) { return b == 0; }),
          "Failed verification exposed plaintext");
    damaged = rotated;
    damaged[1] ^= 1;
    check(!crypto.unpack(damaged, unpacked), "UID-only edit passed verification");
    auto previous = rotated;
    for (int i = 0; i < 32; ++i) {
        check(!crypto.new_identity(original, rotated) && rotated != previous && crypto.unpack(rotated, unpacked),
              "Fresh identity generation or authentication failed");
        previous = rotated;
    }
    auto wrong_keys = keys;
    wrong_keys[0] ^= 1;
    check(crypto.set_keys(wrong_keys) && !crypto.unpack(original, unpacked), "Wrong key passed verification");
    wrong_keys[31] = 17;
    check(!crypto.set_keys(wrong_keys), "Oversized key magic accepted");
    wrong_keys = keys;
    std::fill_n(wrong_keys.begin() + 16, 14, 1);
    check(!crypto.set_keys(wrong_keys), "Unterminated key type accepted");

    auto directory = (std::filesystem::temp_directory_path() / "amiibozero-keys-XXXXXX").string();
    check(mkdtemp(directory.data()), "Cannot create key-loader fixture");
    struct Fixture {
        std::filesystem::path path;
        ~Fixture() { std::error_code error; std::filesystem::remove_all(path, error); }
    } fixture{directory};
    const auto write_key = [&](const char* name, std::size_t size) {
        std::ofstream file(fixture.path / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(keys.data()), size);
    };
    write_key("retail.bin", keys.size());
    check(!crypto.load(directory) && crypto.unpack(original, unpacked), "Retail key alias not loaded");
    write_key("key_retail.bin", 159);
    check(crypto.load(directory) && !crypto.unpack(original, unpacked), "Truncated key accepted");
    write_key("key_retail.bin", keys.size());
    check(!crypto.load(directory) && crypto.unpack(original, unpacked), "Preferred key not loaded");
    std::filesystem::remove(fixture.path / "key_retail.bin");
    std::filesystem::create_symlink(fixture.path / "retail.bin", fixture.path / "key_retail.bin");
    check(crypto.load(directory), "Symlink key unexpectedly followed");
}
