// Crypto format adapted from socram8888/amiitool (MIT).
// (c) 2015-2017 Marcos Del Sol Vives; (c) 2016 javiMaD.
// See third_party/amiitool/LICENSE. OpenSSL implementation and strict file loading below.
#include "amiibo_crypto.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace amiibo {
namespace {
using Byte = std::uint8_t;

template<std::size_t N> struct Secret {
    std::array<Byte, N> bytes{};
    ~Secret() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};

bool hmac(const Byte* key, const Byte* data, std::size_t size, Byte* out)
{
    unsigned length = 0;
    return HMAC(EVP_sha256(), key, 16, data, size, out, &length) && length == 32;
}

bool derive(const Byte* master, const PlainImage& image, std::array<Byte, 48>& keys)
{
    Secret<64> seed;
    std::copy_n(image.begin() + 0x29, 2, seed.bytes.begin());
    std::copy_n(image.begin() + 0x1d4, 8, seed.bytes.begin() + 0x10);
    std::copy_n(image.begin() + 0x1d4, 8, seed.bytes.begin() + 0x18);
    std::copy_n(image.begin() + 0x1e8, 32, seed.bytes.begin() + 0x20);

    // Master key layout: HMAC[16], type[14], RFU[1], magic size[1], magic[16], xor[32].
    const auto* end = static_cast<const Byte*>(std::memchr(master + 16, 0, 14));
    if (!end || master[31] > 16) { return false; }
    Secret<96> input;
    std::size_t size = 2;
    const auto append = [&](const Byte* data, std::size_t count) {
        std::copy_n(data, count, input.bytes.begin() + size);
        size += count;
    };
    append(master + 16, static_cast<std::size_t>(end - master - 16) + 1);
    append(seed.bytes.data(), 16 - master[31]);
    append(master + 32, master[31]);
    append(seed.bytes.data() + 16, 16);
    for (std::size_t i = 0; i < 32; ++i) {
        input.bytes[size++] = seed.bytes[32 + i] ^ master[48 + i];
    }
    Secret<32> block;
    for (unsigned counter = 0; counter < 2; ++counter) {
        input.bytes[0] = 0;
        input.bytes[1] = static_cast<Byte>(counter);
        if (!hmac(master, input.bytes.data(), size, block.bytes.data())) { return false; }
        std::copy_n(block.bytes.begin(), counter == 0 ? 32 : 16, keys.begin() + 32 * counter);
    }
    return true;
}

bool cipher(const std::array<Byte, 48>& keys, const PlainImage& input, PlainImage& output)
{
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int length = 0;
    int final_length = 0;
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_ctr(), nullptr, keys.data(), keys.data() + 16) != 1 ||
        EVP_EncryptUpdate(ctx.get(), output.data() + 0x2c, &length, input.data() + 0x2c, 0x188) != 1 ||
        EVP_EncryptFinal_ex(ctx.get(), output.data() + 0x2c + length, &final_length) != 1 ||
        length + final_length != 0x188) { return false; }
    std::copy_n(input.begin(), 8, output.begin());
    std::copy_n(input.begin() + 0x28, 4, output.begin() + 0x28);
    std::copy_n(input.begin() + 0x1d4, 0x34, output.begin() + 0x1d4);
    return true;
}

void from_tag(const Image& tag, PlainImage& out)
{
    std::copy_n(tag.begin() + 0x008, 0x008, out.begin());
    std::copy_n(tag.begin() + 0x080, 0x020, out.begin() + 0x008);
    std::copy_n(tag.begin() + 0x010, 0x024, out.begin() + 0x028);
    std::copy_n(tag.begin() + 0x0a0, 0x168, out.begin() + 0x04c);
    std::copy_n(tag.begin() + 0x034, 0x020, out.begin() + 0x1b4);
    std::copy_n(tag.begin(), 0x008, out.begin() + 0x1d4);
    std::copy_n(tag.begin() + 0x054, 0x02c, out.begin() + 0x1dc);
}

void to_tag(const PlainImage& in, Image& tag)
{
    std::copy_n(in.begin(), 0x008, tag.begin() + 0x008);
    std::copy_n(in.begin() + 0x008, 0x020, tag.begin() + 0x080);
    std::copy_n(in.begin() + 0x028, 0x024, tag.begin() + 0x010);
    std::copy_n(in.begin() + 0x04c, 0x168, tag.begin() + 0x0a0);
    std::copy_n(in.begin() + 0x1b4, 0x020, tag.begin() + 0x034);
    std::copy_n(in.begin() + 0x1d4, 0x008, tag.begin());
    std::copy_n(in.begin() + 0x1dc, 0x02c, tag.begin() + 0x054);
}
} // namespace

AmiiboCrypto::~AmiiboCrypto() { OPENSSL_cleanse(keys_.data(), keys_.size()); }

bool AmiiboCrypto::set_keys(const RetailKeys& keys)
{
    ready_ = false;
    OPENSSL_cleanse(keys_.data(), keys_.size());
    for (const auto offset : {0, 80}) {
        if (keys[offset + 31] > 16 || !std::memchr(keys.data() + offset + 16, 0, 14)) { return false; }
    }
    keys_ = keys;
    return ready_ = true;
}

