// A Linux board's I2C buses: pods' transfers as I2C_RDWR messages on
// /dev/i2c-N (Linux only), and the addresses the kernel's drivers hold, read
// from sysfs.

#include "hal/linux_board.hpp"
#include "hal/hal_types.hpp"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#endif

namespace kube32::hal {

namespace {

// i2c-dev refuses longer messages
constexpr size_t kMaxMessage = 8192;

#if defined(__linux__)
// An I2C_RDWR failure as the transfer functions answer it
int32_t transfer_error(int err) {
    switch (err) {
        case ENXIO:
        case EREMOTEIO: return HAL_ERR_NODEV;   // no acknowledge
        case ETIMEDOUT: return HAL_ERR_TIMEDOUT;
        case EINVAL:
        case EOPNOTSUPP: return HAL_ERR_INVAL;
        default: return HAL_ERR_IO;
    }
}
#endif

}  // namespace

int i2c_adapter_number(const std::string& device) {
    std::string path = device;
    char resolved[PATH_MAX];
    if (::realpath(device.c_str(), resolved)) path = resolved;
    const size_t slash = path.find_last_of('/');
    const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    if (base.size() <= 4 || base.compare(0, 4, "i2c-") != 0 || base.size() > 9) return -1;
    for (size_t i = 4; i < base.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(base[i]))) return -1;
    }
    return std::atoi(base.c_str() + 4);
}

std::bitset<128> kernel_i2c_addresses(const std::string& sysfs, int adapter) {
    std::bitset<128> out;
    if (adapter < 0) return out;
    const std::string dir = sysfs + "/bus/i2c/devices";
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    // A client of a 7-bit address is <adapter>-00<hh>; 10-bit ones are <adapter>-a<hhh>
    const std::string prefix = std::to_string(adapter) + "-00";
    while (const dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.size() != prefix.size() + 2 || name.compare(0, prefix.size(), prefix) != 0) continue;
        const char hi = name[prefix.size()], lo = name[prefix.size() + 1];
        if (!std::isxdigit(static_cast<unsigned char>(hi)) || !std::isxdigit(static_cast<unsigned char>(lo))) continue;
        const unsigned long addr = std::strtoul(name.c_str() + prefix.size(), nullptr, 16);
        struct stat st;
        if (addr < 128 && ::stat((dir + "/" + name + "/driver").c_str(), &st) == 0) out.set(addr);
    }
    ::closedir(d);
    return out;
}

LinuxI2cAdapter::LinuxI2cAdapter(std::string device, int fd, std::bitset<128> reserved)
    : device_(std::move(device)), fd_(fd), reserved_(reserved) {}

LinuxI2cAdapter::~LinuxI2cAdapter() {
    if (fd_ >= 0) ::close(fd_);
}

int32_t LinuxI2cAdapter::write(uint8_t addr, const uint8_t* data, size_t len) {
    if (addr > 0x7f || len > kMaxMessage || (len > 0 && !data)) return HAL_ERR_INVAL;
    // The kernel does not check I2C_RDWR against its own clients
    if (reserved_.test(addr)) return HAL_ERR_PERM;
#if defined(__linux__)
    i2c_msg msg{};
    msg.addr = addr;
    msg.len = static_cast<uint16_t>(len);
    msg.buf = const_cast<uint8_t*>(data);
    i2c_rdwr_ioctl_data xfer{&msg, 1};
    if (::ioctl(fd_, I2C_RDWR, &xfer) < 0) return transfer_error(errno);
    return len == 0 ? HAL_OK : static_cast<int32_t>(len);
#else
    return HAL_ERR_NODEV;
#endif
}

int32_t LinuxI2cAdapter::read(uint8_t addr, uint8_t* buf, size_t len) {
    if (addr > 0x7f || len == 0 || len > kMaxMessage || !buf) return HAL_ERR_INVAL;
    if (reserved_.test(addr)) return HAL_ERR_PERM;
#if defined(__linux__)
    i2c_msg msg{};
    msg.addr = addr;
    msg.flags = I2C_M_RD;
    msg.len = static_cast<uint16_t>(len);
    msg.buf = buf;
    i2c_rdwr_ioctl_data xfer{&msg, 1};
    if (::ioctl(fd_, I2C_RDWR, &xfer) < 0) return transfer_error(errno);
    return static_cast<int32_t>(len);
#else
    return HAL_ERR_NODEV;
#endif
}

}  // namespace kube32::hal