const char* AmiiboCrypto::load(const std::filesystem::path& directory)
{
    ready_ = false;
    OPENSSL_cleanse(keys_.data(), keys_.size());
    int fd = -1;
    for (const auto* name : {"key_retail.bin", "retail.bin"}) {
        fd = open((directory / name).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd >= 0 || errno != ENOENT) { break; }
    }
    if (fd < 0) { return "Cannot read retail key in role directory."; }
    Secret<160> input;
    struct stat info{};
    bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size == 160;
    std::size_t size = 0;
    while (valid && size < input.bytes.size()) {
        const auto count = read(fd, input.bytes.data() + size, input.bytes.size() - size);
        if (count > 0) { size += static_cast<std::size_t>(count); }
        else if (count < 0 && errno == EINTR) { continue; }
        else { valid = false; }
    }
    close(fd);
    if (!valid) { return "Retail key must be a regular 160-byte file."; }
    return set_keys(input.bytes) ? nullptr : "Invalid retail key structure.";
}

bool AmiiboCrypto::unpack(const Image& image, PlainImage& plain) const
{
    if (!ready_) { OPENSSL_cleanse(plain.data(), plain.size()); return false; }
    Secret<520> internal;
    Secret<48> data_keys, tag_keys;
    from_tag(image, internal.bytes);
    const bool ok = derive(keys_.data(), internal.bytes, data_keys.bytes) &&
        derive(keys_.data() + 80, internal.bytes, tag_keys.bytes) &&
        cipher(data_keys.bytes, internal.bytes, plain) &&
        hmac(tag_keys.bytes.data() + 32, plain.data() + 0x1d4, 0x34, plain.data() + 0x1b4) &&
        hmac(data_keys.bytes.data() + 32, plain.data() + 0x29, 0x1df, plain.data() + 8) &&
        CRYPTO_memcmp(plain.data() + 8, internal.bytes.data() + 8, 32) == 0 &&
        CRYPTO_memcmp(plain.data() + 0x1b4, internal.bytes.data() + 0x1b4, 32) == 0;
    if (!ok) { OPENSSL_cleanse(plain.data(), plain.size()); }
    return ok;
}

bool AmiiboCrypto::pack(const PlainImage& plain, Image& image) const
{
    if (!ready_) { return false; }
    Secret<520> signed_image, encrypted;
    Secret<48> data_keys, tag_keys;
    signed_image.bytes = plain;
    if (!derive(keys_.data(), plain, data_keys.bytes) ||
        !derive(keys_.data() + 80, plain, tag_keys.bytes) ||
        !hmac(tag_keys.bytes.data() + 32, plain.data() + 0x1d4, 0x34, signed_image.bytes.data() + 0x1b4) ||
        !hmac(data_keys.bytes.data() + 32, signed_image.bytes.data() + 0x29, 0x1df, signed_image.bytes.data() + 8) ||
        !cipher(data_keys.bytes, signed_image.bytes, encrypted.bytes)) { return false; }
    std::copy_n(signed_image.bytes.begin() + 8, 32, encrypted.bytes.begin() + 8);
    std::copy_n(signed_image.bytes.begin() + 0x1b4, 32, encrypted.bytes.begin() + 0x1b4);
    to_tag(encrypted.bytes, image);
    return true;
}

const char* AmiiboCrypto::rebind(const Image& source, const Uid& uid, Image& output) const
{
    Secret<520> plain, verified;
    if (!unpack(source, plain.bytes)) { return "Role HMAC check failed; cannot rotate UID."; }
    if (uid[0] != 4) { return "Invalid NXP UID."; }
    auto* tag_uid = plain.bytes.data() + 0x1d4;
    std::copy_n(uid.begin(), 3, tag_uid);
    tag_uid[3] = 0x88 ^ uid[0] ^ uid[1] ^ uid[2];
    std::copy_n(uid.begin() + 3, 4, tag_uid + 4);
    plain.bytes[0] = uid[3] ^ uid[4] ^ uid[5] ^ uid[6];
    Image result = source;
    // The two HMAC fields change; all application content and the new UID must match.
    if (!pack(plain.bytes, result) || !unpack(result, verified.bytes) ||
        !std::equal(plain.bytes.begin(), plain.bytes.begin() + 8, verified.bytes.begin()) ||
        !std::equal(plain.bytes.begin() + 0x28, plain.bytes.begin() + 0x1b4, verified.bytes.begin() + 0x28) ||
        !std::equal(plain.bytes.begin() + 0x1d4, plain.bytes.end(), verified.bytes.begin() + 0x1d4)) {
        return "UID re-encryption verification failed.";
    }
    result[532] = 0xaa ^ uid[1] ^ uid[3];
    result[533] = 0x55 ^ uid[2] ^ uid[4];
    result[534] = 0xaa ^ uid[3] ^ uid[5];
    result[535] = 0x55 ^ uid[4] ^ uid[6];
    result[536] = result[537] = 0x80;
    output = result;
    return nullptr;
}

const char* AmiiboCrypto::new_identity(const Image& source, Image& output) const
{
    Uid uid{4};
    if (RAND_bytes(uid.data() + 1, 6) != 1) { return "Cannot generate fresh UID."; }
    return rebind(source, uid, output);
}
} // namespace amiibo
